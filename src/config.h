/* /data/ps5-homeassistant/config.json: where the broker is and how often to publish.
 *
 *   { "host": "192.168.1.10", "port": 1883, "username": "ps5", "password": "...", "interval": 10 }
 *
 * host is a name or an IPv4 address; port 1-65535; username up to 64 bytes and password up to 128 (both may
 * be empty, and may not hold a quote or backslash); interval 5-300 seconds. The password is stored as given: MQTT sends it as given too. */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#ifndef CONFIG_DIRECTORY /* the host tests point it elsewhere */
#define CONFIG_DIRECTORY "/data/ps5-homeassistant"
#endif
#define CONFIG_PATH CONFIG_DIRECTORY "/config.json"
#define CONFIG_MAX_SIZE 4096

typedef struct {
	char host[254];
	int port;
	char username[65];
	char password[129];
	int interval;
} config;

/* Defaults: no host (not configured), port 1883, interval 10. */
void config_defaults(config* out);
/* Checks text as a config. Fields left out keep the value in out (so a PUT may leave the password out).
   False with a reason in error when it isn't valid; out is then unchanged. */
bool config_parse(const char* text, size_t length, config* out, char* error, size_t error_size);
/* Reads config.json into out (defaults when there is none). False with the reason when it can't be read. */
bool config_load(config* out, char* error, size_t error_size);
/* Writes config.json through a temporary file. False with the reason when it fails. */
bool config_save(const config* c, char* error, size_t error_size);
/* The config as JSON, without the password: "password_set" says whether there is one. */
void config_public_json(const config* c, char* out, size_t cap);
bool config_ready(const config* c);

#endif
