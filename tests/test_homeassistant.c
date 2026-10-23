/* Host tests: MQTT packets, the metrics subset, the discovery messages, commands, the config and live events.
 * Built by `make test` with AddressSanitizer and UBSan; no console or broker needed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/buf.h"
#include "../src/config.h"
#include "../src/discovery.h"
#include "../src/events.h"
#include "../src/json.h"
#include "../src/mqtt.h"
#include "../src/notifications.h"
#include "../src/power.h"
#include "../src/sources.h"
#include "../third_party/sqlite/sqlite3.h"

#include <unistd.h>

static int g_failures;

static void ok(const char* what, int condition)
{
	printf("%-58s %s\n", what, condition ? "ok" : "FAIL");
	if (!condition) g_failures++;
}

/* ------------------------------------------------------------------ MQTT */

static void test_mqtt(void)
{
	unsigned char len[4];
	size_t length = 0;
	ok("length: 0 is one byte", mqtt_encode_length(0, len) == 1 && len[0] == 0);
	ok("length: 127 is one byte", mqtt_encode_length(127, len) == 1 && len[0] == 0x7f);
	ok("length: 128 is two bytes (0x80 0x01)", mqtt_encode_length(128, len) == 2 && len[0] == 0x80 && len[1] == 0x01);
	ok("length: 16383 is two bytes", mqtt_encode_length(16383, len) == 2 && len[0] == 0xff && len[1] == 0x7f);
	ok("length: the maximum is four bytes", mqtt_encode_length(268435455, len) == 4);
	ok("length: above the maximum refused", mqtt_encode_length(268435456, len) == 0);
	const unsigned char two[] = { 0x80, 0x01 };
	ok("length: decodes 128", mqtt_decode_length(two, 2, &length) == 2 && length == 128);
	ok("length: needs more bytes", mqtt_decode_length(two, 1, &length) == 0);
	const unsigned char five[] = { 0xff, 0xff, 0xff, 0xff, 0x01 };
	ok("length: a fifth byte is malformed", mqtt_decode_length(five, 5, &length) == -1);

	static unsigned char out[1024];
	mqtt_options o = { "ps5-test", "user", "secret", "ps5/x/availability", "offline", 30 };
	size_t n = mqtt_build_connect(out, sizeof(out), &o);
	ok("connect: built", n > 0);
	ok("connect: type 1", out[0] == 0x10);
	ok("connect: protocol MQTT level 4", memcmp(out + 2, "\x00\x04MQTT\x04", 7) == 0);
	ok("connect: clean session, will retain, user, password", out[9] == (0x02 | 0x04 | 0x20 | 0x40 | 0x80));
	ok("connect: keepalive 30", out[10] == 0 && out[11] == 30);
	ok("connect: remaining length matches", out[1] == n - 2);
	mqtt_options anonymous = { "ps5-test", "", "", NULL, NULL, 30 };
	n = mqtt_build_connect(out, sizeof(out), &anonymous);
	ok("connect: no will, no user: clean session only", n > 0 && out[9] == 0x02);
	ok("connect: too small a buffer refused", mqtt_build_connect(out, 20, &o) == 0);

	n = mqtt_build_publish(out, sizeof(out), "a/b", "hello", 5, 1);
	ok("publish: retained QoS 0", n == 2 + 2 + 3 + 5 && out[0] == 0x31 && out[1] == 10);
	ok("publish: topic then payload", memcmp(out + 2, "\x00\x03" "a/bhello", 10) == 0);
	ok("publish: too large refused", mqtt_build_publish(out, 16, "a/b", "0123456789abcdef", 16, 0) == 0);
	static unsigned char big[2000];
	static char payload[300];
	memset(payload, 'x', sizeof(payload));
	n = mqtt_build_publish(big, sizeof(big), "t", payload, sizeof(payload), 0);
	ok("publish: two-byte length", n == 1 + 2 + 2 + 1 + 300 && big[1] == (303 % 128 | 0x80) && big[2] == 303 / 128);

	n = mqtt_build_subscribe(out, sizeof(out), 7, "ps5/x/command/#");
	ok("subscribe: type 8 with flags 2", n > 0 && out[0] == 0x82);
	ok("subscribe: packet id, filter, QoS 0", out[2] == 0 && out[3] == 7 && out[n - 1] == 0);

	int type;
	size_t offset, body;
	const unsigned char connack[] = { 0x20, 0x02, 0x00, 0x00 };
	ok("parse: connack", mqtt_parse_packet(connack, 4, &type, &offset, &body) == 4 && type == 2 && body == 2);
	ok("parse: incomplete waits", mqtt_parse_packet(connack, 3, &type, &offset, &body) == 0);
	const unsigned char huge[] = { 0x30, 0xff, 0xff, 0xff, 0x7f };
	ok("parse: a packet above the buffer refused", mqtt_parse_packet(huge, 5, &type, &offset, &body) == -1);
	n = mqtt_build_publish(out, sizeof(out), "ps5/x/command/beep", "PRESS", 5, 0);
	ok("parse: our own publish", mqtt_parse_packet(out, n, &type, &offset, &body) == (long)n && type == 3);
}

/* --------------------------------------------------------------- sources */

static const char METRICS[] =
	"# HELP ps5_temperature_celsius Temperature reported by the console.\n"
	"ps5_temperature_celsius{sensor=\"cpu\"} 51\n"
	"ps5_temperature_celsius{sensor=\"soc0\"} 63\n"
	"ps5_temperature_celsius{sensor=\"soc1\"} 58\n"
	"ps5_power_rail_watts{rail=\"0\",name=\"gpu_core\"} 96.250\n"
	"ps5_power_rail_watts{rail=\"2\",name=\"cpu_soc\"} 100.000\n"
	"ps5_power_watts 220.461\n"
	"ps5_fan_duty_ratio 0.3301\n"
	"ps5_cpu_usage_ratio{core=\"0\"} 0.250\n"
	"ps5_cpu_usage_ratio{core=\"1\"} 0.750\n"
	"ps5_soc_clock_hertz{domain=\"19\",name=\"\"} 400000000\n"
	"ps5_soc_clock_hertz{domain=\"20\",name=\"gpu\"} 2150000000\n"
	"ps5_fps 59.9\n"
	"ps5_uptime_seconds 3600\n"
	"ps5_filesystem_avail_bytes{mountpoint=\"/mnt/usb0\",fstype=\"exfatfs\"} 1\n"
	"ps5_filesystem_avail_bytes{mountpoint=\"/user\",fstype=\"ufs\"} 179000000000\n"
	"ps5_game_running 1\n"
	"ps5_game_info{title_id=\"PPSA00001\"} 1\n"
	"ps5_info{model=\"CFI-7121\",firmware=\"13.60\",system_version=\"13.590.001\"} 1\n"
	"ps5_exporter_build_info{version=\"0.3.0\"} 1\n"
	"ps5_network_info{device=\"lo0\",address=\"127.0.0.1\",mac=\"\",mtu=\"16384\"} 1\n"
	"ps5_network_info{device=\"eth0\",address=\"192.168.68.125\",mac=\"c0:15:1b:cf:c3:91\",mtu=\"1500\"} 1\n"
	"ps5_shadowmount_game_info{title_id=\"PPSA00002\",name=\"Other\",platform=\"ps5\",source_type=\"image\"} 1\n"
	"ps5_shadowmount_game_info{title_id=\"PPSA00001\",name=\"A \\\"quoted\\\" game\",platform=\"ps5\",source_type=\"folder\"} 1\n";

static void test_sources(void)
{
	readings r;
	memset(&r, 0, sizeof(r));
	sources_parse_metrics(METRICS, strlen(METRICS), &r);
	ok("metrics: hottest temperature", r.temperature.have && r.temperature.value == 63);
	ok("metrics: total power", r.power.have && r.power.value > 220.4 && r.power.value < 220.5);
	ok("metrics: GPU power is rail 0", r.gpu_power.have && r.gpu_power.value == 96.25);
	ok("metrics: fan in percent", r.fan.have && r.fan.value > 33.0 && r.fan.value < 33.1);
	ok("metrics: CPU load is the average in percent", r.cpu_load.have && r.cpu_load.value == 50.0);
	ok("metrics: GPU clock in MHz", r.gpu_clock.have && r.gpu_clock.value == 2150.0);
	ok("metrics: fps", r.fps.have && r.fps.value > 59.8);
	ok("metrics: free space on /user only", r.free_space.have && r.free_space.value == 179000000000.0);
	ok("metrics: the game and its name", r.game_running && strcmp(r.game_id, "PPSA00001") == 0 &&
	                                         strcmp(r.game_name, "A \\\"quoted\\\" game") == 0);
	ok("metrics: model and firmware", strcmp(r.model, "CFI-7121") == 0 && strcmp(r.firmware, "13.60") == 0);
	ok("metrics: exporter version", strcmp(r.exporter_version, "0.3.0") == 0);
	ok("metrics: the network address, not loopback", strcmp(r.address, "192.168.68.125") == 0);
	memset(&r, 0, sizeof(r));
	const char* odd = "ps5_network_info{device=\"x\",address=\"999.1.1.1\"} 1\n"
	                  "ps5_network_info{device=\"y\",address=\"10.0.0\"} 1\n";
	sources_parse_metrics(odd, strlen(odd), &r);
	ok("metrics: an address that is not IPv4 is left out", r.address[0] == 0);

	memset(&r, 0, sizeof(r));
	const char* none = "ps5_game_running 0\nps5_uptime_seconds 10\n";
	sources_parse_metrics(none, strlen(none), &r);
	ok("metrics: absent values stay unset", !r.power.have && !r.fps.have && !r.temperature.have);
	ok("metrics: no game", !r.game_running && r.game_id[0] == 0);
	memset(&r, 0, sizeof(r));
	const char* broken = "ps5_power_watts\nps5_fps {\n{}\n\n   \nps5_temperature_celsius{sensor=\"cpu\" 5\n";
	sources_parse_metrics(broken, strlen(broken), &r);
	ok("metrics: broken lines are skipped", !r.power.have && !r.fps.have);
}

/* ------------------------------------------------------------- discovery */

static int valid_json(const char* text)
{
	static json_document doc;
	return json_parse(&doc, text, strlen(text)) && doc.nodes[0].type == JSON_OBJECT;
}

static void test_discovery(void)
{
	readings r;
	memset(&r, 0, sizeof(r));
	sources_parse_metrics(METRICS, strlen(METRICS), &r);
	r.exporter = true;
	static char storage[4096];
	char topic[192];
	int all_valid = 1, all_fit = 1, count = 0;
	for (int i = 0; i < discovery_count(); i++) {
		Buf b;
		buf_init(&b, storage, sizeof(storage));
		if (!discovery_message(i, "aabbccddeeff", &r, true, "0.1.0", topic, sizeof(topic), &b)) break;
		count++;
		all_fit &= !b.truncated;
		/* json.c refuses fractions, which discovery does not use; it checks the structure. */
		if (!valid_json(storage)) {
			all_valid = 0;
			printf("  invalid: %s\n", storage);
		}
	}
	ok("discovery: one message per entity", count == discovery_count() && count >= 15);
	ok("discovery: every message fits", all_fit);
	ok("discovery: every message is a JSON object", all_valid);
	Buf b;
	buf_init(&b, storage, sizeof(storage));
	discovery_message(0, "aabbccddeeff", &r, true, "0.1.0", topic, sizeof(topic), &b);
	ok("discovery: topic", strcmp(topic, "homeassistant/sensor/ps5_aabbccddeeff/temperature/config") == 0);
	ok("discovery: unique id and state topic", strstr(storage, "\"unique_id\":\"ps5_aabbccddeeff_temperature\"") &&
	                                               strstr(storage, "\"state_topic\":\"ps5/aabbccddeeff/state\""));
	ok("discovery: device with the model", strstr(storage, "\"model\":\"CFI-7121\"") != NULL);

	/* The power buttons: there with power actions on, an empty (removing) config without; nothing else changes. */
	int buttons_on = 0, emptied = 0, others_kept = 1;
	for (int i = 0; i < discovery_count(); i++) {
		Buf on, off;
		static char a[4096], z[4096];
		buf_init(&on, a, sizeof(a));
		buf_init(&off, z, sizeof(z));
		discovery_message(i, "aabbccddeeff", &r, true, "0.1.0", topic, sizeof(topic), &on);
		const int power = strstr(topic, "/rest/config") != NULL || strstr(topic, "/power_off/config") != NULL;
		discovery_message(i, "aabbccddeeff", &r, false, "0.1.0", topic, sizeof(topic), &off);
		if (power) {
			buttons_on += strstr(a, "\"command_topic\":\"ps5/aabbccddeeff/command/") != NULL &&
			              strstr(a, "\"payload_press\":\"PRESS\"") != NULL;
			emptied += z[0] == 0;
		} else if (strcmp(a, z) != 0) {
			others_kept = 0;
		}
		if (strstr(topic, "/ip_address/config"))
			ok("discovery: the IP address keeps its value (no availability)", strstr(a, "availability") == NULL);
	}
	ok("discovery: rest mode and power off buttons with power actions on", buttons_on == 2);
	ok("discovery: both removed with power actions off", emptied == 2);
	ok("discovery: the other entities do not depend on it", others_kept);

	int events_found = 0;
	for (int i = 0; i < discovery_count(); i++) {
		buf_init(&b, storage, sizeof(storage));
		discovery_message(i, "aabbccddeeff", &r, true, "0.1.0", topic, sizeof(topic), &b);
		if (strcmp(topic, "homeassistant/event/ps5_aabbccddeeff/trophy/config") == 0)
			ok("discovery: the trophy event's topic and grades",
			   strstr(storage, "\"state_topic\":\"ps5/aabbccddeeff/event/trophy\"") &&
			       strstr(storage, "\"event_types\":[\"bronze\",\"silver\",\"gold\",\"platinum\"]") &&
			       strstr(storage, "command_topic") == NULL);
		events_found += strncmp(topic, "homeassistant/event/", 20) == 0;
	}
	ok("discovery: four event entities", events_found == 4);

	buf_init(&b, storage, sizeof(storage));
	discovery_state(&r, &b);
	ok("state: valid JSON with the escaped name", strstr(storage, "\"game\":\"A \\\"quoted\\\" game\"") != NULL);
	ok("state: the IP address", strstr(storage, "\"ip_address\":\"192.168.68.125\"") != NULL);
	ok("state: values", strstr(storage, "\"power\":220.5") && strstr(storage, "\"gpu_clock\":2150") && strstr(storage, "\"free_space\":179.0"));
	ok("state: an absent value is null", strstr(storage, "\"signal\":false") && strstr(storage, "\"game_running\":true"));
	memset(&r, 0, sizeof(r));
	buf_init(&b, storage, sizeof(storage));
	discovery_state(&r, &b);
	ok("state: no readings, all null", strstr(storage, "\"power\":null") && strstr(storage, "\"game\":null") &&
	                                       strstr(storage, "\"ip_address\":null"));
}

/* -------------------------------------------------------------- commands */

static command cmd(const char* topic, const char* payload)
{
	return discovery_command("aabbccddeeff", topic, strlen(topic), (const unsigned char*)payload, strlen(payload));
}

static void test_commands(void)
{
	command c = cmd("ps5/aabbccddeeff/command/notify", "Someone is at the door");
	ok("notify: text", c.kind == COMMAND_NOTIFY && strcmp(c.text, "Someone is at the door") == 0);
	ok("notify: UTF-8 accepted", cmd("ps5/aabbccddeeff/command/notify", "Er staat iemand aan de deur \xF0\x9F\x9A\xAA").kind == COMMAND_NOTIFY);
	ok("notify: broken UTF-8 refused", cmd("ps5/aabbccddeeff/command/notify", "bad \xC3").kind == COMMAND_NONE);
	ok("notify: control characters refused", cmd("ps5/aabbccddeeff/command/notify", "a\x07" "b").kind == COMMAND_NONE);
	ok("notify: empty refused", cmd("ps5/aabbccddeeff/command/notify", "").kind == COMMAND_NONE);
	static char longer[300];
	memset(longer, 'a', sizeof(longer) - 1);
	ok("notify: over 256 bytes refused", cmd("ps5/aabbccddeeff/command/notify", longer).kind == COMMAND_NONE);
	c = cmd("ps5/aabbccddeeff/command/notify", "{\"title\":\"Deurbel\",\"message\":\"Er staat iemand\\nvoor de deur\"}");
	ok("notify: JSON with a title", c.kind == COMMAND_NOTIFY && strcmp(c.title, "Deurbel") == 0 &&
	                                   strcmp(c.text, "Er staat iemand\nvoor de deur") == 0);
	c = cmd("ps5/aabbccddeeff/command/notify", "{\"title\":null,\"message\":\"No title\"}");
	ok("notify: JSON without a title", c.kind == COMMAND_NOTIFY && c.title[0] == 0 && strcmp(c.text, "No title") == 0);
	ok("notify: JSON without a message refused", cmd("ps5/aabbccddeeff/command/notify", "{\"title\":\"x\"}").kind == COMMAND_NONE);
	ok("notify: \\u escapes refused", cmd("ps5/aabbccddeeff/command/notify", "{\"message\":\"\\u0007\"}").kind == COMMAND_NONE);
	ok("notify: broken JSON refused", cmd("ps5/aabbccddeeff/command/notify", "{\"message\":").kind == COMMAND_NONE);
	ok("beep", cmd("ps5/aabbccddeeff/command/beep", "PRESS").kind == COMMAND_BEEP);
	ok("blink", cmd("ps5/aabbccddeeff/command/blink", "PRESS").kind == COMMAND_BLINK);
	c = cmd("ps5/aabbccddeeff/command/colour", "Purple");
	int bl, w, a;
	discovery_colour_levels(c.colour, &bl, &w, &a);
	ok("colour: a preset", c.kind == COMMAND_COLOUR && bl == 255 && w == 80 && a == 200);
	ok("colour: System is preset 0", cmd("ps5/aabbccddeeff/command/colour", "System").colour == 0);
	ok("colour: unknown refused", cmd("ps5/aabbccddeeff/command/colour", "Green").kind == COMMAND_NONE);
	ok("rest mode on PRESS", cmd("ps5/aabbccddeeff/command/rest", "PRESS").kind == COMMAND_REST);
	ok("power off on PRESS", cmd("ps5/aabbccddeeff/command/power_off", "PRESS").kind == COMMAND_POWER_OFF);
	ok("rest mode: anything but PRESS refused", cmd("ps5/aabbccddeeff/command/rest", "press").kind == COMMAND_NONE &&
	                                                cmd("ps5/aabbccddeeff/command/rest", "").kind == COMMAND_NONE &&
	                                                cmd("ps5/aabbccddeeff/command/power_off", "PRESS!").kind == COMMAND_NONE);
	const char* why;
	ok("power: refused while switched off", !power_allowed(false, 0, 50000, &why) && strstr(why, "off") != NULL);
	ok("power: allowed when on", power_allowed(true, 0, 50000, &why) && why[0] == 0);
	ok("power: one per 10 seconds", !power_allowed(true, 45000, 50000, &why) && power_allowed(true, 40000, 50000, &why));
	ok("another console's topic ignored", cmd("ps5/000000000000/command/beep", "PRESS").kind == COMMAND_NONE);
	ok("unknown action ignored", cmd("ps5/aabbccddeeff/command/reboot", "PRESS").kind == COMMAND_NONE);
	ok("the prefix alone ignored", cmd("ps5/aabbccddeeff/command/", "x").kind == COMMAND_NONE);
}

/* ---------------------------------------------------------------- config */

static void test_config(void)
{
	config c;
	char error[128];
	config_defaults(&c);
	ok("config: defaults are not ready", !config_ready(&c) && c.port == 1883 && c.interval == 10 && !c.power_actions);
	const char* good = "{\"host\":\"192.168.1.10\",\"username\":\"ps5\",\"password\":\"s3cret!\",\"interval\":15}";
	ok("config: a valid config", config_parse(good, strlen(good), &c, error, sizeof(error)) && config_ready(&c) &&
	                                 c.interval == 15 && strcmp(c.password, "s3cret!") == 0);
	const char* no_password = "{\"host\":\"homeassistant.local\"}";
	ok("config: a field left out keeps its value",
	   config_parse(no_password, strlen(no_password), &c, error, sizeof(error)) && strcmp(c.password, "s3cret!") == 0 &&
	       strcmp(c.host, "homeassistant.local") == 0);
	const char* bad[] = {
		"{\"host\":\"evil host\"}", "{\"host\":\"\"}", "{\"port\":0}", "{\"port\":70000}", "{\"interval\":1}",
		"{\"password\":\"with \\\"quote\"}", "{\"username\":5}", "[]", "not json",
		"{\"power_actions\":1}", "{\"power_actions\":\"true\"}",
	};
	int refused = 1;
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		config before = c;
		if (config_parse(bad[i], strlen(bad[i]), &c, error, sizeof(error)) || memcmp(&before, &c, sizeof(c)) != 0) {
			refused = 0;
			printf("  accepted: %s\n", bad[i]);
		}
	}
	ok("config: invalid ones refused and nothing changed", refused);
	const char* power_on = "{\"power_actions\":true}";
	ok("config: power actions switched on", config_parse(power_on, strlen(power_on), &c, error, sizeof(error)) &&
	                                           c.power_actions && strcmp(c.password, "s3cret!") == 0);
	ok("config: save", config_save(&c, error, sizeof(error)));
	config loaded;
	ok("config: load what was saved", config_load(&loaded, error, sizeof(error)) && memcmp(&loaded, &c, sizeof(c)) == 0);
	char text[512];
	config_public_json(&c, text, sizeof(text));
	ok("config: the public view hides the password", strstr(text, "s3cret") == NULL && strstr(text, "\"password_set\":true"));
	ok("config: the public view says whether power actions are on", strstr(text, "\"power_actions\":true") != NULL);
	remove(CONFIG_PATH);
}

/* ---------------------------------------------------------------- events */

static notification_row row(long long id, long long updated_ms, const char* channel, const char* use_case)
{
	notification_row n;
	memset(&n, 0, sizeof(n));
	n.id = id;
	n.updated_ms = updated_ms;
	snprintf(n.channel, sizeof(n.channel), "%s", channel);
	snprintf(n.use_case, sizeof(n.use_case), "%s", use_case);
	return n;
}

static notification_row trophy_row(long long id, long long updated_ms, const char* grade, const char* name)
{
	notification_row n = row(id, updated_ms, "Trophies", "NUC55");
	snprintf(n.grade, sizeof(n.grade), "%s", grade);
	snprintf(n.trophy_name, sizeof(n.trophy_name), "%s", name);
	snprintf(n.trophy_id, sizeof(n.trophy_id), "7");
	snprintf(n.np_comm_id, sizeof(n.np_comm_id), "NPWR00001_00");
	return n;
}

static void test_events(void)
{
	const long long now = 1800000000000LL;
	event_state s;
	events_init(&s);
	console_event out[8];

	notification_row old[] = { trophy_row(10, now - 1000, "Bronze", "Old one"), row(11, now - 500, "Downloads", "NUC240") };
	ok("events: the baseline sends nothing", events_from_notifications(&s, old, 2, now, out, 8) == 0);
	ok("events: the baseline sets the cursor", s.last_id == 11 && s.last_updated_ms == now - 500);
	ok("events: rows seen before send nothing", events_from_notifications(&s, old, 2, now, out, 8) == 0);

	notification_row gold = trophy_row(12, now - 100, "Gold", "A \"quoted\" trophy");
	int n = events_from_notifications(&s, &gold, 1, now, out, 8);
	ok("events: a new gold trophy", n == 1 && strcmp(out[0].object, "trophy") == 0 &&
	                                    strstr(out[0].payload, "\"event_type\":\"gold\"") != NULL);
	ok("events: the trophy's name escaped, id and set", strstr(out[0].payload, "\"trophy\":\"A \\\"quoted\\\" trophy\"") &&
	                                                         strstr(out[0].payload, "\"trophy_id\":\"7\"") &&
	                                                         strstr(out[0].payload, "\"trophy_set\":\"NPWR00001_00\""));
	ok("events: the payload is a JSON object", valid_json(out[0].payload));

	notification_row mixed[5];
	mixed[0] = row(13, now - 90, "Downloads", "IDC"); /* a homebrew notification, ours included */
	mixed[1] = row(14, now - 80, "Downloads", "NUC240");
	snprintf(mixed[1].title_id, sizeof(mixed[1].title_id), "PPSA00002");
	snprintf(mixed[1].message, sizeof(mixed[1].message), "Installed.");
	mixed[2] = row(15, now - EVENTS_MAX_AGE_MS - 1, "Downloads", "NUC246"); /* too old after an outage */
	mixed[3] = row(16, now - 70, "SystemFeedback", "NUC411");
	mixed[4] = trophy_row(17, now - 60, "Diamond", "Unknown grade");
	n = events_from_notifications(&s, mixed, 5, now, out, 8);
	ok("events: only the install is sent", n == 1 && strcmp(out[0].object, "download") == 0 &&
	                                           strstr(out[0].payload, "\"event_type\":\"installed\"") &&
	                                           strstr(out[0].payload, "\"title_id\":\"PPSA00002\"") &&
	                                           strstr(out[0].payload, "\"message\":\"Installed.\""));
	ok("events: the cursor passes every row, sent or not", s.last_id == 17);
	ok("events: an unknown trophy grade is noted", strstr(s.skipped, "Diamond") != NULL);
	notification_row other = row(18, now, "Downloads", "NUC999");
	n = events_from_notifications(&s, &other, 1, now, out, 8);
	ok("events: an unknown download kind is other", n == 1 && strstr(out[0].payload, "\"event_type\":\"other\""));

	/* Games, from ps5-exporter's readings. */
	readings r;
	memset(&r, 0, sizeof(r));
	ok("game: no exporter, no baseline", events_from_reading(&s, &r, out, 8) == 0 && !s.game_seen);
	r.exporter = true;
	r.game_running = true;
	snprintf(r.game_id, sizeof(r.game_id), "PPSA00001");
	snprintf(r.game_name, sizeof(r.game_name), "First");
	ok("game: the baseline sends nothing", events_from_reading(&s, &r, out, 8) == 0);
	ok("game: the same game sends nothing", events_from_reading(&s, &r, out, 8) == 0);
	readings gone = r;
	gone.exporter = false;
	gone.game_running = false;
	gone.game_id[0] = 0;
	ok("game: the exporter missing is not a stop", events_from_reading(&s, &gone, out, 8) == 0 &&
	                                                    strcmp(s.game_id, "PPSA00001") == 0);
	snprintf(r.game_id, sizeof(r.game_id), "PPSA00002");
	snprintf(r.game_name, sizeof(r.game_name), "Second");
	n = events_from_reading(&s, &r, out, 8);
	ok("game: a switch is a stop and a start", n == 2 && strstr(out[0].payload, "\"event_type\":\"stopped\"") &&
	                                                strstr(out[0].payload, "\"game\":\"First\"") &&
	                                                strstr(out[1].payload, "\"event_type\":\"started\"") &&
	                                                strstr(out[1].payload, "\"title_id\":\"PPSA00002\""));
	r.game_running = false;
	n = events_from_reading(&s, &r, out, 8);
	ok("game: quitting is a stop", n == 1 && strstr(out[0].payload, "\"stopped\"") && strstr(out[0].payload, "Second"));

	/* A trophy names the game that runs; without a game, only the trophy set. */
	r.game_running = true;
	snprintf(r.game_id, sizeof(r.game_id), "PPSA00003");
	snprintf(r.game_name, sizeof(r.game_name), "Third");
	events_from_reading(&s, &r, out, 8);
	notification_row during = trophy_row(30, now - 10, "Bronze", "In game");
	n = events_from_notifications(&s, &during, 1, now, out, 8);
	ok("trophy: the running game's title ID and name", n == 1 && strstr(out[0].payload, "\"title_id\":\"PPSA00003\"") &&
	                                                       strstr(out[0].payload, "\"game\":\"Third\""));
	r.game_running = false;
	events_from_reading(&s, &r, out, 8);
	notification_row after = trophy_row(31, now - 5, "Silver", "No game");
	n = events_from_notifications(&s, &after, 1, now, out, 8);
	ok("trophy: no game running, no game fields", n == 1 && strstr(out[0].payload, "title_id") == NULL &&
	                                                  strstr(out[0].payload, "\"game\"") == NULL);

	/* ShadowMountPlus' storage jobs. */
	storage_status st;
	memset(&st, 0, sizeof(st));
	ok("storage: no answer, nothing", events_from_storage(&s, &st, out, 8) == 0 && !s.storage_seen);
	st.have = true;
	st.job_id = 3;
	snprintf(st.state, sizeof(st.state), "completed");
	ok("storage: a job that ended before the baseline is not sent", events_from_storage(&s, &st, out, 8) == 0);
	st.job_id = 4;
	snprintf(st.state, sizeof(st.state), "transferring");
	ok("storage: a running job sends nothing", events_from_storage(&s, &st, out, 8) == 0);
	snprintf(st.state, sizeof(st.state), "completed");
	snprintf(st.operation, sizeof(st.operation), "copy");
	snprintf(st.destination, sizeof(st.destination), "/mnt/usb0/data/etaHEN/games/PPSA00001 A.ffpfsc");
	n = events_from_storage(&s, &st, out, 8);
	ok("storage: a finished copy", n == 1 && strcmp(out[0].object, "storage") == 0 &&
	                                   strstr(out[0].payload, "\"event_type\":\"completed\"") &&
	                                   strstr(out[0].payload, "\"operation\":\"copy\"") &&
	                                   strstr(out[0].payload, "\"destination\":\"/mnt/usb0/data/etaHEN/games/PPSA00001 A.ffpfsc\""));
	ok("storage: sent once", events_from_storage(&s, &st, out, 8) == 0);
	st.job_id = 5;
	snprintf(st.state, sizeof(st.state), "failed");
	snprintf(st.error, sizeof(st.error), "a game is active");
	n = events_from_storage(&s, &st, out, 8);
	ok("storage: a failed job with its error", n == 1 && strstr(out[0].payload, "\"failed\"") &&
	                                               strstr(out[0].payload, "\"error\":\"a game is active\""));
	event_state fresh;
	events_init(&fresh);
	st.job_id = 7;
	snprintf(st.state, sizeof(st.state), "transferring");
	events_from_storage(&fresh, &st, out, 8);
	snprintf(st.state, sizeof(st.state), "cancelled");
	ok("storage: a job running at the baseline is sent when it ends", events_from_storage(&fresh, &st, out, 8) == 1);

	const char* answer =
		"{\"status\":0,\"job_id\":2,\"operation\":\"copy\",\"state\":\"completed\",\"active\":false,"
		"\"title_id\":\"PPSA02225\",\"destination\":\"\\/mnt\\/usb0\\/data\\/etaHEN\\/games\\/PPSA02225 GOT.ffpfsc\","
		"\"progress_percent\":100.0,\"result_status\":0,\"result_error\":\"\"}";
	sources_parse_storage(answer, &st);
	ok("storage: ShadowMountPlus' answer, fractions and escaped slashes",
	   st.have && st.job_id == 2 && strcmp(st.state, "completed") == 0 && strcmp(st.operation, "copy") == 0 &&
	       strcmp(st.title_id, "PPSA02225") == 0 &&
	       strcmp(st.destination, "/mnt/usb0/data/etaHEN/games/PPSA02225 GOT.ffpfsc") == 0 && st.error[0] == 0);
	sources_parse_storage("{\"status\":2,\"error\":\"storage job not found\"}", &st);
	ok("storage: no job yet", !st.have);
	sources_parse_storage("not json", &st);
	ok("storage: garbage", !st.have);
}

/* --------------------------------------------------------- notifications */

#define TEST_DB "build/notification-test.db"

/* The schema of the system's notification2.db (decision log D9); the rows are made up. */
static void db_exec(const char* sql)
{
	sqlite3* db;
	sqlite3_open(TEST_DB, &db);
	sqlite3_exec(db, sql, NULL, NULL, NULL);
	sqlite3_close(db);
}

static void test_notifications(void)
{
	remove(TEST_DB);
	db_exec("CREATE TABLE notification(    id INTEGER PRIMARY KEY AUTOINCREMENT,    user_id INTEGER DEFAULT -1,"
	        "    originator INTEGER NOT NULL,    is_popped_up INTEGER DEFAULT 0,    server_sync_state INTEGER,"
	        "    notification_id TEXT,    local_notification_id TEXT,    bundle_name TEXT,    state TEXT NOT NULL,"
	        "    created_date_time INTEGER NOT NULL,    updated_date_time INTEGER NOT NULL,"
	        "    expiration_date_time INTEGER,    raw_data TEXT NOT NULL,    upload_data TEXT,    cached_data TEXT,"
	        "    extra_data TEXT );"
	        "INSERT INTO notification(id, originator, bundle_name, state, created_date_time, updated_date_time, raw_data)"
	        " VALUES (20, 0, 'local:Trophy', 'New', 1000, 1000,"
	        " '{\"channelType\":\"Trophies\",\"useCaseId\":\"NUC55\",\"telemetryData\":{\"parameters\":"
	        "{\"isTrophyAttained\":true,\"trophyGrade\":\"Silver\",\"trophyId\":\"3\",\"trophyName\":\"Made up\","
	        "\"trophyNpCommId\":\"NPWR00001_00\"}},\"viewData\":{\"message\":{\"body\":\" Made up\"}}}'),"
	        " (21, 0, NULL, 'New', 2000, 2000,"
	        " '{\"channelType\":\"Downloads\",\"useCaseId\":\"NUC240\",\"viewData\":{\"icon\":{\"parameters\":"
	        "{\"npTitleIds\":[\"PPSA00009_00\"]},\"type\":\"TitleByTitle\"},\"message\":{\"body\":\"Installed.\"}}}'),"
	        " (22, 0, NULL, 'New', 3000, 3000, 'not json'),"
	        " (23, 0, NULL, 'New', 4000, 4000,"
	        " '{\"channelType\":\"Downloads\",\"useCaseId\":\"IDC\",\"viewData\":{\"message\":{\"body\":\"etaHEN\"}}}');");

	notifications_reader reader;
	notifications_init(&reader, TEST_DB);
	static notification_row rows[NOTIFICATIONS_MAX_ROWS];
	int count = 0;
	char error[128];
	notifications_result result = notifications_poll(&reader, 0, 0, rows, &count, error, sizeof(error));
	ok("notifications: read from a copy", result == NOTIFICATIONS_READ);
	ok("notifications: valid rows, oldest first, bad JSON skipped",
	   count == 3 && rows[0].id == 20 && rows[1].id == 21 && rows[2].id == 23);
	ok("notifications: the trophy's fields", strcmp(rows[0].channel, "Trophies") == 0 && strcmp(rows[0].grade, "Silver") == 0 &&
	                                             strcmp(rows[0].trophy_name, "Made up") == 0 &&
	                                             strcmp(rows[0].trophy_id, "3") == 0 &&
	                                             strcmp(rows[0].np_comm_id, "NPWR00001_00") == 0);
	ok("notifications: the title ID without _00, and the message",
	   strcmp(rows[1].title_id, "PPSA00009") == 0 && strcmp(rows[1].message, "Installed.") == 0 && rows[1].updated_ms == 2000);
	ok("notifications: an unchanged file is not read again",
	   notifications_poll(&reader, 23, 4000, rows, &count, error, sizeof(error)) == NOTIFICATIONS_UNCHANGED);

	usleep(20000); /* a later modification time */
	db_exec("INSERT INTO notification(id, originator, state, created_date_time, updated_date_time, raw_data)"
	        " VALUES (24, 0, 'New', 5000, 5000, '{\"channelType\":\"Trophies\",\"telemetryData\":"
	        "{\"parameters\":{\"trophyGrade\":\"Platinum\"}}}');");
	result = notifications_poll(&reader, 23, 4000, rows, &count, error, sizeof(error));
	ok("notifications: only the new row after a change", result == NOTIFICATIONS_READ && count == 1 && rows[0].id == 24 &&
	                                                         strcmp(rows[0].grade, "Platinum") == 0);

	FILE* f = fopen(TEST_DB, "w");
	if (f) {
		fputs("this is not a database", f);
		fclose(f);
	}
	result = notifications_poll(&reader, 24, 5000, rows, &count, error, sizeof(error));
	ok("notifications: a file that is not SQLite is an error", result == NOTIFICATIONS_ERROR && error[0] != 0);
	ok("notifications: and is not tried again until it changes",
	   notifications_poll(&reader, 24, 5000, rows, &count, error, sizeof(error)) == NOTIFICATIONS_UNCHANGED);
	remove(TEST_DB);
	notifications_init(&reader, TEST_DB);
	ok("notifications: a missing file is an error",
	   notifications_poll(&reader, 0, 0, rows, &count, error, sizeof(error)) == NOTIFICATIONS_ERROR);
}

int main(void)
{
	test_mqtt();
	test_sources();
	test_discovery();
	test_commands();
	test_config();
	test_events();
	test_notifications();
	printf("\n%s (%d failure%s)\n", g_failures ? "FAIL" : "PASS", g_failures, g_failures == 1 ? "" : "s");
	return g_failures != 0;
}
