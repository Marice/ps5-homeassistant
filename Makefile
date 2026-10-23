# ps5-homeassistant: build with the ps5-payload-dev SDK (PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk).
#   make           ps5-homeassistant.elf      the local API on 127.0.0.1 only
#   make dev       ps5-homeassistant-dev.elf  the API also on the network, for testing from a PC; never release it
#   make test      host tests with AddressSanitizer and UBSan (gcc, no SDK)
PS5_HOST ?= 192.168.68.125

TITLE   := ps5-homeassistant
VERSION := 0.3.0

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

CFLAGS := -Wall -Wextra -O2 -std=c11 -D_DEFAULT_SOURCE -DHA_VERSION='"$(VERSION)"'
LDLIBS := -lkernel_sys -lSceSystemService -lpthread
SRCS   := src/main.c src/mqtt.c src/config.c src/json.c src/buf.c src/http.c src/sources.c src/discovery.c src/power.c \
          src/events.c src/notifications.c src/chain.c src/procs.c src/launch.c
HDRS   := $(wildcard src/*.h)

# SQLite (public domain, third_party/sqlite) reads the system's notification database. Built once into its own
# object, without the project's warnings (upstream code, left as it is), optimised for size: -Os takes the ELF from 1.4 to
# 1.07 MB. No --gc-sections: it drops the version marker Signal Companion reads.
SQLITE_FLAGS := -Os -w -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 \
                -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_DQS=0
SQLITE_SRC   := third_party/sqlite/sqlite3.c

HOST_CC ?= gcc
HOST_FLAGS := -Wall -Wextra -O1 -std=c11 -D_DEFAULT_SOURCE -fsanitize=address,undefined -fno-sanitize-recover=all

all: $(TITLE).elf

require-sdk:
	@test -n "$(PS5_PAYLOAD_SDK)" || { echo "PS5_PAYLOAD_SDK is undefined; export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk"; exit 1; }

build/sqlite3.o: $(SQLITE_SRC) | require-sdk
	@mkdir -p build
	$(CC) $(SQLITE_FLAGS) -c -o $@ $<

build/host/sqlite3.o: $(SQLITE_SRC)
	@mkdir -p build/host
	$(HOST_CC) $(SQLITE_FLAGS) -c -o $@ $<

$(TITLE).elf: require-sdk $(SRCS) $(HDRS) build/sqlite3.o
	$(CC) $(CFLAGS) -o $@ $(SRCS) build/sqlite3.o $(LDLIBS)

dev: $(TITLE)-dev.elf

$(TITLE)-dev.elf: require-sdk $(SRCS) $(HDRS) build/sqlite3.o
	$(CC) $(CFLAGS) -DHA_DEV -o $@ $(SRCS) build/sqlite3.o $(LDLIBS)

test: build/host/sqlite3.o
	@mkdir -p build/config
	@$(HOST_CC) $(HOST_FLAGS) -DCONFIG_DIRECTORY='"build/config"' -o build/test_homeassistant \
		tests/test_homeassistant.c src/mqtt.c src/config.c src/json.c src/buf.c src/http.c src/sources.c src/discovery.c \
		src/events.c src/notifications.c src/chain.c build/host/sqlite3.o -lm
	@./build/test_homeassistant

clean:
	rm -rf $(TITLE).elf $(TITLE)-dev.elf build

.PHONY: all require-sdk dev test clean
