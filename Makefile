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

app: $(APP)

$(APP): app/fanctl.c include/smartfan_uapi.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

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

test: app
	$(PYTHON) tests/test_fanctl.py

clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)/driver" clean
	rm -rf -- "$(BUILD_DIR)"
