# SQLite

The SQLite amalgamation, version 3.46.1, as published: `sqlite3.c` and `sqlite3.h`, unchanged.

- Source: https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
- SHA-256 of that zip: `77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784`
- Licence: public domain (https://www.sqlite.org/copyright.html)

ps5-homeassistant uses it to read the console's notification list (see `src/notifications.c`). The build options
are in the Makefile (`SQLITE_FLAGS`).
