# Makefile for hb2-btproxy (Home Base 2 ESPHome Bluetooth Proxy)
#
# Cross-compilation for Cortex-A9 soft-float uClibc target:
#   make CC=/home/mathis/hb2-buildroot/buildroot-2025.02.18/output/host/bin/arm-buildroot-linux-uclibcgnueabi-gcc

CC ?= arm-buildroot-linux-uclibcgnueabi-gcc
STRIP ?= $(subst -gcc,-strip,$(CC))

CFLAGS ?= -Wall -Wextra -O2
CFLAGS += -std=gnu11 -Isrc -Isrc/include -pthread
LDFLAGS += -pthread

TARGET = hb2-btproxy

SOURCES = \
    src/main.c \
    src/esphome_api.c \
    src/esphome_proto.c \
    src/esphome_plugin.c \
    src/switch_proto.c \
    src/bluetooth_proxy_plugin.c \
    src/ble_scanner_hci.c \
    src/ble_gatt_client.c

OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@echo "[LD] $@"
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LDFLAGS)
	@if command -v $(STRIP) >/dev/null 2>&1; then \
	    echo "[STRIP] $@"; \
	    $(STRIP) $@; \
	fi

%.o: %.c
	@echo "[CC] $<"
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean
