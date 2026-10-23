#include "notifications.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../third_party/sqlite/sqlite3.h"

void notifications_init(notifications_reader* r, const char* path)
{
	memset(r, 0, sizeof(*r));
	r->path = path;
}

static long long stamp_ns(const struct stat* st)
{
#if defined(__APPLE__)
	return (long long)st->st_mtimespec.tv_sec * 1000000000LL + st->st_mtimespec.tv_nsec;
#else
	return (long long)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
#endif
}

static void column_text(sqlite3_stmt* q, int column, char* out, size_t cap)
{
	const unsigned char* text = sqlite3_column_text(q, column);
	snprintf(out, cap, "%s", text ? (const char*)text : "");
}

/* json_valid() first: one malformed row would otherwise fail the whole query. The title ID comes from the
   notification's parameters, else from its first npTitleIds entry without the _00 suffix. */
static const char QUERY[] =
	"SELECT id, updated_date_time,"
	" json_extract(raw_data, '$.channelType'),"
	" json_extract(raw_data, '$.useCaseId'),"
	" json_extract(raw_data, '$.viewData.message.body'),"
	" coalesce(json_extract(raw_data, '$.viewData.parameters.titleId'),"
	"          substr(json_extract(raw_data, '$.viewData.icon.parameters.npTitleIds[0]'), 1, 9)),"
	" json_extract(raw_data, '$.telemetryData.parameters.trophyGrade'),"
	" json_extract(raw_data, '$.telemetryData.parameters.trophyName'),"
	" json_extract(raw_data, '$.telemetryData.parameters.trophyId'),"
	" json_extract(raw_data, '$.telemetryData.parameters.trophyNpCommId')"
	" FROM `notification`"
	" WHERE (id > ?1 OR updated_date_time > ?2) AND json_valid(raw_data)"
	" ORDER BY id DESC LIMIT ?3";

/* Queries the copy in memory. buffer is SQLite's (sqlite3_malloc64) and freed with the database. */
static bool query(unsigned char* buffer, long long size, long long after_id, long long after_updated_ms,
                  notification_row* rows, int* count, char* error, size_t error_size)
{
	sqlite3* db = NULL;
	sqlite3_stmt* q = NULL;
	bool ok = false;
	if (sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
		sqlite3_free(buffer);
		snprintf(error, error_size, "sqlite: cannot open a database in memory");
		sqlite3_close(db);
		return false;
	}
	if (sqlite3_deserialize(db, "main", buffer, size, size,
	                        SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_READONLY) != SQLITE_OK ||
	    sqlite3_prepare_v2(db, QUERY, -1, &q, NULL) != SQLITE_OK) {
		snprintf(error, error_size, "sqlite: %s", sqlite3_errmsg(db));
		goto done;
	}
	sqlite3_bind_int64(q, 1, after_id);
	sqlite3_bind_int64(q, 2, after_updated_ms);
	sqlite3_bind_int(q, 3, NOTIFICATIONS_MAX_ROWS);
	int n = 0, rc;
	while ((rc = sqlite3_step(q)) == SQLITE_ROW && n < NOTIFICATIONS_MAX_ROWS) {
		notification_row* row = &rows[n++];
		memset(row, 0, sizeof(*row));
		row->id = sqlite3_column_int64(q, 0);
		row->updated_ms = sqlite3_column_int64(q, 1);
		column_text(q, 2, row->channel, sizeof(row->channel));
		column_text(q, 3, row->use_case, sizeof(row->use_case));
		column_text(q, 4, row->message, sizeof(row->message));
		column_text(q, 5, row->title_id, sizeof(row->title_id));
		column_text(q, 6, row->grade, sizeof(row->grade));
		column_text(q, 7, row->trophy_name, sizeof(row->trophy_name));
		column_text(q, 8, row->trophy_id, sizeof(row->trophy_id));
		column_text(q, 9, row->np_comm_id, sizeof(row->np_comm_id));
	}
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		snprintf(error, error_size, "sqlite: %s", sqlite3_errmsg(db));
		goto done;
	}
	/* Newest first from the query; oldest first for the caller. */
	for (int i = 0, j = n - 1; i < j; i++, j--) {
		notification_row swap = rows[i];
		rows[i] = rows[j];
		rows[j] = swap;
	}
	*count = n;
	ok = true;
done:
	sqlite3_finalize(q);
	sqlite3_close(db);
	return ok;
}

/* Remembers the file as it is, so the same problem is not tried again until the file changes. */
static notifications_result remember_error(notifications_reader* r, const struct stat* st)
{
	r->have_stamp = true;
	r->mtime_ns = stamp_ns(st);
	r->size = (long long)st->st_size;
	return NOTIFICATIONS_ERROR;
}

notifications_result notifications_poll(notifications_reader* r, long long after_id, long long after_updated_ms,
                                        notification_row* rows, int* count, char* error, size_t error_size)
{
	*count = 0;
	error[0] = 0;
	struct stat before, after;
	if (stat(r->path, &before) != 0) {
		snprintf(error, error_size, "cannot read %s", r->path);
		return NOTIFICATIONS_ERROR;
	}
	if (r->have_stamp && r->mtime_ns == stamp_ns(&before) && r->size == (long long)before.st_size)
		return NOTIFICATIONS_UNCHANGED;
	if (before.st_size <= 0 || before.st_size > NOTIFICATIONS_MAX_BYTES) {
		snprintf(error, error_size, "%s: unexpected size %lld", r->path, (long long)before.st_size);
		return remember_error(r, &before);
	}

	int fd = open(r->path, O_RDONLY);
	if (fd < 0) {
		snprintf(error, error_size, "cannot open %s", r->path);
		return remember_error(r, &before);
	}
	long long size = (long long)before.st_size;
	unsigned char* buffer = sqlite3_malloc64((sqlite3_uint64)size);
	long long done = 0;
	while (buffer && done < size) {
		ssize_t n = read(fd, buffer + done, (size_t)(size - done));
		if (n <= 0) break;
		done += n;
	}
	bool stable = buffer && done == size && fstat(fd, &after) == 0 && stamp_ns(&after) == stamp_ns(&before) &&
	              after.st_size == before.st_size;
	close(fd);
	if (!stable) {
		/* Written during the copy (or no memory): leave the stamp, so the next look tries again. */
		sqlite3_free(buffer);
		return NOTIFICATIONS_UNCHANGED;
	}
	bool ok = query(buffer, size, after_id, after_updated_ms, rows, count, error, error_size);
	r->have_stamp = true;
	r->mtime_ns = stamp_ns(&before);
	r->size = size;
	return ok ? NOTIFICATIONS_READ : NOTIFICATIONS_ERROR;
}
