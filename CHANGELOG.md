# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.0.0/), and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [v0.3.0] - 2026-10-23

### Added
- Live events: four event entities for automations. Trophy (`bronze`, `silver`, `gold`, `platinum`) and Download
  (`installed`, `ready`, `addon_ready`, `update_added`, `other`) come from the console's notification list, read from
  a copy every 2 seconds and never locked or written. Game started or stopped comes from ps5-exporter. Storage job
  (`completed`, `failed`, `cancelled`) comes from ShadowMountPlus' copy, move, unpack and delete jobs. Nothing fires
  for what happened before the payload started, or for events older than 10 minutes after a lost connection.
- A trophy event also names the game that runs (`title_id`, `game`), from ps5-exporter's last reading.
- Free space of the extended storage (`/mnt/ext1`) and the first USB drive (`/mnt/usb0`).
- A diagnostic sensor per payload in ps5-autoloader's boot chain, and a Start button for each payload that is not
  running (not for etaHEN, kstuff or ps5-homeassistant itself).
- Start a game from Home Assistant: a select with every title ShadowMountPlus lists, and a Start game button. It
  refuses while a game runs.
- `GET /status` on the local API has an `events` field: empty, or why the live events are not read.

### Changed
- The payload bundles SQLite 3.46.1 (public domain) to read the notification list. The ELF grows from 168 KB to
  about 1.07 MB.

## [v0.2.1] - 2026-10-19

### Changed
- The process is named `ps5-hass.elf` (a process name has at most 19 characters). Started through an ELF loader's
  port it showed as `payload.elf` in process lists such as pldmgr's and PayLoader's.

## [v0.2.0] - 2026-10-19

### Added
- Rest mode and Power off buttons, off by default: switched on with `power_actions` in the config. Each beeps (when
  ps5-signal runs) and shows a notification first, marks the device offline, and uses the system's own services.
  Only `PRESS` counts, at most one per 10 seconds.
- IP address sensor (diagnostic) that keeps its last value while the console sleeps.

### Fixed
- After waking from rest mode the device could stay unavailable in Home Assistant: a late `offline` (the broker's last
  will for the broken connection) now gets undone, because the payload publishes `online` with every state.

## [v0.1.0] - 2026-10-18

### Added
- MQTT 3.1.1 client (QoS 0, last will, keepalive), reconnecting with backoff and after rest mode.
- Home Assistant discovery: a "PS5" device with temperature, power, GPU power, fan, CPU load, GPU clock, frame rate,
  game, uptime and free space from ps5-exporter, and whether ps5-exporter and ps5-signal answer.
- A notification on the TV from Home Assistant: a beep, then the title, the message and Home Assistant's icon.
- Beep, blink and the light's colour through ps5-signal.
- Local API on 127.0.0.1:9160 for the broker settings.
- A version marker in the ELF file (`PS5HA_VER:0.1.0`).
