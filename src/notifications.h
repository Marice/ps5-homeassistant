/* The system's notification database, where trophies and downloads show up (decision log D9).
 *
 * The system writes it with SQLite (rollback journal). The payload never opens that file with SQLite and never
 * locks it: when its modification time or size changed, it copies the bytes into memory, checks that the file did
 * not change during the copy, and queries the copy (decision log D10). */
#ifndef NOTIFICATIONS_H
#define NOTIFICATIONS_H

#include <stdbool.h>
#include <stddef.h>

#include "events.h"

#define NOTIFICATIONS_PATH "/system_data/priv/mms/notification2.db"
#define NOTIFICATIONS_MAX_BYTES (4L << 20) /* 254 KB on a console in October 2026 */
#define NOTIFICATIONS_MAX_ROWS 32          /* per look; more new rows than this in 2 seconds are not expected */

typedef enum {
	NOTIFICATIONS_UNCHANGED, /* the file did not change, or changed during the copy (tried again next time) */
	NOTIFICATIONS_READ,      /* rows holds the rows newer than the cursor, in id order */
	NOTIFICATIONS_ERROR      /* error says why; tried again when the file changes */
} notifications_result;

typedef struct {
	const char* path;
	bool have_stamp;
	long long mtime_ns;
	long long size;
} notifications_reader;

void notifications_init(notifications_reader* r, const char* path);
/* Rows with an id above after_id or an update time above after_updated_ms (at most NOTIFICATIONS_MAX_ROWS, the newest
   ones), oldest first. */
notifications_result notifications_poll(notifications_reader* r, long long after_id, long long after_updated_ms,
                                        notification_row* rows, int* count, char* error, size_t error_size);

#endif
