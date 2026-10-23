/* What the bridge publishes, read from ps5-exporter's /metrics (127.0.0.1:9100) and ps5-signal (127.0.0.1:9150). */
#ifndef SOURCES_H
#define SOURCES_H

#include <stdbool.h>
#include <stddef.h>

#define EXPORTER_PORT 9100
#define SIGNAL_PORT 9150

/* A value with have = false is published as null (HA shows it as unknown), never as 0. */
typedef struct {
	bool have;
	double value;
} reading;

typedef struct {
	bool exporter;          /* ps5-exporter answered */
	bool signal;            /* ps5-signal answered */
	reading temperature;    /* hottest sensor, C */
	reading power;          /* ps5_power_watts */
	reading gpu_power;      /* rail 0 */
	reading fan;            /* percent */
	reading cpu_load;       /* percent, average over the logical CPUs */
	reading gpu_clock;      /* MHz */
	reading fps;
	reading uptime;         /* seconds */
	reading free_space;     /* bytes free on /user */
	reading free_ext;       /* bytes free on the extended storage, /mnt/ext1 */
	reading free_usb;       /* bytes free on the first USB drive, /mnt/usb0 */
	bool game_running;
	char game_id[16];       /* title ID, "" when none */
	char game_name[128];    /* from ShadowMountPlus' list, "" when unknown */
	char model[64];
	char firmware[16];
	char exporter_version[16];
	char address[16];       /* the console's IPv4 address on the network, "" when unknown */
} readings;

/* ShadowMountPlus' current or last storage job (copy, move, unpack, delete), from its HTTP API on 127.0.0.1:10101. */
#define SHADOWMOUNT_PORT 10101

typedef struct {
	bool have;              /* ShadowMountPlus answered with a job */
	long long job_id;
	char state[16];         /* preparing ... completed, failed, cancelled */
	char operation[16];     /* copy, move, unpack, delete */
	char title_id[16];
	char destination[256];
	char error[128];        /* result_error, "" when none */
} storage_status;

/* A title ShadowMountPlus lists (games and homebrew; decision log D14). */
#define GAMES_MAX 96

typedef struct {
	char title_id[16];
	char name[96];
} game_entry;

/* Reads the subset of Prometheus text the bridge needs into out (pure; tested on the host). */
void sources_parse_metrics(const char* text, size_t length, readings* out);
/* Reads ShadowMountPlus' storage status answer into out (pure; tested on the host). */
void sources_parse_storage(const char* text, storage_status* out);
/* Asks both services. */
void sources_read(readings* out);
/* Asks ShadowMountPlus for its storage job; out->have is false when it does not answer or has had no job. */
void sources_read_storage(storage_status* out);
/* Reads ShadowMountPlus' game list answer, sorted by name (pure; tested on the host). The count, or -1 when it is
   not a game list. */
int sources_parse_games(const char* text, game_entry* out, int cap);
/* Asks ShadowMountPlus for its titles; -1 when it does not answer. */
int sources_read_games(game_entry* out, int cap);

#endif
