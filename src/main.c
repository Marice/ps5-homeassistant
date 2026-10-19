/* ps5-homeassistant: the PS5 as a device in Home Assistant, over MQTT.
 *
 * Reads ps5-exporter (127.0.0.1:9100) and ps5-signal (127.0.0.1:9150) on the console, publishes the values
 * with Home Assistant's MQTT discovery, and carries out what HA sends: a notification on the screen, a beep,
 * a blink or a colour of the light (through ps5-signal). The broker is set in config.json, by Signal
 * Companion through the local API on 127.0.0.1:9160. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ifaddrs.h>
#include <net/if_dl.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "buf.h"
#include "config.h"
#include "discovery.h"
#include "http.h"
#include "mqtt.h"
#include "power.h"
#include "sources.h"

#ifndef HA_VERSION
#define HA_VERSION "0.0.0-dev"
#endif
#define API_PORT 9160

/* The version in the file itself, so Signal Companion can tell an older copy from its own. */
__attribute__((used)) static const volatile char version_marker[] = "PS5HA_VER:" HA_VERSION;

int sceKernelSendNotificationRequest(uint32_t device, void* request, size_t size, int blocking);

struct notification_request {
	uint8_t reserved[45];
	char message[3075];
};

static void notify(const char* text)
{
	static struct notification_request req;
	memset(&req, 0, sizeof(req));
	snprintf(req.message, sizeof(req.message), "%s", text);
	(void)sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* A rich notification: a title, a line under it and an icon, with the system's InteractiveToastTemplateB. Shape
   from the payload SDK's notify sample (LightningMods, GPL-3.0). libSceNotification is opened on first use, so a
   firmware without it falls back to the plain notification instead of stopping the payload at load. */
#define NOTIFICATION_USER_SYSTEM 0xFE

typedef int (*notification_send_fn)(int user_id, bool is_logged, const char* payload);

static void add_json_text(Buf* b, const char* text)
{
	for (size_t i = 0; text[i]; i++) {
		unsigned char c = (unsigned char)text[i];
		if (c == '"' || c == '\\') buf_addf(b, "\\%c", c);
		else if (c == '\n') buf_addf(b, "\\n");
		else if (c >= 32) buf_addf(b, "%c", c);
	}
}

static int send_rich(const char* payload)
{
	static notification_send_fn send;
	static int tried;
	if (!tried) {
		tried = 1;
		void* library = dlopen("libSceNotification.sprx", RTLD_LAZY);
		if (library) send = (notification_send_fn)dlsym(library, "sceNotificationSend");
	}
	return send ? send(NOTIFICATION_USER_SYSTEM, true, payload) : -1;
}

/* The icon, fetched once from the Home Assistant the broker runs on (its own favicon), so the logo is not shipped
   with the payload. It sits next to config.json: the system UI draws the notification and must be able to read
   it, so the folder and the icon are readable by all; config.json, which holds the password, is not (0600). */
#define ICON_PATH CONFIG_DIRECTORY "/home-assistant.png"
/* The same file as the system UI names it: /data is the payload's name for /user/data, and the system UI only
   knows the latter (an icon at /data/... is not shown). */
#define ICON_URL "/user/data/ps5-homeassistant/home-assistant.png"
#define HOME_ASSISTANT_WEB_PORT 8123

static void fetch_icon(const char* host)
{
	if (access(ICON_PATH, R_OK) == 0) return;
	mkdir(CONFIG_DIRECTORY, 0755);
	chmod(CONFIG_DIRECTORY, 0755);
	if (http_download(host, HOME_ASSISTANT_WEB_PORT, "/static/icons/favicon-192x192.png", ICON_PATH, 64 * 1024, 5000) == 0)
		chmod(ICON_PATH, 0644);
}

static void notify_rich(const char* title, const char* body, const char* icon)
{
	static char storage[4096];
	Buf b;
	buf_init(&b, storage, sizeof(storage));
	buf_addf(&b, "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\",\"channelType\":\"Downloads\","
	             "\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,\"viewData\":{");
	if (icon && icon[0]) {
		buf_addf(&b, "\"icon\":{\"type\":\"Url\",\"parameters\":{\"url\":\"");
		add_json_text(&b, icon);
		buf_addf(&b, "\"}},");
	}
	buf_addf(&b, "\"message\":{\"body\":\"");
	add_json_text(&b, title);
	buf_addf(&b, "\"},\"subMessage\":{\"body\":\"");
	add_json_text(&b, body);
	buf_addf(&b, "\"}},\"platformViews\":{\"previewDisabled\":{\"viewData\":{\"message\":{\"body\":\"");
	add_json_text(&b, title);
	buf_addf(&b, "\"}}}}},\"localNotificationId\":\"%ld\"}", (long)time(NULL));
	if (b.truncated || send_rich(storage) != 0) notify(body);
}

/* ---- shared state between the API thread and the MQTT loop ---- */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static config g_config;
static volatile int g_reconfigured; /* the config changed: connect again */
static volatile int g_quit;
static char g_state[48] = "not configured";
static char g_error[128] = "";

static void set_status(const char* state, const char* error)
{
	pthread_mutex_lock(&g_lock);
	snprintf(g_state, sizeof(g_state), "%s", state);
	snprintf(g_error, sizeof(g_error), "%s", error);
	pthread_mutex_unlock(&g_lock);
}

/* ---- local API ---- */

static int handle(const char* method, const char* path, const char* request_body, char* body, size_t cap)
{
	if (strcmp(path, "/status") == 0 && strcmp(method, "GET") == 0) {
		pthread_mutex_lock(&g_lock);
		snprintf(body, cap, "{\"service\":\"ps5-homeassistant\",\"version\":\"%s\",\"state\":\"%s\",\"error\":\"%s\"}",
		         HA_VERSION, g_state, g_error);
		pthread_mutex_unlock(&g_lock);
		return 200;
	}
	if (strcmp(path, "/config") == 0 && strcmp(method, "GET") == 0) {
		pthread_mutex_lock(&g_lock);
		config_public_json(&g_config, body, cap);
		pthread_mutex_unlock(&g_lock);
		return 200;
	}
	if (strcmp(path, "/config") == 0 && strcmp(method, "PUT") == 0) {
		char error[128];
		pthread_mutex_lock(&g_lock);
		config next = g_config;
		bool ok = config_parse(request_body, strlen(request_body), &next, error, sizeof(error)) &&
		          config_save(&next, error, sizeof(error));
		if (ok) {
			g_config = next;
			g_reconfigured = 1;
			config_public_json(&g_config, body, cap);
		}
		pthread_mutex_unlock(&g_lock);
		if (!ok) {
			snprintf(body, cap, "{\"error\":\"%s\"}", error);
			return 400;
		}
		return 200;
	}
#if defined(HA_DEV)
	/* Development build: a raw notification template, to try layouts from a PC. */
	if (strcmp(path, "/toast") == 0 && strcmp(method, "POST") == 0) {
		snprintf(body, cap, "{\"code\":%d}", send_rich(request_body));
		return 200;
	}
#endif
	if (strcmp(path, "/quit") == 0 && strcmp(method, "POST") == 0) {
		g_quit = 1;
		snprintf(body, cap, "{}");
		return 200;
	}
	snprintf(body, cap, "{\"error\":\"not found\"}");
	return 404;
}

static void* serve_api(void* unused)
{
	(void)unused;
	while (!g_quit) {
#if defined(HA_DEV)
		const int any_address = 1; /* development build: also on the network, for testing from a PC */
#else
		const int any_address = 0;
#endif
		if (http_serve(API_PORT, any_address, handle, &g_quit) != 0) sleep(1); /* busy port, or rest mode closed it */
	}
	return NULL;
}

/* ---- MQTT ---- */

static char g_id[16];
static char g_topic[160];

/* The console's id: the MAC address of its first interface with one, without colons. */
static void console_id(void)
{
	snprintf(g_id, sizeof(g_id), "unknown");
	struct ifaddrs* list = NULL;
	if (getifaddrs(&list) != 0) return;
	for (struct ifaddrs* a = list; a; a = a->ifa_next) {
		if (!a->ifa_addr || a->ifa_addr->sa_family != AF_LINK || strncmp(a->ifa_name, "lo", 2) == 0) continue;
		const struct sockaddr_dl* dl = (const struct sockaddr_dl*)a->ifa_addr;
		if (dl->sdl_alen != 6) continue;
		const unsigned char* m = (const unsigned char*)LLADDR(dl);
		snprintf(g_id, sizeof(g_id), "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
		break;
	}
	freeifaddrs(list);
}

static const char* topic(const char* suffix)
{
	snprintf(g_topic, sizeof(g_topic), "ps5/%s/%s", g_id, suffix);
	return g_topic;
}

static int publish_text(mqtt* m, const char* t, const char* text, int retain)
{
	return mqtt_publish(m, t, text, strlen(text), retain);
}

static int publish_discovery(mqtt* m, const readings* r, bool power_actions)
{
	static char storage[4096];
	char t[192];
	for (int i = 0; i < discovery_count(); i++) {
		Buf b;
		buf_init(&b, storage, sizeof(storage));
		if (!discovery_message(i, g_id, r, power_actions, HA_VERSION, t, sizeof(t), &b) || b.truncated) continue;
		if (publish_text(m, t, storage, 1) != 0) return -1;
	}
	return 0;
}

static int publish_state(mqtt* m, const readings* r)
{
	static char storage[2048];
	Buf b;
	buf_init(&b, storage, sizeof(storage));
	discovery_state(r, &b);
	if (b.truncated) return 0;
	if (publish_text(m, topic("state"), storage, 1) != 0) return -1;
	/* online again with every state: an "offline" that arrived late (the broker's last will for a connection
	   that rest mode broke, or the one sent before a power action) is undone within one interval. */
	if (publish_text(m, topic("availability"), "online", 1) != 0) return -1;
	return publish_text(m, topic("signal"), r->signal ? "online" : "offline", 1);
}

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Rest mode or power off, when allowed: beeps, says so on the TV, marks the device offline (the session will not
   say goodbye from rest mode), then asks the system. */
static void power_action(const command* c, mqtt* m, const config* conf)
{
	static long long last;
	const char* reason;
	const bool rest = c->kind == COMMAND_REST;
	if (!power_allowed(conf->power_actions, last, now_ms(), &reason)) {
		printf("ps5-homeassistant: %s refused: %s\n", rest ? "rest mode" : "power off", reason);
		return;
	}
	last = now_ms();
	/* A beep first, as for a notification, so it is heard even with the TV off (when ps5-signal runs). */
	char answer[256];
	http_request(SIGNAL_PORT, "POST", "/beep", "{\"pattern\":[{\"tone\":1}]}", answer, sizeof(answer), 1000);
	notify_rich("Home Assistant", rest ? "Rest mode" : "Turning off", access(ICON_PATH, R_OK) == 0 ? ICON_URL : NULL);
	char will[64];
	snprintf(will, sizeof(will), "ps5/%s/availability", g_id);
	publish_text(m, will, "offline", 1);
	sleep(2); /* the notification shows, and the message leaves */
	int rc = rest ? power_rest() : power_off();
	if (rc != 0) {
		printf("ps5-homeassistant: %s failed: 0x%08x\n", rest ? "rest mode" : "power off", (unsigned)rc);
		publish_text(m, will, "online", 1);
		notify(rest ? "ps5-homeassistant: rest mode did not start" : "ps5-homeassistant: the console did not turn off");
	}
}

typedef struct {
	mqtt* m;
	const config* conf;
} session_context;

/* Carries out a command. Notifications at most one per 2 seconds; ps5-signal limits the rest itself. */
static void run(const command* c, const session_context* s)
{
	static long long last_notify;
	char answer[512];
	switch (c->kind) {
	case COMMAND_NOTIFY:
		if (now_ms() - last_notify < 2000) return;
		last_notify = now_ms();
		/* A beep first, so the message is heard before it is read (when ps5-signal runs). */
		http_request(SIGNAL_PORT, "POST", "/beep", "{\"pattern\":[{\"tone\":1}]}", answer, sizeof(answer), 1000);
		notify_rich(c->title[0] ? c->title : "Home Assistant", c->text, access(ICON_PATH, R_OK) == 0 ? ICON_URL : NULL);
		break;
	case COMMAND_BEEP:
		http_request(SIGNAL_PORT, "POST", "/beep", "{\"pattern\":[{\"tone\":1}]}", answer, sizeof(answer), 1000);
		break;
	case COMMAND_BLINK:
		http_request(SIGNAL_PORT, "POST", "/led",
		             "{\"pattern\":[{\"color\":\"orange\",\"ms\":400},{\"color\":\"off\",\"ms\":400}],\"repeat\":3}", answer,
		             sizeof(answer), 1000);
		break;
	case COMMAND_COLOUR: {
		char body[96];
		if (c->colour == 0) {
			snprintf(body, sizeof(body), "{\"system\":true}");
		} else {
			int b, w, a;
			discovery_colour_levels(c->colour, &b, &w, &a);
			snprintf(body, sizeof(body), "{\"blue\":%d,\"white\":%d,\"amber\":%d}", b, w, a);
		}
		http_request(SIGNAL_PORT, "POST", "/color", body, answer, sizeof(answer), 1000);
		break;
	}
	case COMMAND_REST:
	case COMMAND_POWER_OFF:
		power_action(c, s->m, s->conf);
		break;
	default:
		break;
	}
}

static volatile int g_rediscover; /* Home Assistant came back: send discovery again */

static void on_message(const char* t, size_t t_len, const unsigned char* payload, size_t len, void* context)
{
	static const char status[] = DISCOVERY_PREFIX "/status";
	if (t_len == sizeof(status) - 1 && memcmp(t, status, t_len) == 0) {
		if (len == 6 && memcmp(payload, "online", 6) == 0) g_rediscover = 1;
		return;
	}
	command c = discovery_command(g_id, t, t_len, payload, len);
	run(&c, (const session_context*)context);
}

/* One connection: until it drops, the config changes, or the console slept. Returns 0 when it was used. */
static int session(const config* c)
{
	static mqtt m;
	char client[32], will[64];
	snprintf(client, sizeof(client), "ps5-%s", g_id);
	snprintf(will, sizeof(will), "ps5/%s/availability", g_id);
	mqtt_options o = { client, c->username, c->password, will, "offline", 30 };
	set_status("connecting", "");
	int rc = mqtt_connect(&m, c->host, c->port, &o, 5000);
	if (rc != 0) {
		set_status("disconnected", rc == 4 || rc == 5 ? "the broker refused the username or password"
		                               : rc > 0     ? "the broker refused the connection"
		                                            : "the broker does not answer");
		return -1;
	}
	readings r;
	sources_read(&r);
	session_context context = { &m, c };
	if (publish_discovery(&m, &r, c->power_actions) != 0 || publish_text(&m, will, "online", 1) != 0 ||
	    mqtt_subscribe(&m, topic("command/#")) != 0 || mqtt_subscribe(&m, DISCOVERY_PREFIX "/status") != 0 ||
	    publish_state(&m, &r) != 0) {
		mqtt_close(&m);
		set_status("disconnected", "the connection dropped");
		return -1;
	}
	set_status("connected", "");
	fetch_icon(c->host);
	static int greeted;
	if (!greeted) {
		greeted = 1;
		notify("ps5-homeassistant: connected to Home Assistant");
	}
	long long next = now_ms() + (long long)c->interval * 1000;
	time_t wall = time(NULL);
	for (;;) {
		if (g_quit || g_reconfigured) break;
		/* A jump of the wall clock: the console slept, and rest mode closed the socket without telling. */
		time_t now = time(NULL);
		if (now - wall > 5) break;
		wall = now;
		if (mqtt_poll(&m, 500, on_message, &context) != 0) break;
		if (g_rediscover) {
			g_rediscover = 0;
			if (publish_discovery(&m, &r, c->power_actions) != 0) break;
		}
		if (now_ms() >= next) {
			sources_read(&r);
			if (publish_state(&m, &r) != 0) break;
			next = now_ms() + (long long)c->interval * 1000;
		}
	}
	/* A clean goodbye: the will only fires when the connection is lost. */
	publish_text(&m, will, "offline", 1);
	mqtt_close(&m);
	set_status("disconnected", "");
	return 0;
}

/* In libkernel. The ELF loader names a payload sent to it "payload.elf"; this name is what process lists
   (pldmgr, PayLoader) show instead. At most 19 characters, ending in ".elf" so they count it as a payload, hence
   not "ps5-homeassistant.elf". */
int sceKernelSetProcessName(const char* name);

int main(void)
{
	sceKernelSetProcessName("ps5-hass.elf");
	signal(SIGPIPE, SIG_IGN);
	console_id();
	char error[128];
	if (!config_load(&g_config, error, sizeof(error))) {
		set_status("not configured", error);
		config_defaults(&g_config);
	}
	pthread_t api;
	pthread_create(&api, NULL, serve_api, NULL);
	int backoff = 1;
	while (!g_quit) {
		pthread_mutex_lock(&g_lock);
		config c = g_config;
		g_reconfigured = 0;
		pthread_mutex_unlock(&g_lock);
		if (!config_ready(&c)) {
			set_status("not configured", "");
			sleep(1);
			continue;
		}
		if (session(&c) == 0) {
			backoff = 1;
			continue;
		}
		/* Wait before the next try, but not past a new config. */
		for (int i = 0; i < backoff && !g_quit && !g_reconfigured; i++) sleep(1);
		if (backoff < 60) backoff *= 2;
	}
	pthread_join(api, NULL);
	return 0;
}
