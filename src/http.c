/* HTTP/1.0 on loopback, after ps5-exporter's http.c: deadlines for a whole request and response, so a slow
   client can't hold the single-threaded server. */
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define REQUEST_DEADLINE_MS 5000
#define RESPONSE_DEADLINE_MS 5000
#define SOCKET_TIMEOUT_MS 2000
#define MAX_REQUEST 8192

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_timeout(int fd, int ms)
{
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int send_all(int fd, const char* data, size_t len, long long deadline)
{
	while (len > 0) {
		if (now_ms() > deadline) return -1;
		ssize_t n = send(fd, data, len, 0);
		if (n <= 0) {
			if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
			return -1;
		}
		data += n;
		len -= (size_t)n;
	}
	return 0;
}

static const char* reason(int code)
{
	switch (code) {
	case 200: return "OK";
	case 400: return "Bad Request";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 413: return "Payload Too Large";
	default: return "Internal Server Error";
	}
}

/* Reads one request: the head, then Content-Length bytes of body. 0, or the status to answer. */
static int read_request(int fd, char* req, size_t cap, char** body)
{
	long long deadline = now_ms() + REQUEST_DEADLINE_MS;
	size_t got = 0;
	char* end = NULL;
	while (end == NULL) {
		if (got + 1 >= cap) return 413;
		if (now_ms() > deadline) return 400;
		ssize_t n = recv(fd, req + got, cap - 1 - got, 0);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) continue;
			return 400;
		}
		got += (size_t)n;
		req[got] = 0;
		end = strstr(req, "\r\n\r\n");
	}
	*body = end + 4;
	size_t want = 0;
	for (char* line = strstr(req, "\r\n"); line && line < end; line = strstr(line + 2, "\r\n")) {
		if (strncasecmp(line + 2, "Content-Length:", 15) == 0) want = strtoul(line + 17, NULL, 10);
	}
	size_t head = (size_t)(*body - req);
	if (head + want + 1 > cap) return 413;
	while (got < head + want) {
		if (now_ms() > deadline) return 400;
		ssize_t n = recv(fd, req + got, head + want - got, 0);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) continue;
			return 400;
		}
		got += (size_t)n;
	}
	req[head + want] = 0;
	return 0;
}

int http_serve(int port, int any_address, http_handler handler, volatile int* stop)
{
	int server = socket(AF_INET, SOCK_STREAM, 0);
	if (server < 0) return -1;
	int one = 1;
	setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(any_address ? INADDR_ANY : INADDR_LOOPBACK);
	addr.sin_port = htons((uint16_t)port);
	if (bind(server, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(server, 4) < 0) {
		close(server);
		return -1;
	}
	static char req[MAX_REQUEST];
	static char body[16 * 1024];
	static char head[256];
	int failures = 0;
	while (!*stop) {
		struct pollfd p = { .fd = server, .events = POLLIN, .revents = 0 };
		if (poll(&p, 1, 1000) <= 0) continue;
		int fd = accept(server, NULL, NULL);
		if (fd < 0) {
			if (errno == EINTR || errno == EAGAIN || ++failures < 20) continue;
			close(server);
			return -1; /* rest mode closed it: open a new one */
		}
		failures = 0;
		set_timeout(fd, SOCKET_TIMEOUT_MS);
		char* request_body = NULL;
		int code = read_request(fd, req, sizeof(req), &request_body);
		if (code != 0) {
			snprintf(body, sizeof(body), "{\"error\":\"%s\"}", code == 413 ? "the request is too large" : "bad request");
		} else {
			char method[8] = { 0 }, path[128] = { 0 };
			if (sscanf(req, "%7s %127s", method, path) != 2) {
				code = 400;
				snprintf(body, sizeof(body), "{\"error\":\"bad request\"}");
			} else {
				code = handler(method, path, request_body, body, sizeof(body));
			}
		}
		size_t len = strlen(body);
		int hlen = snprintf(head, sizeof(head),
		                    "HTTP/1.0 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
		                    code, reason(code), len);
		long long deadline = now_ms() + RESPONSE_DEADLINE_MS;
		if (hlen > 0 && send_all(fd, head, (size_t)hlen, deadline) == 0) send_all(fd, body, len, deadline);
		close(fd);
	}
	close(server);
	return 0;
}

int http_request(int port, const char* method, const char* path, const char* body, char* out, size_t out_cap,
                 int timeout_ms)
{
	if (out_cap == 0) return -1;
	out[0] = 0;
	long long deadline = now_ms() + timeout_ms;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	set_timeout(fd, timeout_ms < SOCKET_TIMEOUT_MS ? timeout_ms : SOCKET_TIMEOUT_MS);
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -2;
	}
	char head[512];
	size_t body_len = body ? strlen(body) : 0;
	int hlen = snprintf(head, sizeof(head),
	                    "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
	                    method, path, body_len);
	if (hlen <= 0 || (size_t)hlen >= sizeof(head) || send_all(fd, head, (size_t)hlen, deadline) < 0 ||
	    (body_len && send_all(fd, body, body_len, deadline) < 0)) {
		close(fd);
		return -3;
	}
	size_t got = 0;
	while (got + 1 < out_cap && now_ms() <= deadline) {
		ssize_t n = recv(fd, out + got, out_cap - 1 - got, 0);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) continue;
			break;
		}
		got += (size_t)n;
	}
	close(fd);
	out[got] = 0;
	int status = -4;
	if (got > 12 && strncmp(out, "HTTP/", 5) == 0) status = atoi(out + 9);
	char* sep = strstr(out, "\r\n\r\n");
	if (sep) memmove(out, sep + 4, strlen(sep + 4) + 1);
	else out[0] = 0;
	return status;
}

int http_download(const char* host, int port, const char* path, const char* file, size_t max_bytes, int timeout_ms)
{
	char service[8];
	snprintf(service, sizeof(service), "%d", port);
	struct addrinfo hints, *list = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, service, &hints, &list) != 0 || list == NULL) return -1;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0 || (set_timeout(fd, SOCKET_TIMEOUT_MS), connect(fd, list->ai_addr, list->ai_addrlen)) != 0) {
		if (fd >= 0) close(fd);
		freeaddrinfo(list);
		return -1;
	}
	freeaddrinfo(list);
	long long deadline = now_ms() + timeout_ms;
	char head[512];
	int hlen = snprintf(head, sizeof(head), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
	if (hlen <= 0 || (size_t)hlen >= sizeof(head) || send_all(fd, head, (size_t)hlen, deadline) != 0) {
		close(fd);
		return -1;
	}
	static char data[256 * 1024];
	size_t cap = max_bytes + 1024 < sizeof(data) ? max_bytes + 1024 : sizeof(data);
	size_t got = 0;
	while (got < cap && now_ms() <= deadline) {
		ssize_t n = recv(fd, data + got, cap - got, 0);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) continue;
			break;
		}
		got += (size_t)n;
	}
	close(fd);
	if (got >= cap || got < 12 || strncmp(data, "HTTP/", 5) != 0 || atoi(data + 9) != 200) return -1;
	char* body = NULL;
	for (size_t i = 0; i + 3 < got; i++) {
		if (memcmp(data + i, "\r\n\r\n", 4) == 0) {
			body = data + i + 4;
			break;
		}
	}
	if (body == NULL) return -1;
	size_t len = got - (size_t)(body - data);
	if (len == 0 || len > max_bytes) return -1;
	char temporary[300];
	snprintf(temporary, sizeof(temporary), "%s.new", file);
	FILE* f = fopen(temporary, "wb");
	if (f == NULL) return -1;
	size_t written = fwrite(body, 1, len, f);
	if (fclose(f) != 0 || written != len || rename(temporary, file) != 0) {
		unlink(temporary);
		return -1;
	}
	return 0;
}
