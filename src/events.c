#include "events.h"

#include <stdio.h>
#include <string.h>

#include "buf.h"

void events_init(event_state* s)
{
	memset(s, 0, sizeof(*s));
}

/* Text between JSON quotes: quotes and backslashes escaped, control characters dropped. */
static void add_text(Buf* b, const char* text)
{
	for (size_t i = 0; text[i]; i++) {
		unsigned char c = (unsigned char)text[i];
		if (c < 32) continue;
		if (c == '"' || c == '\\') buf_addf(b, "\\%c", c);
		else buf_addf(b, "%c", c);
	}
}

/* A "key":"text" member with a leading comma, or nothing when text is empty. */
static void add_member(Buf* b, const char* key, const char* text)
{
	if (!text[0]) return;
	buf_addf(b, ",\"%s\":\"", key);
	add_text(b, text);
	buf_addf(b, "\"");
}

/* Starts an event; the caller adds members and close_event() ends it. */
static void open_event(console_event* e, const char* object, const char* type, Buf* b)
{
	snprintf(e->object, sizeof(e->object), "%s", object);
	buf_init(b, e->payload, sizeof(e->payload));
	buf_addf(b, "{\"event_type\":\"%s\"", type);
}

/* Ends an event. A payload that did not fit is dropped rather than sent cut off. */
static int close_event(Buf* b)
{
	buf_addf(b, "}");
	return b->truncated ? 0 : 1;
}

static const char* trophy_type(const char* grade)
{
	static const char* const grades[][2] = {
		{ "Bronze", "bronze" }, { "Silver", "silver" }, { "Gold", "gold" }, { "Platinum", "platinum" },
	};
	for (size_t i = 0; i < sizeof(grades) / sizeof(grades[0]); i++)
		if (strcmp(grade, grades[i][0]) == 0) return grades[i][1];
	return NULL;
}

/* The download notifications seen on a console (decision log D9); anything else from Downloads is "other". */
static const char* download_type(const char* use_case)
{
	static const char* const cases[][2] = {
		{ "NUC240", "installed" }, { "NUC246", "ready" }, { "NUC547", "addon_ready" }, { "NUC237", "update_added" },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		if (strcmp(use_case, cases[i][0]) == 0) return cases[i][1];
	return "other";
}

/* The event for one notification row, or 0 when it is not one Home Assistant gets. */
static int notification_event(event_state* s, const notification_row* row, console_event* e)
{
	Buf b;
	if (strcmp(row->use_case, "IDC") == 0) return 0; /* homebrew, ours included: not an event of the console */
	if (strcmp(row->channel, "Trophies") == 0) {
		const char* type = trophy_type(row->grade);
		if (!type) {
			snprintf(s->skipped, sizeof(s->skipped), "trophy grade '%s'", row->grade);
			return 0;
		}
		open_event(e, "trophy", type, &b);
		add_member(&b, "trophy", row->trophy_name);
		add_member(&b, "trophy_id", row->trophy_id);
		add_member(&b, "trophy_set", row->np_comm_id);
		add_member(&b, "title_id", row->title_id);
		return close_event(&b);
	}
	if (strcmp(row->channel, "Downloads") == 0) {
		open_event(e, "download", download_type(row->use_case), &b);
		add_member(&b, "title_id", row->title_id);
		add_member(&b, "message", row->message);
		return close_event(&b);
	}
	return 0;
}

int events_from_notifications(event_state* s, const notification_row* rows, int count, long long now_ms,
                              console_event* out, int cap)
{
	int n = 0;
	for (int i = 0; i < count; i++) {
		const notification_row* row = &rows[i];
		if (row->id <= s->last_id && row->updated_ms <= s->last_updated_ms) continue;
		/* The baseline sends nothing; after it, a row older than EVENTS_MAX_AGE_MS is passed over. */
		bool send = s->notifications_seen && now_ms - row->updated_ms <= EVENTS_MAX_AGE_MS;
		if (row->id > s->last_id) s->last_id = row->id;
		if (row->updated_ms > s->last_updated_ms) s->last_updated_ms = row->updated_ms;
		if (send && n < cap) n += notification_event(s, row, &out[n]);
	}
	s->notifications_seen = true;
	return n;
}

static int game_event(const char* type, const char* id, const char* name, console_event* e)
{
	Buf b;
	open_event(e, "game", type, &b);
	add_member(&b, "title_id", id);
	add_member(&b, "game", name[0] ? name : id);
	return close_event(&b);
}

int events_from_reading(event_state* s, const readings* r, console_event* out, int cap)
{
	if (!r->exporter) return 0; /* no answer is not "stopped" */
	const char* id = r->game_running ? r->game_id : "";
	int n = 0;
	if (s->game_seen && strcmp(id, s->game_id) != 0) {
		if (s->game_id[0] && n < cap) n += game_event("stopped", s->game_id, s->game_name, &out[n]);
		if (id[0] && n < cap) n += game_event("started", id, r->game_name, &out[n]);
	}
	s->game_seen = true;
	snprintf(s->game_id, sizeof(s->game_id), "%s", id);
	snprintf(s->game_name, sizeof(s->game_name), "%s", r->game_running ? r->game_name : "");
	return n;
}

static bool finished(const char* state)
{
	return strcmp(state, "completed") == 0 || strcmp(state, "failed") == 0 || strcmp(state, "cancelled") == 0;
}

int events_from_storage(event_state* s, const storage_status* st, console_event* out, int cap)
{
	if (!st->have) return 0;
	if (!finished(st->state) || st->job_id == s->storage_job) {
		/* A job that runs at the baseline is reported when it ends; one that had ended already is not. */
		s->storage_seen = true;
		return 0;
	}
	bool send = s->storage_seen;
	s->storage_seen = true;
	s->storage_job = st->job_id;
	if (!send || cap < 1) return 0;
	Buf b;
	open_event(&out[0], "storage", st->state, &b);
	add_member(&b, "operation", st->operation);
	add_member(&b, "title_id", st->title_id);
	add_member(&b, "destination", st->destination);
	add_member(&b, "error", st->error);
	return close_event(&b);
}
