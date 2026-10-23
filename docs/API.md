# Local API

`http://127.0.0.1:9160`, on the console only (the development build also on the network). Bodies and answers are
JSON.

| Request | Body | Answer |
| --- | --- | --- |
| `GET /status` | | `{"service":"ps5-homeassistant","version":"0.3.0","state":"connected","error":"","events":""}` |
| `GET /config` | | `{"host":"...","port":1883,"username":"ps5","password_set":true,"interval":10,"power_actions":false}` |
| `PUT /config` | any of `host`, `port`, `username`, `password`, `interval`, `power_actions` | the config as `GET /config` shows it, or `400` with `{"error":"..."}` |
| `POST /quit` | | stops the payload |

`state` is one of `not configured`, `connecting`, `connected`, `disconnected`. `error` says why a connection
failed, for example `the broker refused the username or password`. `events` is empty while the live events work;
otherwise it says why the notification database is not read, or what was skipped (for example an unknown trophy
grade).

## Config rules

| Field | Rule |
| --- | --- |
| `host` | a name or IPv4 address: letters, digits, dots and hyphens, up to 253 |
| `port` | 1-65535 (default 1883) |
| `username` | up to 64 bytes, no quotes or backslashes |
| `password` | up to 128 bytes, no quotes or backslashes; left out, the stored one stays |
| `interval` | 5-300 seconds between state messages (default 10) |
| `power_actions` | `true` or `false` (default `false`): the Rest mode and Power off buttons in Home Assistant |

A `PUT` is saved to `/data/ps5-homeassistant/config.json` (0600) and the payload connects again with it.
