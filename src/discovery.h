/* Home Assistant MQTT discovery: the "PS5" device, its entities, the state message and the commands.
 *
 * Topics, for a console with id <id> (its MAC address without colons):
 *   ps5/<id>/availability       online / offline (retained; offline is the last will)
 *   ps5/<id>/signal             online / offline: whether ps5-signal answers (the light and beeper actions)
 *   ps5/<id>/state              one JSON object with every value (retained)
 *   ps5/<id>/command/<action>   notify (text), beep, blink (any payload), colour (a preset name),
 *                               rest, power_off (PRESS; only while power actions are on)
 *   ps5/<id>/event/<object>     trophy, download, game, storage: {"event_type": ..., ...} (not retained)
 *   homeassistant/<component>/ps5_<id>/<object>/config   discovery (retained) */
#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>

#include "buf.h"
#include "sources.h"

#define DISCOVERY_PREFIX "homeassistant"

/* How many discovery messages there are. */
int discovery_count(void);
/* Discovery message index: its topic and its JSON config. False past the last one. The rest mode and power off
   buttons get an empty payload while power_actions is off: retained, that removes them from Home Assistant. */
bool discovery_message(int index, const char* id, const readings* r, bool power_actions, const char* version, char* topic,
                       size_t topic_cap, Buf* payload);
/* The state message. */
void discovery_state(const readings* r, Buf* payload);

typedef enum {
	COMMAND_NONE,
	COMMAND_NOTIFY,
	COMMAND_BEEP,
	COMMAND_BLINK,
	COMMAND_COLOUR,
	COMMAND_REST,
	COMMAND_POWER_OFF
} command_kind;

typedef struct {
	command_kind kind;
	char text[257];     /* COMMAND_NOTIFY: the message */
	char title[65];     /* COMMAND_NOTIFY: its title, "" for the default */
	int colour;         /* COMMAND_COLOUR: index into the presets */
} command;

/* The light presets the "Light colour" select offers. Index 0 is "System" (the console's own light). */
int discovery_colour_count(void);
const char* discovery_colour_name(int index);
/* The blue, white and amber levels of a preset (index > 0). */
void discovery_colour_levels(int index, int* blue, int* white, int* amber);

/* Checks a message on ps5/<id>/command/<action>: the action from the topic, the payload validated.
   COMMAND_NONE when it is not a command or not valid (too long, not UTF-8, an unknown preset). */
command discovery_command(const char* id, const char* topic, size_t topic_len, const unsigned char* payload,
                          size_t payload_len);

#endif
