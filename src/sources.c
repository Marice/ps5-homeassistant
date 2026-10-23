#include "sources.h"

#include "http.h"
#include "../third_party/sqlite/sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The value of key="..." inside a label set, into out; false when absent. Escaped quotes stay escaped. */
static bool label(const char* labels, size_t len, const char* key, char* out, size_t cap)
{
	size_t key_len = strlen(key);
	for (size_t i = 0; i + key_len + 2 <= len; i++) {
		if ((i == 0 || labels[i - 1] == ',' || labels[i - 1] == '{') && strncmp(labels + i, key, key_len) == 0 &&
		    labels[i + key_len] == '=' && labels[i + key_len + 1] == '"') {
			size_t start = i + key_len + 2, j = start;
			while (j < len && !(labels[j] == '"' && labels[j - 1] != '\\')) j++;
			if (j >= len) return false;
			size_t n = j - start < cap - 1 ? j - start : cap - 1;
			memcpy(out, labels + start, n);
			out[n] = 0;
			return true;
		}
	}
	return false;
}

static bool is(const char* name, size_t len, const char* want)
{
	return strlen(want) == len && strncmp(name, want, len) == 0;
}

/* Four numbers 0-255 with dots between them. */
static bool ipv4(const char* text)
{
	int parts = 0;
	const char* p = text;
	while (*p) {
		int value = 0, digits = 0;
		while (*p >= '0' && *p <= '9' && digits < 4) {
			value = value * 10 + (*p++ - '0');
			digits++;
		}
		if (digits == 0 || digits > 3 || value > 255) return false;
		parts++;
		if (*p == '.') p++;
		else if (*p) return false;
	}
	return parts == 4 && p > text && p[-1] != '.';
}

static void set(reading* r, double v)
{
	r->have = true;
	r->value = v;
}

void sources_parse_metrics(const char* text, size_t length, readings* out)
{
	double cpu_sum = 0.0;
	int cpu_count = 0;
	const char* end = text + length;
	/* The game's name sits on another line than its ID, so a second pass finds it. */
	for (int pass = 0; pass < 2; pass++) {
		const char* line = text;
		while (line < end) {
			const char* next = memchr(line, '\n', (size_t)(end - line));
			if (next == NULL) next = end;
			if (*line != '#' && next > line) {
				const char* p = line;
				while (p < next && *p != '{' && *p != ' ') p++;
				size_t name_len = (size_t)(p - line);
				const char* labels = "";
				size_t labels_len = 0;
				if (p < next && *p == '{') {
					labels = p;
					while (p < next && *p != '}') p++;
					labels_len = (size_t)(p - labels);
					if (p < next) p++;
				}
				char number[48];
				size_t n = (size_t)(next - p);
				if (n >= sizeof(number)) n = sizeof(number) - 1;
				memcpy(number, p, n);
				number[n] = 0;
				char* stop = NULL;
				double v = strtod(number, &stop);
				bool valid = stop != number;
				char tmp[160];
				if (pass == 0 && valid) {
					if (is(line, name_len, "ps5_temperature_celsius")) {
						if (!out->temperature.have || v > out->temperature.value) set(&out->temperature, v);
					} else if (is(line, name_len, "ps5_power_watts")) {
						set(&out->power, v);
					} else if (is(line, name_len, "ps5_power_rail_watts") && label(labels, labels_len, "rail", tmp, sizeof(tmp)) &&
					           strcmp(tmp, "0") == 0) {
						set(&out->gpu_power, v);
					} else if (is(line, name_len, "ps5_fan_duty_ratio")) {
						set(&out->fan, v * 100.0);
					} else if (is(line, name_len, "ps5_cpu_usage_ratio")) {
						cpu_sum += v;
						cpu_count++;
					} else if (is(line, name_len, "ps5_soc_clock_hertz") && label(labels, labels_len, "name", tmp, sizeof(tmp)) &&
					           strcmp(tmp, "gpu") == 0) {
						set(&out->gpu_clock, v / 1e6);
					} else if (is(line, name_len, "ps5_fps")) {
						set(&out->fps, v);
					} else if (is(line, name_len, "ps5_uptime_seconds")) {
						set(&out->uptime, v);
					} else if (is(line, name_len, "ps5_filesystem_avail_bytes") &&
					           label(labels, labels_len, "mountpoint", tmp, sizeof(tmp))) {
						if (strcmp(tmp, "/user") == 0) set(&out->free_space, v);
						else if (strcmp(tmp, "/mnt/ext1") == 0) set(&out->free_ext, v);
						else if (strcmp(tmp, "/mnt/usb0") == 0) set(&out->free_usb, v);
					} else if (is(line, name_len, "ps5_game_running")) {
						out->game_running = v > 0.5;
					} else if (is(line, name_len, "ps5_game_info")) {
						label(labels, labels_len, "title_id", out->game_id, sizeof(out->game_id));
					} else if (is(line, name_len, "ps5_info")) {
						label(labels, labels_len, "model", out->model, sizeof(out->model));
						label(labels, labels_len, "firmware", out->firmware, sizeof(out->firmware));
					} else if (is(line, name_len, "ps5_network_info") && out->address[0] == 0 &&
					           label(labels, labels_len, "address", tmp, sizeof(tmp)) && ipv4(tmp) &&
					           strncmp(tmp, "127.", 4) != 0) {
						memcpy(out->address, tmp, strlen(tmp) + 1); /* ipv4() allows 15 characters at most */
					} else if (is(line, name_len, "ps5_exporter_build_info")) {
						label(labels, labels_len, "version", out->exporter_version, sizeof(out->exporter_version));
					}
				} else if (pass == 1 && out->game_id[0] && is(line, name_len, "ps5_shadowmount_game_info") &&
				           label(labels, labels_len, "title_id", tmp, sizeof(tmp)) && strcmp(tmp, out->game_id) == 0) {
					label(labels, labels_len, "name", out->game_name, sizeof(out->game_name));
				}
			}
			line = next + 1;
		}
		if (out->game_id[0] == 0) break;
	}
	if (cpu_count > 0) set(&out->cpu_load, cpu_sum / cpu_count * 100.0);
	if (!out->game_running) {
		out->game_id[0] = 0;
		out->game_name[0] = 0;
	}
}

void sources_read(readings* out)
{
	memset(out, 0, sizeof(*out));
	static char metrics[256 * 1024];
	int status = http_request(EXPORTER_PORT, "GET", "/metrics", NULL, metrics, sizeof(metrics), 3000);
	if (status == 200) {
		out->exporter = true;
		sources_parse_metrics(metrics, strlen(metrics), out);
	}
	char answer[2048];
	out->signal = http_request(SIGNAL_PORT, "GET", "/status", NULL, answer, sizeof(answer), 1000) == 200;
}

/* ShadowMountPlus' answer holds fractions (progress_percent), which the small JSON reader refuses on purpose; the
   storage status is read with SQLite's JSON functions, already in the payload for the notification database. */
static const char STORAGE_QUERY[] =
	"SELECT json_extract(?1, '$.status'), json_extract(?1, '$.job_id'), json_extract(?1, '$.state'),"
	" json_extract(?1, '$.operation'), json_extract(?1, '$.title_id'), json_extract(?1, '$.destination'),"
	" json_extract(?1, '$.result_error')"
	" WHERE json_valid(?1)";

static void column_text(sqlite3_stmt* q, int column, char* out, size_t cap)
{
	const unsigned char* text = sqlite3_column_text(q, column);
	snprintf(out, cap, "%s", text ? (const char*)text : "");
}

void sources_parse_storage(const char* text, storage_status* out)
{
	memset(out, 0, sizeof(*out));
	sqlite3* db = NULL;
	sqlite3_stmt* q = NULL;
	if (sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK &&
	    sqlite3_prepare_v2(db, STORAGE_QUERY, -1, &q, NULL) == SQLITE_OK) {
		sqlite3_bind_text(q, 1, text, -1, SQLITE_STATIC);
		/* status 0 and a job id: the answer describes a job (none yet gives an error status) */
		if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_type(q, 0) == SQLITE_INTEGER &&
		    sqlite3_column_int64(q, 0) == 0 && sqlite3_column_type(q, 1) == SQLITE_INTEGER) {
			out->have = true;
			out->job_id = sqlite3_column_int64(q, 1);
			column_text(q, 2, out->state, sizeof(out->state));
			column_text(q, 3, out->operation, sizeof(out->operation));
			column_text(q, 4, out->title_id, sizeof(out->title_id));
			column_text(q, 5, out->destination, sizeof(out->destination));
			column_text(q, 6, out->error, sizeof(out->error));
		}
	}
	sqlite3_finalize(q);
	sqlite3_close(db);
}

void sources_read_storage(storage_status* out)
{
	static char answer[8192];
	memset(out, 0, sizeof(*out));
	if (http_request(SHADOWMOUNT_PORT, "POST", "/api/v1/games/storage/status", "{}", answer, sizeof(answer), 1000) ==
	    200)
		sources_parse_storage(answer, out);
}

/* Every title with an ID; a missing name becomes the ID. Sorted by name, case-insensitively. */
static const char GAMES_QUERY[] =
	"SELECT json_extract(value, '$.title_id') AS id,"
	" coalesce(json_extract(value, '$.title_name'), json_extract(value, '$.title_id')) AS name"
	" FROM json_each(?1, '$.games')"
	" WHERE json_extract(?1, '$.status') = 0 AND json_type(value, '$.title_id') = 'text'"
	" ORDER BY lower(name), id LIMIT ?2";

int sources_parse_games(const char* text, game_entry* out, int cap)
{
	sqlite3* db = NULL;
	sqlite3_stmt* q = NULL;
	int count = -1;
	if (sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK &&
	    sqlite3_prepare_v2(db, GAMES_QUERY, -1, &q, NULL) == SQLITE_OK) {
		sqlite3_bind_text(q, 1, text, -1, SQLITE_STATIC);
		sqlite3_bind_int(q, 2, cap);
		int rc;
		count = 0;
		while ((rc = sqlite3_step(q)) == SQLITE_ROW && count < cap) {
			column_text(q, 0, out[count].title_id, sizeof(out[count].title_id));
			column_text(q, 1, out[count].name, sizeof(out[count].name));
			count++;
		}
		if (rc != SQLITE_ROW && rc != SQLITE_DONE) count = -1;
		/* No rows can also be a refused or broken answer: only a real list counts. */
		if (count == 0) {
			sqlite3_stmt* check = NULL;
			if (sqlite3_prepare_v2(db, "SELECT json_valid(?1) AND json_extract(?1, '$.status') = 0", -1, &check, NULL) ==
			        SQLITE_OK) {
				sqlite3_bind_text(check, 1, text, -1, SQLITE_STATIC);
				if (sqlite3_step(check) != SQLITE_ROW || sqlite3_column_int(check, 0) != 1) count = -1;
			}
			sqlite3_finalize(check);
		}
	}
	sqlite3_finalize(q);
	sqlite3_close(db);
	return count;
}

int sources_read_games(game_entry* out, int cap)
{
	static char answer[512 * 1024];
	if (http_request(SHADOWMOUNT_PORT, "POST", "/api/v1/games", "{\"include_size\":false}", answer, sizeof(answer),
	                 3000) != 200)
		return -1;
	return sources_parse_games(answer, out, cap);
}
