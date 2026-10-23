#include "chain.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

void chain_key(const char* file, char* key, size_t cap)
{
	const char* base = strrchr(file, '/');
	base = base ? base + 1 : file;
	size_t n = strlen(base);
	if (n >= 4 && strcasecmp(base + n - 4, ".elf") == 0) n -= 4;
	size_t end = 0;
	for (; end < n; end++) {
		char c = base[end];
		if (c == '_') break;
		if (c == '-' && end + 1 < n &&
		    (isdigit((unsigned char)base[end + 1]) ||
		     ((base[end + 1] == 'v' || base[end + 1] == 'V') && end + 2 < n && isdigit((unsigned char)base[end + 2]))))
			break;
	}
	size_t k = 0;
	for (size_t i = 0; i < end && k + 1 < cap; i++) key[k++] = (char)tolower((unsigned char)base[i]);
	if (cap > 0) key[k] = 0;
}

/* Lower case, without .elf. */
static void process_stem(const char* name, char* out, size_t cap)
{
	size_t n = strlen(name);
	if (n >= 4 && strcasecmp(name + n - 4, ".elf") == 0) n -= 4;
	size_t k = 0;
	for (size_t i = 0; i < n && k + 1 < cap; i++) out[k++] = (char)tolower((unsigned char)name[i]);
	out[k] = 0;
}

/* The process name is the key, or starts with it followed by a separator ("etahen critical ser"). */
static bool belongs(const char* stem, const char* key)
{
	size_t n = strlen(key);
	if (n == 0 || strncmp(stem, key, n) != 0) return false;
	char next = stem[n];
	return next == 0 || next == ' ' || next == '_' || next == '-' || next == '.';
}

int chain_running(const chain_payload* p, const char (*processes)[20], int count)
{
	if (count <= 0) return -1;
	/* A process name has at most 19 characters: ps5-homeassistant names itself ps5-hass.elf. */
	const char* alias = strcmp(p->key, "ps5-homeassistant") == 0 ? "ps5-hass" : NULL;
	char stem[24];
	for (int i = 0; i < count; i++) {
		process_stem(processes[i], stem, sizeof(stem));
		if (belongs(stem, p->key) || (alias && belongs(stem, alias))) return 1;
	}
	return 0;
}

static bool starts_with(const char* text, const char* prefix)
{
	return strncmp(text, prefix, strlen(prefix)) == 0;
}

int chain_parse(const char* text, size_t length, const char* dir, chain_payload* out, int cap)
{
	int count = 0;
	size_t start = 0;
	while (start < length && count < cap) {
		size_t end = start;
		while (end < length && text[end] != '\n') end++;
		char line[CHAIN_PATH_MAX];
		size_t n = end - start < sizeof(line) - 1 ? end - start : sizeof(line) - 1;
		memcpy(line, text + start, n);
		line[n] = 0;
		start = end + 1;
		/* As ps5-autoloader reads a line: a comment ends it, then trailing \r, spaces and tabs go. */
		char* hash = strchr(line, '#');
		if (hash) *hash = 0;
		n = strlen(line);
		while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
		if (n == 0 || line[0] == '!' || line[0] == '@') continue;

		chain_payload* p = &out[count];
		memset(p, 0, sizeof(*p));
		if (line[0] == '/') snprintf(p->path, sizeof(p->path), "%s", line);
		else if (snprintf(p->path, sizeof(p->path), "%s/%s", dir, line) >= (int)sizeof(p->path)) continue;
		chain_key(line, p->key, sizeof(p->key));
		if (p->key[0] == 0) continue;
		bool seen = false;
		for (int i = 0; i < count; i++) seen |= strcmp(out[i].key, p->key) == 0;
		if (seen) continue;
		const char* base = strrchr(line, '/');
		base = base ? base + 1 : line;
		snprintf(p->name, sizeof(p->name), "%s", base);
		size_t len = strlen(p->name);
		if (len >= 4 && strcasecmp(p->name + len - 4, ".elf") == 0) p->name[len - 4] = 0;
		snprintf(p->object, sizeof(p->object), "payload_%s", p->key);
		for (char* c = p->object; *c; c++)
			if (!isalnum((unsigned char)*c) && *c != '_') *c = '_';
		p->can_start = !starts_with(p->key, "etahen") && !starts_with(p->key, "kstuff") &&
		               strcmp(p->key, "ps5-homeassistant") != 0;
		count++;
	}
	return count;
}

bool chain_locate(char* path, size_t path_cap, char* dir, size_t dir_cap)
{
	struct stat st;
	for (int i = 0; i <= 8; i++) {
		char folder[64];
		if (i < 8) snprintf(folder, sizeof(folder), "/mnt/usb%d/ps5_autoloader", i);
		else snprintf(folder, sizeof(folder), "/data/ps5_autoloader");
		snprintf(path, path_cap, "%s/autoload.txt", folder);
		if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
			snprintf(dir, dir_cap, "%s", folder);
			return true;
		}
	}
	path[0] = 0;
	dir[0] = 0;
	return false;
}
