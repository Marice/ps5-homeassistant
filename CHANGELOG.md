# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.0.0/), and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [v0.1.0] - 2026-10-18

### Added
- MQTT 3.1.1 client (QoS 0, last will, keepalive), reconnecting with backoff and after rest mode.
- Home Assistant discovery: a "PS5" device with temperature, power, GPU power, fan, CPU load, GPU clock, frame rate,
  game, uptime and free space from ps5-exporter, and whether ps5-exporter and ps5-signal answer.
- A notification on the TV from Home Assistant: a beep, then the title, the message and Home Assistant's icon.
- Beep, blink and the light's colour through ps5-signal.
- Local API on 127.0.0.1:9160 for the broker settings.
- A version marker in the ELF file (`PS5HA_VER:0.1.0`).
