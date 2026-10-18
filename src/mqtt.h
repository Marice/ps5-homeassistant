/* A small MQTT 3.1.1 client: CONNECT with a last will, PUBLISH and SUBSCRIBE at QoS 0, PINGREQ.
 *
 * Written from the OASIS MQTT 3.1.1 specification. QoS 0 only: the bridge publishes state every
 * interval and retains it, so a lost message is replaced by the next one. The packet builders and
 * the parser are pure functions (tested on the host); mqtt_connect and mqtt_poll do the socket I/O. */
#ifndef MQTT_H
#define MQTT_H

#include <stddef.h>
#include <stdint.h>

#define MQTT_MAX_PACKET (64 * 1024)

typedef struct {
	int fd;
	uint16_t next_id;
	long long last_sent_ms;
	int keepalive_s;
	unsigned char rx[MQTT_MAX_PACKET];
	size_t rx_len;
} mqtt;

typedef struct {
	const char* client_id;
	const char* username;   /* NULL or "" for none */
	const char* password;   /* NULL or "" for none */
	const char* will_topic; /* retained, QoS 0 */
	const char* will_message;
	int keepalive_s;
} mqtt_options;

/* Called for every PUBLISH the broker sends. topic and payload are not NUL-terminated. */
typedef void (*mqtt_message_fn)(const char* topic, size_t topic_len, const unsigned char* payload, size_t payload_len,
                                void* context);

/* ---- pure packet code ---- */

/* Writes the remaining-length field; returns its size (1-4), or 0 when length is above the maximum. */
size_t mqtt_encode_length(size_t length, unsigned char out[4]);
/* Reads a remaining-length field: returns its size, 0 when more bytes are needed, -1 when malformed. */
int mqtt_decode_length(const unsigned char* in, size_t available, size_t* length);
/* Each builder writes one whole packet into out and returns its size, or 0 when it does not fit. */
size_t mqtt_build_connect(unsigned char* out, size_t cap, const mqtt_options* options);
size_t mqtt_build_publish(unsigned char* out, size_t cap, const char* topic, const void* payload, size_t payload_len,
                          int retain);
size_t mqtt_build_subscribe(unsigned char* out, size_t cap, uint16_t packet_id, const char* filter);
size_t mqtt_build_pingreq(unsigned char* out, size_t cap);
size_t mqtt_build_disconnect(unsigned char* out, size_t cap);
/* The first whole packet in a buffer: its type (high nibble of the first byte), where its body starts and how
   long it is. Returns the packet's total size, 0 when it is not complete yet, -1 when malformed or too large. */
long mqtt_parse_packet(const unsigned char* in, size_t available, int* type, size_t* body_offset, size_t* body_len);

/* ---- socket I/O ---- */

/* Connects to host (name or IPv4) and port, sends CONNECT and waits for CONNACK. Returns 0, a CONNACK return
   code (1-5: refused by the broker, 4 or 5 is a wrong username/password), or -1 on a network error. */
int mqtt_connect(mqtt* m, const char* host, int port, const mqtt_options* options, int timeout_ms);
int mqtt_publish(mqtt* m, const char* topic, const void* payload, size_t payload_len, int retain);
int mqtt_subscribe(mqtt* m, const char* filter);
/* Waits up to timeout_ms for packets, hands PUBLISHes to on_message, answers the keepalive.
   Returns 0, or -1 when the connection is gone. */
int mqtt_poll(mqtt* m, int timeout_ms, mqtt_message_fn on_message, void* context);
void mqtt_close(mqtt* m);

#endif
