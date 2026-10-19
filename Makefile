# ps5-homeassistant: build with the ps5-payload-dev SDK (PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk).
#   make           ps5-homeassistant.elf      the local API on 127.0.0.1 only
#   make dev       ps5-homeassistant-dev.elf  the API also on the network, for testing from a PC; never release it
#   make test      host tests with AddressSanitizer and UBSan (gcc, no SDK)
PS5_HOST ?= 192.168.68.125

TITLE   := ps5-homeassistant
VERSION := 0.2.0

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

CFLAGS := -Wall -Wextra -O2 -std=c11 -D_DEFAULT_SOURCE -DHA_VERSION='"$(VERSION)"'
LDLIBS := -lkernel_sys -lSceSystemService -lpthread
SRCS   := src/main.c src/mqtt.c src/config.c src/json.c src/buf.c src/http.c src/sources.c src/discovery.c src/power.c
HDRS   := $(wildcard src/*.h)

HOST_CC ?= gcc
HOST_FLAGS := -Wall -Wextra -O1 -std=c11 -D_DEFAULT_SOURCE -fsanitize=address,undefined -fno-sanitize-recover=all

all: $(TITLE).elf

require-sdk:
	@test -n "$(PS5_PAYLOAD_SDK)" || { echo "PS5_PAYLOAD_SDK is undefined; export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk"; exit 1; }

$(TITLE).elf: require-sdk $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

dev: $(TITLE)-dev.elf

$(TITLE)-dev.elf: require-sdk $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -DHA_DEV -o $@ $(SRCS) $(LDLIBS)

test:
	@mkdir -p build/config
	@$(HOST_CC) $(HOST_FLAGS) -DCONFIG_DIRECTORY='"build/config"' -o build/test_homeassistant \
		tests/test_homeassistant.c src/mqtt.c src/config.c src/json.c src/buf.c src/http.c src/sources.c src/discovery.c
	@./build/test_homeassistant

clean:
	rm -rf $(TITLE).elf $(TITLE)-dev.elf build

.PHONY: all require-sdk dev test clean
