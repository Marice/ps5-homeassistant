/* The boot chain: the payloads ps5-autoloader (itsPLK) starts from its autoload.txt, and whether each one runs.
 *
 * File and process names differ ("shadowmountplus_1.7beta5.elf" runs as "shadowmountplus.elf", "etaHEN-13.60.elf"
 * as "etaHEN Critical ser"), so a file name is cut to a key and compared with the start of the process names.
 * Parsing and matching are pure (tested on the host); chain_locate() looks at the file system. */
#ifndef CHAIN_H
#define CHAIN_H

#include <stdbool.h>
#include <stddef.h>

#define CHAIN_MAX 16
#define CHAIN_PATH_MAX 256

typedef struct {
	char path[CHAIN_PATH_MAX]; /* the ELF, absolute */
	char key[32];              /* the file name cut: shadowmountplus, etahen, ps5-exporter */
	char object[40];           /* the entity's object id: payload_<key> with '-' as '_' */
	char name[64];             /* the file name without .elf, for the entity's name */
	bool can_start;            /* not etaHEN, kstuff or ps5-homeassistant itself (decision log D14) */
} chain_payload;

/* The payloads in an autoload.txt, in its order. Lines that are empty, comments (#), sleeps (!ms) or directives (@)
   are skipped; a relative line is a file in dir (the list's folder), as ps5-autoloader reads it. The same key twice
   counts once. Returns the number written to out. */
int chain_parse(const char* text, size_t length, const char* dir, chain_payload* out, int cap);

/* A file name cut to its key: lower case, without folder and .elf, up to the first '_' or a '-' followed by a digit
   or by 'v' and a digit. */
void chain_key(const char* file, char* key, size_t cap);

/* 1 when one of the process names belongs to the payload, 0 when none does, -1 when there are no names (unknown). */
int chain_running(const chain_payload* p, const char (*processes)[20], int count);

/* The autoload.txt ps5-autoloader uses, in its order: /mnt/usb0..7 first, then /data. Its folder goes to dir.
   False when there is none. */
bool chain_locate(char* path, size_t path_cap, char* dir, size_t dir_cap);

#endif
