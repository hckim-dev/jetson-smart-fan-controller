# Native build on the target Jetson; override KDIR for a matching prepared tree.
KDIR ?= /lib/modules/$(shell uname -r)/build
CC = gcc
DT_CPP ?= cpp
DTC ?= dtc
PYTHON ?= python3
CPPFLAGS += -Iinclude
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror

BUILD_DIR := build
APP := $(BUILD_DIR)/fanctl
DT_SOURCE := dts/smartfan.dts
DT_PREPROCESSED := $(BUILD_DIR)/smartfan.preprocessed.dts
DT_OVERLAY := $(BUILD_DIR)/smartfan.dtbo
DT_HEADERS := $(KDIR)/include/dt-bindings/gpio/tegra234-gpio.h \
	$(KDIR)/include/dt-bindings/gpio/gpio.h

.PHONY: all app module dt prepare-dt test clean
all: app module dt

app: $(APP) $(BUILD_DIR)/encoder-monitor $(BUILD_DIR)/encoder-diagnose $(BUILD_DIR)/bmp180-monitor

$(APP): app/fanctl.c app/encoder.c app/encoder.h app/lcd.c app/lcd.h app/sensor.c app/sensor.h include/smartfan_uapi.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread -o $@ app/fanctl.c app/encoder.c app/lcd.c app/sensor.c $(LDLIBS)

$(BUILD_DIR)/bmp180-monitor: app/bmp180-monitor.c app/bmp180.c app/bmp180.h app/sensor.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ app/bmp180-monitor.c app/bmp180.c $(LDLIBS)

$(BUILD_DIR)/encoder-monitor: app/encoder-monitor.c app/encoder.c app/encoder.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ app/encoder-monitor.c app/encoder.c $(LDLIBS)

$(BUILD_DIR)/encoder-diagnose: app/encoder-diagnose.c app/encoder.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ app/encoder-diagnose.c $(LDLIBS)

$(BUILD_DIR)/test_encoder: tests/test_encoder.c app/encoder.c app/encoder.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iapp $(LDFLAGS) -o $@ tests/test_encoder.c app/encoder.c $(LDLIBS)

$(BUILD_DIR)/test_lcd: tests/test_lcd.c app/lcd.c app/lcd.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iapp $(LDFLAGS) -pthread -o $@ tests/test_lcd.c app/lcd.c $(LDLIBS)

$(BUILD_DIR)/test_bmp180: tests/test_bmp180.c app/bmp180.c app/bmp180.h app/sensor.c app/sensor.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iapp $(LDFLAGS) -o $@ tests/test_bmp180.c app/bmp180.c app/sensor.c $(LDLIBS)

$(BUILD_DIR)/test_sensor: tests/test_sensor.c app/sensor.c app/sensor.h $(BUILD_DIR)/bmp180-monitor | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iapp $(LDFLAGS) -o $@ tests/test_sensor.c app/sensor.c $(LDLIBS)

$(BUILD_DIR)/test_auto_guard: tests/test_auto_guard.c app/fanctl.c app/encoder.c app/encoder.h app/lcd.c app/lcd.h app/sensor.c app/sensor.h include/smartfan_uapi.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iapp $(LDFLAGS) -pthread -o $@ tests/test_auto_guard.c app/encoder.c app/lcd.c app/sensor.c $(LDLIBS)

$(BUILD_DIR)/test_lcd_shutdown: tests/test_lcd_shutdown.c app/lcd.c app/lcd.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iapp $(LDFLAGS) -pthread -o $@ tests/test_lcd_shutdown.c $(LDLIBS)

module:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)/driver" modules

dt: $(DT_OVERLAY)

prepare-dt: dt
	$(PYTHON) scripts/prepare-dt.py

$(DT_PREPROCESSED): $(DT_SOURCE) $(DT_HEADERS) Makefile | $(BUILD_DIR)
	$(DT_CPP) -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
		-I "$(KDIR)/include" $< -o $@

$(DT_OVERLAY): $(DT_PREPROCESSED)
	$(DTC) -@ -I dts -O dtb -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

test: app $(BUILD_DIR)/test_encoder $(BUILD_DIR)/test_lcd $(BUILD_DIR)/test_bmp180 $(BUILD_DIR)/test_sensor $(BUILD_DIR)/test_auto_guard $(BUILD_DIR)/test_lcd_shutdown
	./$(BUILD_DIR)/test_encoder
	./$(BUILD_DIR)/test_lcd
	./$(BUILD_DIR)/test_bmp180
	./$(BUILD_DIR)/test_sensor
	./$(BUILD_DIR)/test_auto_guard
	./$(BUILD_DIR)/test_lcd_shutdown
	$(PYTHON) tests/test_fanctl.py

clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)/driver" clean
	rm -rf -- "$(BUILD_DIR)"
