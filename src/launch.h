/* Starting things on the console: a payload through the ELF loader, a game or app through the system. On the console
 * only. */
#ifndef LAUNCH_H
#define LAUNCH_H

#include <stddef.h>

#define LAUNCH_LOADER_PORT 9021
#define LAUNCH_MAX_ELF (16L << 20)

/* Reads the ELF and sends it to the ELF loader on 127.0.0.1:9021. 0, or -1 with the reason in error. */
int launch_payload(const char* path, char* error, size_t error_size);

/* Starts a title for the user in front of the console, as websrv (ps5-payload-dev) does it:
   sceUserServiceGetForegroundUser, then sceSystemServiceLaunchApp. It does not close a running game: the caller
   refuses first (decision log D14). 0, or -1 with the reason in error. code is what sceSystemServiceLaunchApp
   answered (0 when it was not called): a start once succeeded with a non-zero answer, so a caller can look whether
   the game runs after all. */
int launch_title(const char* title_id, int* code, char* error, size_t error_size);

#endif
