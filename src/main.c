/* ps5-homeassistant: the PS5 as a device in Home Assistant, over MQTT.
 *
 * Reads ps5-exporter (127.0.0.1:9100) and ps5-signal (127.0.0.1:9150) on the console, publishes the values
 * with Home Assistant's MQTT discovery, and carries out what HA sends: a notification on the screen, a beep,
 * a blink or a colour of the light (through ps5-signal). It also sends live events: trophies and downloads from
 * the system's notification database, games started and stopped, ShadowMountPlus storage jobs. The broker is set
 * in config.json, by Signal Companion through the local API on 127.0.0.1:9160. */
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
#include "chain.h"
#include "config.h"
#include "discovery.h"
#include "events.h"
#include "http.h"
#include "launch.h"
#include "mqtt.h"
#include "notifications.h"
#include "power.h"
#include "procs.h"
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
static char g_events_note[128] = ""; /* why an event source is not read, or what was skipped; "" when all is well */

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
		snprintf(body, cap,
		         "{\"service\":\"ps5-homeassistant\",\"version\":\"%s\",\"state\":\"%s\",\"error\":\"%s\",\"events\":\"%s\"}",
		         HA_VERSION, g_state, g_error, g_events_note);
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

/* ---- live events (events.c): the cursor lives as long as the payload, across sessions ---- */

#define EVENTS_TICK_MS 2000

static event_state g_events;
static notifications_reader g_notifications;
static volatile int g_games_due = 1; /* read ShadowMountPlus' titles again */

/* The note goes into /status between JSON quotes: quotes, backslashes and control characters become spaces. */
static void set_events_note(const char* note)
{
	pthread_mutex_lock(&g_lock);
	size_t i = 0;
	for (; note[i] && i + 1 < sizeof(g_events_note); i++) {
		unsigned char c = (unsigned char)note[i];
		g_events_note[i] = (c < 32 || c == '"' || c == '\\') ? ' ' : (char)c;
	}
	g_events_note[i] = 0;
	pthread_mutex_unlock(&g_lock);
}

static int publish_events(mqtt* m, const console_event* events, int count)
{
	char suffix[32];
	for (int i = 0; i < count; i++) {
		snprintf(suffix, sizeof(suffix), "event/%s", events[i].object);
		if (publish_text(m, topic(suffix), events[i].payload, 0) != 0) return -1;
	}
	return 0;
}

/* The notification database and ShadowMountPlus' storage job, every EVENTS_TICK_MS. */
static int check_events(mqtt* m)
{
	static notification_row rows[NOTIFICATIONS_MAX_ROWS];
	static console_event events[NOTIFICATIONS_MAX_ROWS];
	int count = 0;
	char error[128];
	notifications_result result = notifications_poll(&g_notifications, g_events.last_id, g_events.last_updated_ms, rows,
	                                                 &count, error, sizeof(error));
	if (result == NOTIFICATIONS_ERROR) set_events_note(error);
	if (result == NOTIFICATIONS_READ) {
		struct timespec wall;
		clock_gettime(CLOCK_REALTIME, &wall);
		long long wall_ms = (long long)wall.tv_sec * 1000 + wall.tv_nsec / 1000000;
		int n = events_from_notifications(&g_events, rows, count, wall_ms, events, NOTIFICATIONS_MAX_ROWS);
		set_events_note(g_events.skipped[0] ? g_events.skipped : "");
		if (publish_events(m, events, n) != 0) return -1;
	}
	storage_status storage;
	sources_read_storage(&storage);
	int n = events_from_storage(&g_events, &storage, events, NOTIFICATIONS_MAX_ROWS);
	if (n > 0) g_games_due = 1; /* a copy, move or delete can change the list of titles */
	return publish_events(m, events, n);
}

static int reading_events(mqtt* m, const readings* r)
{
	console_event events[2];
	int n = events_from_reading(&g_events, r, events, 2);
	return publish_events(m, events, n);
}

/* ---- the boot chain's payloads and the titles to start (decision log D14) ---- */

#define GAMES_REFRESH_MS (5LL * 60 * 1000)
#define PUBLISHED_PAYLOADS CONFIG_DIRECTORY "/payloads.txt" /* object ids with discovery sent, to remove them later */

static chain_payload g_chain[CHAIN_MAX];
static int g_chain_count;
static long long g_chain_mtime = -1;
static game_entry g_games[GAMES_MAX];
static int g_games_count = -1;
static char g_selected[16]; /* the title chosen in the select */
static char g_processes[PROCS_MAX][20];

/* Reads the boot chain again when its file changed (or another one is used). True when the list of payloads
   changed. */
static bool load_chain(void)
{
	char path[160], dir[128];
	struct stat st;
	long long mtime = -2;
	if (chain_locate(path, sizeof(path), dir, sizeof(dir)) && stat(path, &st) == 0)
		mtime = (long long)st.st_mtime * 1000000000LL + st.st_mtim.tv_nsec + (long long)st.st_size;
	if (mtime == g_chain_mtime) return false;
	g_chain_mtime = mtime;
	static chain_payload next[CHAIN_MAX];
	int count = 0;
	FILE* f = mtime >= 0 ? fopen(path, "r") : NULL;
	if (f) {
		static char text[16 * 1024];
		size_t n = fread(text, 1, sizeof(text), f);
		fclose(f);
		count = chain_parse(text, n, dir, next, CHAIN_MAX);
	}
	bool changed = count != g_chain_count;
	for (int i = 0; !changed && i < count; i++) changed = strcmp(next[i].object, g_chain[i].object) != 0;
	memcpy(g_chain, next, sizeof(next));
	g_chain_count = count;
	return changed;
}

/* Status and start entities for the chain; those of payloads that left the chain are removed (empty retained
   configs), using the list kept from the last time. */
static int publish_chain_discovery(mqtt* m, const readings* r)
{
	static char storage[4096];
	char t[192];
	FILE* f = fopen(PUBLISHED_PAYLOADS, "r");
	char line[64];
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		bool kept = false;
		for (int i = 0; i < g_chain_count; i++) kept |= strcmp(line, g_chain[i].object) == 0;
		if (kept || !line[0]) continue;
		for (int kind = DISCOVERY_PAYLOAD_STATUS; kind <= DISCOVERY_PAYLOAD_START; kind++) {
			discovery_payload_topic(line, kind, g_id, t, sizeof(t));
			if (publish_text(m, t, "", 1) != 0) {
				fclose(f);
				return -1;
			}
		}
	}
	if (f) fclose(f);
	mkdir(CONFIG_DIRECTORY, 0755);
	f = fopen(PUBLISHED_PAYLOADS, "w");
	for (int i = 0; i < g_chain_count; i++) {
		if (f) fprintf(f, "%s\n", g_chain[i].object);
		for (int kind = DISCOVERY_PAYLOAD_STATUS; kind <= DISCOVERY_PAYLOAD_START; kind++) {
			Buf b;
			buf_init(&b, storage, sizeof(storage));
			if (!discovery_payload_message(&g_chain[i], kind, g_id, r, HA_VERSION, t, sizeof(t), &b) || b.truncated) continue;
			if (publish_text(m, t, storage, 1) != 0) {
				if (f) fclose(f);
				return -1;
			}
		}
	}
	if (f) fclose(f);
	return 0;
}

/* Whether each chain payload runs, from the process table (not from ps5-exporter, which is one of them). */
static int publish_chain_state(mqtt* m)
{
	int count = procs_names(g_processes, PROCS_MAX);
	int running[CHAIN_MAX];
	for (int i = 0; i < g_chain_count; i++) running[i] = chain_running(&g_chain[i], (const char(*)[20])g_processes, count);
	static char storage[1024];
	Buf b;
	buf_init(&b, storage, sizeof(storage));
	discovery_payload_state(g_chain, running, g_chain_count, &b);
	if (b.truncated) return 0;
	return publish_text(m, topic("payloads"), storage, 1);
}

static int publish_games_discovery(mqtt* m, const readings* r)
{
	static char storage[24 * 1024];
	char t[192];
	Buf b;
	buf_init(&b, storage, sizeof(storage));
	if (!discovery_games_message(g_games, g_games_count, g_id, r, HA_VERSION, t, sizeof(t), &b) || b.truncated) return 0;
	return publish_text(m, t, storage, 1);
}

/* ShadowMountPlus' titles, every GAMES_REFRESH_MS and after a storage job; the select is sent again when the list
   changed. A ShadowMountPlus that does not answer keeps the last list. */
static int refresh_games(mqtt* m, const readings* r)
{
	static game_entry next[GAMES_MAX];
	int count = sources_read_games(next, GAMES_MAX);
	if (count < 0) return 0;
	if (count == g_games_count && memcmp(next, g_games, sizeof(game_entry) * (size_t)count) == 0) return 0;
	memcpy(g_games, next, sizeof(next));
	g_games_count = count;
	return publish_games_discovery(m, r);
}

static int publish_all_discovery(mqtt* m, const readings* r, bool power_actions)
{
	if (publish_discovery(m, r, power_actions) != 0 || publish_chain_discovery(m, r) != 0) return -1;
	return g_games_count >= 0 ? publish_games_discovery(m, r) : 0;
}

/* A rich notification with Home Assistant's icon, for what a start did. */
static void tell(const char* text)
{
	notify_rich("Home Assistant", text, access(ICON_PATH, R_OK) == 0 ? ICON_URL : NULL);
}

static bool game_runs(void)
{
	int count = procs_names(g_processes, PROCS_MAX);
	for (int i = 0; i < count; i++)
		if (strcmp(g_processes[i], "eboot.bin") == 0) return true;
	return false;
}

static void start_payload(const char* object)
{
	static long long last;
	char text[160], error[128];
	const chain_payload* p = NULL;
	for (int i = 0; i < g_chain_count; i++)
		if (strcmp(g_chain[i].object, object) == 0) p = &g_chain[i];
	if (!p || !p->can_start) return; /* not a button this payload offers */
	if (last && now_ms() - last < 10000) return;
	last = now_ms();
	int count = procs_names(g_processes, PROCS_MAX);
	int running = chain_running(p, (const char(*)[20])g_processes, count);
	if (running != 0) {
		snprintf(text, sizeof(text), running > 0 ? "%s runs already" : "%s: cannot tell whether it runs", p->name);
		tell(text);
		return;
	}
	if (launch_payload(p->path, error, sizeof(error)) != 0) snprintf(text, sizeof(text), "%s did not start: %s", p->name, error);
	else snprintf(text, sizeof(text), "Starting %s", p->name);
	tell(text);
}

static void start_game(void)
{
	static long long last;
	char text[200], error[128];
	if (last && now_ms() - last < 10000) return;
	last = now_ms();
	if (!g_selected[0]) {
		tell("Choose a game to start first");
		return;
	}
	const char* name = g_selected;
	for (int i = 0; i < g_games_count; i++)
		if (strcmp(g_games[i].title_id, g_selected) == 0) name = g_games[i].name;
	if (game_runs()) {
		snprintf(text, sizeof(text), "A game runs: close it first to start %s", name);
		tell(text);
		return;
	}
	int code = 0;
	if (launch_title(g_selected, &code, error, sizeof(error)) != 0) {
		/* A start once answered non-zero and still worked (decision log D14): look before saying it failed. */
		sleep(3);
		if (code == 0 || !game_runs()) {
			snprintf(text, sizeof(text), "%s did not start: %s", name, error);
			tell(text);
		}
		return;
	}
	snprintf(text, sizeof(text), "Starting %s", name);
	tell(text);
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
	case COMMAND_START_PAYLOAD:
		start_payload(c->text);
		break;
	case COMMAND_SELECT_GAME:
		snprintf(g_selected, sizeof(g_selected), "%s", c->text);
		break;
	case COMMAND_START_GAME:
		start_game();
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
	if (reading_events(&m, &r) != 0) {
		mqtt_close(&m);
		set_status("disconnected", "the connection dropped");
		return -1;
	}
	session_context context = { &m, c };
	load_chain();
	if (publish_all_discovery(&m, &r, c->power_actions) != 0 || publish_text(&m, will, "online", 1) != 0 ||
	    mqtt_subscribe(&m, topic("command/#")) != 0 || mqtt_subscribe(&m, DISCOVERY_PREFIX "/status") != 0 ||
	    publish_state(&m, &r) != 0 || publish_chain_state(&m) != 0) {
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
	long long next_events = now_ms();
	long long next_games = now_ms();
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
			if (publish_all_discovery(&m, &r, c->power_actions) != 0) break;
		}
		if (now_ms() >= next) {
			sources_read(&r);
			if (publish_state(&m, &r) != 0 || reading_events(&m, &r) != 0) break;
			if (load_chain() && publish_chain_discovery(&m, &r) != 0) break;
			if (publish_chain_state(&m) != 0) break;
			next = now_ms() + (long long)c->interval * 1000;
		}
		if (g_games_due || now_ms() >= next_games) {
			g_games_due = 0;
			if (refresh_games(&m, &r) != 0) break;
			next_games = now_ms() + GAMES_REFRESH_MS;
		}
		if (now_ms() >= next_events) {
			if (check_events(&m) != 0) break;
			next_events = now_ms() + EVENTS_TICK_MS;
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
	events_init(&g_events);
	notifications_init(&g_notifications, NOTIFICATIONS_PATH);
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
