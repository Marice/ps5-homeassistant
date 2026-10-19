#include "discovery.h"

#include <stdio.h>

#include "json.h"
#include <string.h>

/* One entity: its component and object id, and the JSON members that differ per entity. */
typedef struct {
	const char* component;
	const char* object;
	const char* name;
	const char* members; /* without the leading comma */
	bool needs_signal;   /* also unavailable while ps5-signal does not answer */
	bool power;          /* only while power actions are on; otherwise its discovery is removed */
	bool keeps_value;    /* no availability: keeps its last value while the console sleeps */
} entity;

#define VALUE(key) "\"value_template\":\"{{ value_json." key " }}\""

static const entity ENTITIES[] = {
	{ "sensor", "temperature", "Temperature",
	  "\"device_class\":\"temperature\",\"unit_of_measurement\":\"\xC2\xB0" "C\",\"state_class\":\"measurement\"," VALUE("temperature"), false, false, false },
	{ "sensor", "power", "Power",
	  "\"device_class\":\"power\",\"unit_of_measurement\":\"W\",\"state_class\":\"measurement\"," VALUE("power"), false, false, false },
	{ "sensor", "gpu_power", "GPU power",
	  "\"device_class\":\"power\",\"unit_of_measurement\":\"W\",\"state_class\":\"measurement\"," VALUE("gpu_power"), false, false, false },
	{ "sensor", "fan", "Fan",
	  "\"unit_of_measurement\":\"%\",\"state_class\":\"measurement\",\"icon\":\"mdi:fan\"," VALUE("fan"), false, false, false },
	{ "sensor", "cpu_load", "CPU load",
	  "\"unit_of_measurement\":\"%\",\"state_class\":\"measurement\",\"icon\":\"mdi:cpu-64-bit\"," VALUE("cpu_load"), false, false, false },
	{ "sensor", "gpu_clock", "GPU clock",
	  "\"device_class\":\"frequency\",\"unit_of_measurement\":\"MHz\",\"state_class\":\"measurement\"," VALUE("gpu_clock"), false, false, false },
	{ "sensor", "fps", "Frame rate",
	  "\"unit_of_measurement\":\"fps\",\"state_class\":\"measurement\",\"icon\":\"mdi:speedometer\"," VALUE("fps"), false, false, false },
	{ "sensor", "game", "Game", "\"icon\":\"mdi:gamepad-variant\"," VALUE("game"), false, false, false },
	{ "sensor", "game_id", "Game title ID", "\"icon\":\"mdi:identifier\",\"entity_category\":\"diagnostic\"," VALUE("game_id"), false, false, false },
	{ "sensor", "uptime", "Uptime",
	  "\"device_class\":\"duration\",\"unit_of_measurement\":\"s\",\"entity_category\":\"diagnostic\"," VALUE("uptime"), false, false, false },
	{ "sensor", "free_space", "Free space",
	  "\"device_class\":\"data_size\",\"unit_of_measurement\":\"GB\",\"suggested_display_precision\":1,\"state_class\":\"measurement\"," VALUE("free_space"), false, false, false },
	{ "sensor", "ip_address", "IP address",
	  "\"icon\":\"mdi:ip-network\",\"entity_category\":\"diagnostic\"," VALUE("ip_address"), false, false, true },
	{ "binary_sensor", "game_running", "Game running",
	  "\"icon\":\"mdi:controller\",\"payload_on\":true,\"payload_off\":false," VALUE("game_running"), false, false, false },
	{ "binary_sensor", "exporter", "ps5-exporter",
	  "\"device_class\":\"connectivity\",\"entity_category\":\"diagnostic\",\"payload_on\":true,\"payload_off\":false," VALUE("exporter"), false, false, false },
	{ "binary_sensor", "signal", "ps5-signal",
	  "\"device_class\":\"connectivity\",\"entity_category\":\"diagnostic\",\"payload_on\":true,\"payload_off\":false," VALUE("signal"), false, false, false },
	{ "notify", "notify", "Notification",
	  "\"icon\":\"mdi:message-text\",\"command_template\":\"{{ {'title': title, 'message': message} | tojson }}\"", false, false, false },
	{ "button", "beep", "Beep", "\"icon\":\"mdi:volume-high\",\"payload_press\":\"PRESS\"", true, false, false },
	{ "button", "blink", "Blink the light", "\"icon\":\"mdi:lightbulb-on\",\"payload_press\":\"PRESS\"", true, false, false },
	{ "select", "colour", "Light colour", "\"icon\":\"mdi:palette\",\"optimistic\":true", true, false, false },
	{ "button", "rest", "Rest mode", "\"icon\":\"mdi:power-sleep\",\"payload_press\":\"PRESS\"", false, true, false },
	{ "button", "power_off", "Power off", "\"icon\":\"mdi:power\",\"payload_press\":\"PRESS\"", false, true, false },
};
#define ENTITY_COUNT ((int)(sizeof(ENTITIES) / sizeof(ENTITIES[0])))

static const struct {
	const char* name;
	int blue, white, amber;
} COLOURS[] = {
	{ "System", 0, 0, 0 },     { "Blue", 255, 0, 0 },      { "White", 0, 255, 0 },
	{ "Orange", 0, 0, 255 },   { "Purple", 255, 80, 200 }, { "Pink", 65, 10, 255 },
	{ "Off", 0, 0, 0 },
};
#define COLOUR_COUNT ((int)(sizeof(COLOURS) / sizeof(COLOURS[0])))

int discovery_count(void)
{
	return ENTITY_COUNT;
}

int discovery_colour_count(void)
{
	return COLOUR_COUNT;
}

const char* discovery_colour_name(int index)
{
	return index >= 0 && index < COLOUR_COUNT ? COLOURS[index].name : "";
}

void discovery_colour_levels(int index, int* blue, int* white, int* amber)
{
	*blue = *white = *amber = 0;
	if (index <= 0 || index >= COLOUR_COUNT) return;
	*blue = COLOURS[index].blue;
	*white = COLOURS[index].white;
	*amber = COLOURS[index].amber;
}

/* Text between JSON quotes: control characters dropped, quotes and backslashes escaped. A label value from
   Prometheus text arrives escaped already (\" and \\), which is valid JSON as it stands. */
static void add_text(Buf* b, const char* text, bool escaped)
{
	for (size_t i = 0; text[i]; i++) {
		unsigned char c = (unsigned char)text[i];
		if (c < 32) continue;
		if (escaped && c == '\\' && (text[i + 1] == '"' || text[i + 1] == '\\')) {
			buf_addf(b, "\\%c", text[i + 1]);
			i++;
			continue;
		}
		if (c == '"' || c == '\\') buf_addf(b, "\\%c", c);
		else buf_addf(b, "%c", c);
	}
}

bool discovery_message(int index, const char* id, const readings* r, bool power_actions, const char* version, char* topic,
                       size_t topic_cap, Buf* b)
{
	if (index < 0 || index >= ENTITY_COUNT) return false;
	const entity* e = &ENTITIES[index];
	snprintf(topic, topic_cap, DISCOVERY_PREFIX "/%s/ps5_%s/%s/config", e->component, id, e->object);
	if (e->power && !power_actions) return true; /* an empty retained config removes the entity */
	buf_addf(b, "{\"name\":\"%s\",\"unique_id\":\"ps5_%s_%s\",\"object_id\":\"ps5_%s\",", e->name, id, e->object,
	         e->object);
	if (strcmp(e->component, "sensor") == 0 || strcmp(e->component, "binary_sensor") == 0)
		buf_addf(b, "\"state_topic\":\"ps5/%s/state\",", id);
	else
		buf_addf(b, "\"command_topic\":\"ps5/%s/command/%s\",", id, e->object);
	if (strcmp(e->component, "select") == 0) {
		buf_addf(b, "\"options\":[");
		for (int i = 0; i < COLOUR_COUNT; i++) buf_addf(b, "%s\"%s\"", i ? "," : "", COLOURS[i].name);
		buf_addf(b, "],");
	}
	if (e->keeps_value) {
		buf_addf(b, "%s,", e->members);
	} else {
		buf_addf(b, "\"availability\":[{\"topic\":\"ps5/%s/availability\"}", id);
		if (e->needs_signal) buf_addf(b, ",{\"topic\":\"ps5/%s/signal\"}", id);
		buf_addf(b, "],\"availability_mode\":\"all\",%s,", e->members);
	}
	buf_addf(b, "\"device\":{\"identifiers\":[\"ps5_%s\"],\"name\":\"PS5\",\"manufacturer\":\"Sony Interactive Entertainment\",", id);
	buf_addf(b, "\"model\":\"");
	add_text(b, r->model[0] ? r->model : "PlayStation 5", true);
	buf_addf(b, "\",\"sw_version\":\"");
	add_text(b, r->firmware, true);
	buf_addf(b, "\"},\"origin\":{\"name\":\"ps5-homeassistant\",\"sw\":\"%s\",\"url\":\"https://github.com/Marice/ps5-homeassistant\"}}",
	         version);
	return true;
}

static void add_reading(Buf* b, const char* key, const reading* r, int decimals)
{
	if (r->have) buf_addf(b, "\"%s\":%.*f,", key, decimals, r->value);
	else buf_addf(b, "\"%s\":null,", key);
}

void discovery_state(const readings* r, Buf* b)
{
	buf_addf(b, "{");
	add_reading(b, "temperature", &r->temperature, 0);
	add_reading(b, "power", &r->power, 1);
	add_reading(b, "gpu_power", &r->gpu_power, 1);
	add_reading(b, "fan", &r->fan, 0);
	add_reading(b, "cpu_load", &r->cpu_load, 0);
	add_reading(b, "gpu_clock", &r->gpu_clock, 0);
	add_reading(b, "fps", &r->fps, 0);
	add_reading(b, "uptime", &r->uptime, 0);
	reading gigabytes = r->free_space;
	gigabytes.value /= 1e9;
	add_reading(b, "free_space", &gigabytes, 1);
	/* The game: its name when ShadowMountPlus knows it, else the title ID; null when none runs. */
	if (r->game_running && r->game_id[0]) {
		buf_addf(b, "\"game\":\"");
		add_text(b, r->game_name[0] ? r->game_name : r->game_id, true);
		buf_addf(b, "\",\"game_id\":\"");
		add_text(b, r->game_id, true);
		buf_addf(b, "\",");
	} else {
		buf_addf(b, "\"game\":null,\"game_id\":null,");
	}
	if (r->address[0]) buf_addf(b, "\"ip_address\":\"%s\",", r->address);
	else buf_addf(b, "\"ip_address\":null,");
	buf_addf(b, "\"game_running\":%s,\"exporter\":%s,\"signal\":%s}", r->game_running ? "true" : "false",
	         r->exporter ? "true" : "false", r->signal ? "true" : "false");
}

/* Valid UTF-8 without control characters other than newline. */
static bool valid_text(const unsigned char* s, size_t len)
{
	for (size_t i = 0; i < len;) {
		unsigned char c = s[i];
		if (c < 0x80) {
			if ((c < 32 && c != '\n') || c == 127) return false;
			i++;
			continue;
		}
		size_t n = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
		if (n == 0 || i + n > len) return false;
		for (size_t k = 1; k < n; k++)
			if ((s[i + k] & 0xc0) != 0x80) return false;
		i += n;
	}
	return true;
}

/* A JSON string member's text with the common escapes decoded (\\" \\\\ \\n \\/; \\u is refused), validated. */
static bool take_text(const json_node* n, char* out, size_t cap)
{
	if (n->type != JSON_STRING) return false;
	size_t j = 0;
	for (size_t i = 0; i < n->length; i++) {
		char c = n->text[i];
		if (c == '\\') {
			if (++i >= n->length) return false;
			char e = n->text[i];
			c = e == 'n' ? '\n' : (e == '"' || e == '\\' || e == '/') ? e : 0;
			if (c == 0) return false;
		}
		if (j + 1 >= cap) return false;
		out[j++] = c;
	}
	out[j] = 0;
	return valid_text((const unsigned char*)out, j);
}

command discovery_command(const char* id, const char* topic, size_t topic_len, const unsigned char* payload,
                          size_t payload_len)
{
	command c;
	memset(&c, 0, sizeof(c));
	char prefix[64];
	int n = snprintf(prefix, sizeof(prefix), "ps5/%s/command/", id);
	if (n <= 0 || (size_t)n >= topic_len || strncmp(topic, prefix, (size_t)n) != 0) return c;
	const char* action = topic + n;
	size_t action_len = topic_len - (size_t)n;
#define ACTION(text) (action_len == strlen(text) && strncmp(action, text, action_len) == 0)
	if (ACTION("notify")) {
		/* {"title": ..., "message": ...} from Home Assistant's command template, or plain text. */
		if (payload_len > 0 && payload[0] == '{') {
			static json_document doc;
			if (!json_parse(&doc, (const char*)payload, payload_len) || doc.nodes[0].type != JSON_OBJECT) return c;
			const json_node* message = json_member(&doc, &doc.nodes[0], "message");
			const json_node* title = json_member(&doc, &doc.nodes[0], "title");
			if (message == NULL || !take_text(message, c.text, sizeof(c.text))) return c;
			if (title != NULL && title->type == JSON_STRING && !take_text(title, c.title, sizeof(c.title))) return c;
		} else {
			if (payload_len == 0 || payload_len >= sizeof(c.text) || !valid_text(payload, payload_len)) return c;
			memcpy(c.text, payload, payload_len);
			c.text[payload_len] = 0;
		}
		if (c.text[0] == 0) return c;
		c.kind = COMMAND_NOTIFY;
	} else if (ACTION("rest") || ACTION("power_off")) {
		/* Home Assistant's button sends PRESS; anything else is not a press. */
		if (payload_len == 5 && memcmp(payload, "PRESS", 5) == 0) c.kind = ACTION("rest") ? COMMAND_REST : COMMAND_POWER_OFF;
	} else if (ACTION("beep")) {
		c.kind = COMMAND_BEEP;
	} else if (ACTION("blink")) {
		c.kind = COMMAND_BLINK;
	} else if (ACTION("colour")) {
		for (int i = 0; i < COLOUR_COUNT; i++) {
			if (strlen(COLOURS[i].name) == payload_len && memcmp(COLOURS[i].name, payload, payload_len) == 0) {
				c.kind = COMMAND_COLOUR;
				c.colour = i;
			}
		}
	}
#undef ACTION
	return c;
}
