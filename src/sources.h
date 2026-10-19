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
	bool game_running;
	char game_id[16];       /* title ID, "" when none */
	char game_name[128];    /* from ShadowMountPlus' list, "" when unknown */
	char model[64];
	char firmware[16];
	char exporter_version[16];
	char address[16];       /* the console's IPv4 address on the network, "" when unknown */
} readings;

/* Reads the subset of Prometheus text the bridge needs into out (pure; tested on the host). */
void sources_parse_metrics(const char* text, size_t length, readings* out);
/* Asks both services. */
void sources_read(readings* out);

#endif
