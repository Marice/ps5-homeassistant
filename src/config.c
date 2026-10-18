#include "config.h"

#include "json.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void config_defaults(config* out)
{
	memset(out, 0, sizeof(*out));
	out->port = 1883;
	out->interval = 10;
}

bool config_ready(const config* c)
{
	return c->host[0] != 0;
}

static bool fail(char* error, size_t size, const char* text)
{
	snprintf(error, size, "%s", text);
	return false;
}

/* A host name or IPv4 address: letters, digits, dots and hyphens. */
static bool valid_host(const char* text, size_t len)
{
	if (len == 0 || len > 253) return false;
	for (size_t i = 0; i < len; i++) {
		char c = text[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
			return false;
	}
	return true;
}

/* No control characters (they would only end up in the MQTT packet), no quote or backslash: the JSON reader
   keeps escapes as written, so those could not be stored and read back unchanged. */
static bool printable(const char* text, size_t len)
{
	for (size_t i = 0; i < len; i++)
		if ((unsigned char)text[i] < 32 || text[i] == 127 || text[i] == '"' || text[i] == '\\') return false;
	return true;
}

static bool copy_string(const json_node* n, char* out, size_t cap)
{
	if (n->type != JSON_STRING || n->length >= cap) return false;
	memcpy(out, n->text, n->length);
	out[n->length] = 0;
	return true;
}

bool config_parse(const char* text, size_t length, config* out, char* error, size_t error_size)
{
	static json_document doc;
	if (!json_parse(&doc, text, length) || doc.nodes[0].type != JSON_OBJECT)
		return fail(error, error_size, "not a JSON object");
	config c = *out;
	const json_node* root = &doc.nodes[0];
	const json_node* n;
	if ((n = json_member(&doc, root, "host")) != NULL) {
		if (n->type != JSON_STRING || !valid_host(n->text, n->length) || !copy_string(n, c.host, sizeof(c.host)))
			return fail(error, error_size, "host: a name or IPv4 address");
	}
	if ((n = json_member(&doc, root, "port")) != NULL) {
		if (n->type != JSON_NUMBER || n->number < 1 || n->number > 65535)
			return fail(error, error_size, "port: 1-65535");
		c.port = (int)n->number;
	}
	if ((n = json_member(&doc, root, "username")) != NULL) {
		if (!copy_string(n, c.username, sizeof(c.username)) || !printable(c.username, strlen(c.username)))
			return fail(error, error_size, "username: text without quotes or backslashes, up to 64 bytes");
	}
	if ((n = json_member(&doc, root, "password")) != NULL) {
		if (!copy_string(n, c.password, sizeof(c.password)) || !printable(c.password, strlen(c.password)))
			return fail(error, error_size, "password: text without quotes or backslashes, up to 128 bytes");
	}
	if ((n = json_member(&doc, root, "interval")) != NULL) {
		if (n->type != JSON_NUMBER || n->number < 5 || n->number > 300)
			return fail(error, error_size, "interval: 5-300 seconds");
		c.interval = (int)n->number;
	}
	*out = c;
	return true;
}

bool config_load(config* out, char* error, size_t error_size)
{
	config_defaults(out);
	FILE* f = fopen(CONFIG_PATH, "r");
	if (f == NULL) {
		if (errno == ENOENT) return true;
		return fail(error, error_size, "config.json can't be read");
	}
	static char text[CONFIG_MAX_SIZE];
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	int too_big = !feof(f);
	fclose(f);
	if (too_big) return fail(error, error_size, "config.json is too large");
	text[n] = 0;
	return config_parse(text, n, out, error, error_size);
}

/* A JSON string with quotes and backslashes escaped (the fields hold no control characters). */
static void json_string(char* out, size_t cap, const char* text)
{
	size_t j = 0;
	if (cap < 3) return;
	out[j++] = '"';
	for (size_t i = 0; text[i] && j + 3 < cap; i++) {
		if (text[i] == '"' || text[i] == '\\') out[j++] = '\\';
		out[j++] = text[i];
	}
	out[j++] = '"';
	out[j] = 0;
}

bool config_save(const config* c, char* error, size_t error_size)
{
	char host[300], user[150], pass[280];
	json_string(host, sizeof(host), c->host);
	json_string(user, sizeof(user), c->username);
	json_string(pass, sizeof(pass), c->password);
	mkdir(CONFIG_DIRECTORY, 0755); /* readable: the notification icon sits here too (see main.c) */
	const char* temporary = CONFIG_PATH ".new";
	FILE* f = fopen(temporary, "w");
	if (f == NULL) return fail(error, error_size, "config.json can't be written");
	int written = fprintf(f, "{\n  \"host\": %s,\n  \"port\": %d,\n  \"username\": %s,\n  \"password\": %s,\n  \"interval\": %d\n}\n",
	                      host, c->port, user, pass, c->interval);
	int closed = fclose(f);
	if (written < 0 || closed != 0 || rename(temporary, CONFIG_PATH) != 0) {
		unlink(temporary);
		return fail(error, error_size, "config.json can't be written");
	}
	chmod(CONFIG_PATH, 0600);
	return true;
}

void config_public_json(const config* c, char* out, size_t cap)
{
	char host[300], user[150];
	json_string(host, sizeof(host), c->host);
	json_string(user, sizeof(user), c->username);
	snprintf(out, cap, "{\"host\":%s,\"port\":%d,\"username\":%s,\"password_set\":%s,\"interval\":%d}", host, c->port,
	         user, c->password[0] ? "true" : "false", c->interval);
}
