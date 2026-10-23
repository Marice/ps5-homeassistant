#include "launch.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

int launch_payload(const char* path, char* error, size_t error_size)
{
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		snprintf(error, error_size, "%s is not there", path);
		return -1;
	}
	if (st.st_size <= 0 || st.st_size > LAUNCH_MAX_ELF) {
		snprintf(error, error_size, "%s: unexpected size", path);
		return -1;
	}
	int file = open(path, O_RDONLY);
	unsigned char* elf = malloc((size_t)st.st_size);
	ssize_t done = 0;
	while (file >= 0 && elf && done < st.st_size) {
		ssize_t n = read(file, elf + done, (size_t)(st.st_size - done));
		if (n <= 0) break;
		done += n;
	}
	if (file >= 0) close(file);
	if (!elf || done != st.st_size || memcmp(elf, "\x7f" "ELF", 4) != 0) {
		free(elf);
		snprintf(error, error_size, "%s cannot be read as an ELF", path);
		return -1;
	}

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(LAUNCH_LOADER_PORT);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	struct timeval timeout = { 5, 0 };
	if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	int rc = fd >= 0 && connect(fd, (struct sockaddr*)&address, sizeof(address)) == 0 ? 0 : -1;
	for (ssize_t sent = 0; rc == 0 && sent < done;) {
		ssize_t n = send(fd, elf + sent, (size_t)(done - sent), 0);
		if (n <= 0) rc = -1;
		else sent += n;
	}
	if (fd >= 0) close(fd);
	free(elf);
	if (rc != 0) snprintf(error, error_size, "the ELF loader on port %d does not take it", LAUNCH_LOADER_PORT);
	return rc;
}

/* sceSystemServiceLaunchApp's third argument, as websrv declares it. */
typedef struct {
	uint32_t structsize;
	uint32_t user_id;
	uint32_t app_opt;
	uint64_t crash_report;
	uint32_t check_flag;
} launch_context;

typedef int (*user_init_fn)(void*);
typedef int (*foreground_user_fn)(uint32_t*);
typedef int (*launch_app_fn)(const char*, char**, launch_context*);

int launch_title(const char* title_id, int* code, char* error, size_t error_size)
{
	*code = 0;
	/* Looked up on first use, so a firmware without them refuses a start instead of stopping the payload. */
	static user_init_fn user_init;
	static foreground_user_fn foreground_user;
	static launch_app_fn launch_app;
	static int tried;
	if (!tried) {
		tried = 1;
		void* user = dlopen("libSceUserService.sprx", RTLD_LAZY);
		void* system = dlopen("libSceSystemService.sprx", RTLD_LAZY);
		if (user) {
			user_init = (user_init_fn)dlsym(user, "sceUserServiceInitialize");
			foreground_user = (foreground_user_fn)dlsym(user, "sceUserServiceGetForegroundUser");
		}
		if (system) launch_app = (launch_app_fn)dlsym(system, "sceSystemServiceLaunchApp");
		if (user_init) user_init(NULL); /* already initialised is fine */
	}
	if (!foreground_user || !launch_app) {
		snprintf(error, error_size, "this firmware does not offer starting a title");
		return -1;
	}
	launch_context context;
	memset(&context, 0, sizeof(context));
	int rc = foreground_user(&context.user_id);
	if (rc != 0) {
		snprintf(error, error_size, "no user in front of the console (0x%08x)", (unsigned)rc);
		return -1;
	}
	char* argv[] = { NULL };
	/* websrv reads any non-zero answer as a failure, yet a start on 13.60 gave one: only a negative value is an
	   error (decision log D14). */
	rc = launch_app(title_id, argv, &context);
	*code = rc;
	if (rc < 0) {
		snprintf(error, error_size, "%s did not start (0x%08x)", title_id, (unsigned)rc);
		return -1;
	}
	return 0;
}
