#include "sources.h"

#include "http.h"

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
					           label(labels, labels_len, "mountpoint", tmp, sizeof(tmp)) && strcmp(tmp, "/user") == 0) {
						set(&out->free_space, v);
					} else if (is(line, name_len, "ps5_game_running")) {
						out->game_running = v > 0.5;
					} else if (is(line, name_len, "ps5_game_info")) {
						label(labels, labels_len, "title_id", out->game_id, sizeof(out->game_id));
					} else if (is(line, name_len, "ps5_info")) {
						label(labels, labels_len, "model", out->model, sizeof(out->model));
						label(labels, labels_len, "firmware", out->firmware, sizeof(out->firmware));
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
