# ps5-homeassistant

Your jailbroken PS5 as a device in [Home Assistant](https://www.home-assistant.io/), from inside the console.

A payload that runs next to [ps5-exporter](https://github.com/Marice/ps5-exporter) and
[ps5-signal](https://github.com/Marice/ps5-signal). It publishes the console's values over MQTT, with Home
Assistant's discovery, so a "PS5" device appears by itself. Home Assistant can send a notification to the TV,
beep, blink the light or change its colour.

Other integrations (such as ps5-mqtt) run on a PC and talk to the console over PSN. They know whether it is on and
what it plays. This one runs on the console: it also reports temperatures, power, the frame rate and the fan, and
it can show a notification on the screen.

## What Home Assistant gets

| Entity | Type | From |
| --- | --- | --- |
| Temperature (hottest sensor), Power (SoC and memory), GPU power, Fan, CPU load, GPU clock, Frame rate | sensor | ps5-exporter 0.3.0 or later |
| Game (its name from ShadowMountPlus, else the title ID), Game title ID, Game running | sensor, binary_sensor | ps5-exporter |
| Uptime, Free space (internal storage) | sensor | ps5-exporter |
| ps5-exporter, ps5-signal | binary_sensor (diagnostic) | whether each answers |
| Notification | notify | the console |
| Beep, Blink the light | button | ps5-signal |
| Light colour (System, Blue, White, Orange, Purple, Pink, Off) | select | ps5-signal |

A value the console does not report shows as *Unknown*, never as 0. Without ps5-signal the light and beep entities
show as unavailable. When the console sleeps (rest mode) or the payload stops, the whole device shows as
unavailable, and it comes back after waking.

## A notification on the TV

A notification starts with a beep (when ps5-signal runs), then shows the title, the message and Home Assistant's
icon. Emoji work.

```yaml
action: notify.send_message
target:
  entity_id: notify.ps5_notification
data:
  title: Front door
  message: "🔔 Someone is at the door"
```

Without a title the notification says "Home Assistant". Messages are limited to 256 bytes, and to one every
2 seconds.

The icon is fetched once from your own Home Assistant (`http://<broker host>:8123/static/icons/favicon-192x192.png`),
so it is not shipped with the payload. When that fails, the notification shows without it.

## Installing

1. In Home Assistant, install the **Mosquitto broker** add-on (Settings, Add-ons), and create a user for the console
   (Settings, People, Users), for example `ps5`.
2. Load `ps5-homeassistant.elf` with your ELF loader, or put it in ps5-autoloader's list between ps5-exporter and
   ps5-signal. Signal Companion can do this for you.
3. Set the broker, in Signal Companion, or from a program on the console:

   ```sh
   curl -X PUT http://127.0.0.1:9160/config \
     -d '{"host":"192.168.1.10","username":"ps5","password":"..."}'
   ```

The device appears in Home Assistant under Settings, Devices and services, MQTT, as "PS5".

## Security

- **No TLS.** MQTT to the broker is plain, as is usual inside a home network. Keep the broker off the internet.
- **The password** is stored in `/data/ps5-homeassistant/config.json`, readable by its owner only (0600). The API
  never returns it.
- **The local API** (`127.0.0.1:9160`) answers on the console only.
- **Commands** come from anyone who may publish on the broker: give the console's user its own password. Messages
  are checked (length, UTF-8, known colours), and ps5-signal limits beeps and light changes.

## Topics

For a console with id `<id>` (its MAC address without colons):

| Topic | |
| --- | --- |
| `ps5/<id>/availability` | `online` / `offline`, retained; `offline` is also the last will |
| `ps5/<id>/signal` | `online` / `offline`: whether ps5-signal answers |
| `ps5/<id>/state` | one JSON object with every value, every 10 seconds (5-300, `interval` in the config) |
| `ps5/<id>/command/notify` | `{"title": ..., "message": ...}` or plain text |
| `ps5/<id>/command/beep`, `/blink` | any payload |
| `ps5/<id>/command/colour` | a colour name from the list above |
| `homeassistant/<component>/ps5_<id>/<object>/config` | discovery, retained; sent again when Home Assistant restarts |

[docs/API.md](docs/API.md) describes the local API.

## Tested

On one console: PS5 Pro (CFI-7121), firmware 13.60, with Home Assistant's Mosquitto add-on. Other firmware: likely,
not verified.

## Building

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make           # ps5-homeassistant.elf
make dev       # ps5-homeassistant-dev.elf: the API also on the network, for testing from a PC; never release it
make test      # host tests with AddressSanitizer and UBSan (gcc, no SDK)
```

## Thanks

- The rich notification follows the payload SDK's `notify` sample by LightningMods, and
  [phantomptr/ps5upload](https://github.com/phantomptr/ps5upload) for showing it in use.
- The values come from [ps5-exporter](https://github.com/Marice/ps5-exporter); the light and beeper from
  [ps5-signal](https://github.com/Marice/ps5-signal).
- MQTT 3.1.1 is written from the OASIS specification.

## Licence

GPL-3.0-or-later. See `LICENSE`.
