/* HTTP/1.0 on loopback: the local API server (127.0.0.1 only) and a client for ps5-exporter and ps5-signal. */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/* Fills body (NUL-terminated) and returns the HTTP status. request_body is NUL-terminated, "" when there is none. */
typedef int (*http_handler)(const char* method, const char* path, const char* request_body, char* body, size_t cap);

/* Serves on 127.0.0.1:port (any address: also the network, for the development build) until *stop is set
   (checked between requests and once a second). Returns 0 then, or -1 when the socket can't be opened or becomes
   unusable (rest mode), so the caller can open it again. */
int http_serve(int port, int any_address, http_handler handler, volatile int* stop);

/* Sends a request to http://127.0.0.1:port/path (body may be NULL) and stores the response body in out.
   Returns the HTTP status, or a negative value when the service does not answer. */
int http_request(int port, const char* method, const char* path, const char* body, char* out, size_t out_cap,
                 int timeout_ms);

/* Fetches http://host:port/path into a file (written through a temporary file). 0, or -1 when it fails or the
   answer is not 200 or larger than max_bytes. */
int http_download(const char* host, int port, const char* path, const char* file, size_t max_bytes, int timeout_ms);

#endif
