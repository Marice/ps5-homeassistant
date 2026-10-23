/* Live events for Home Assistant: a trophy earned, a download or install done, a game started or stopped, a
 * ShadowMountPlus storage job finished. Pure: it compares what it is given with what it saw before (tested on the
 * host).
 *
 * The first look after the payload starts is the baseline: it sends nothing, or every old trophy would fire. The
 * cursor lives as long as the payload, across MQTT sessions. */
#ifndef EVENTS_H
#define EVENTS_H

#include <stdbool.h>

#include "sources.h"

/* A row of the system's notification database (/system_data/priv/mms/notification2.db), the fields events use. */
typedef struct {
	long long id;
	long long updated_ms;  /* updated_date_time, ms since the epoch */
	char channel[24];      /* raw_data.channelType: Trophies, Downloads, ... */
	char use_case[16];     /* raw_data.useCaseId; IDC is a homebrew notification (ours too) */
	char message[128];     /* raw_data.viewData.message.body */
	char title_id[16];     /* the game, when the notification names one */
	char grade[16];        /* trophies: Bronze, Silver, Gold, Platinum */
	char trophy_name[128];
	char trophy_id[8];
	char np_comm_id[16];   /* the game's trophy set, NPWR..._00 */
} notification_row;

typedef struct {
	char object[16];   /* the event entity: trophy, download, game, storage */
	char payload[640]; /* the JSON for ps5/<id>/event/<object>, with event_type */
} console_event;

typedef struct {
	bool notifications_seen;
	long long last_id;
	long long last_updated_ms;
	bool game_seen;
	char game_id[16];
	char game_name[128];
	bool storage_seen;
	long long storage_job; /* the last job whose end was reported (or seen at the baseline) */
	char skipped[64];      /* the last notification skipped for an unknown trophy grade, for the local API */
} event_state;

/* Notifications older than this are not sent after a reconnect (no flood after a long outage). */
#define EVENTS_MAX_AGE_MS (10LL * 60 * 1000)

void events_init(event_state* s);

/* Rows newer than the cursor, in id order. now_ms is the wall clock (ms since the epoch). Returns the number of
   events written to out. */
int events_from_notifications(event_state* s, const notification_row* rows, int count, long long now_ms,
                              console_event* out, int cap);

/* A new reading of ps5-exporter: started and stopped. Nothing while ps5-exporter does not answer. */
int events_from_reading(event_state* s, const readings* r, console_event* out, int cap);

/* ShadowMountPlus' storage job: an event when a job reached completed, failed or cancelled. */
int events_from_storage(event_state* s, const storage_status* st, console_event* out, int cap);

#endif
