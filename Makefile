# Jetson native build. KDIR 변경 시 실행 커널과 일치하는 준비된 tree를 사용한다.
KDIR ?= /lib/modules/$(shell uname -r)/build
ifeq ($(origin CC), default)
CC = gcc
endif
CC ?= gcc
DT_CPP ?= cpp
DTC ?= dtc
PYTHON ?= python3
CPPFLAGS += -Iinclude
CFLAGS ?= -O2
override CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror

BUILD_DIR := build
APP := $(BUILD_DIR)/fanctl
CONTROL_SOURCES := app/controller.c app/fan-device.c app/event-log.c
CONTROL_HEADERS := app/controller.h app/fan-device.h app/event-log.h
APP_SOURCES := app/main.c $(CONTROL_SOURCES) app/encoder.c app/lcd.c app/sensor.c
DT_SOURCE := dts/smartfan.dts
DT_PREPROCESSED := $(BUILD_DIR)/smartfan.preprocessed.dts
DT_OVERLAY := $(BUILD_DIR)/smartfan.dtbo
APP_BINARIES := $(APP) $(BUILD_DIR)/encoder-monitor $(BUILD_DIR)/encoder-diagnose $(BUILD_DIR)/bmp180-monitor
TEST_BINARIES := $(addprefix $(BUILD_DIR)/,test_encoder test_encoder_filter test_encoder_config test_lcd test_bmp180 test_sensor test_auto_guard test_lcd_shutdown test_process test_event_log test_lcd_concurrency)
PROCESS_FIXTURE := $(BUILD_DIR)/test-bmp180-process
TEST_CFLAGS = $(CFLAGS) -UNDEBUG
SAN_CFLAGS = $(TEST_CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
ANALYSIS_OBJECTS := $(patsubst app/%.c,$(BUILD_DIR)/analysis/%.o,$(wildcard app/*.c))
SAN_TESTS := $(addprefix $(BUILD_DIR)/sanitizers/,test_encoder test_encoder_filter test_bmp180 test_sensor test_auto_guard test_lcd_shutdown test_process test_event_log test_lcd_concurrency)
THREAD_TESTS := $(BUILD_DIR)/threads/test_event_log $(BUILD_DIR)/threads/test_lcd_concurrency
DT_HEADERS := $(KDIR)/include/dt-bindings/gpio/tegra234-gpio.h \
	$(KDIR)/include/dt-bindings/gpio/gpio.h

.PHONY: all app module dt prepare-dt test analyze sanitize thread-check clean
all: app module dt

app: $(APP_BINARIES)

$(APP): $(APP_SOURCES) $(CONTROL_HEADERS) app/encoder.h app/lcd.h app/sensor.h include/smartfan_uapi.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread -o $@ $(APP_SOURCES) $(LDLIBS)

$(BUILD_DIR)/bmp180-monitor: app/bmp180-monitor.c app/bmp180.c app/bmp180.h app/sensor.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ app/bmp180-monitor.c app/bmp180.c $(LDLIBS)

$(BUILD_DIR)/encoder-monitor: app/encoder-monitor.c app/encoder.c app/encoder.h app/encoder-filter.c app/encoder-filter.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ app/encoder-monitor.c app/encoder.c app/encoder-filter.c $(LDLIBS)

$(BUILD_DIR)/encoder-diagnose: app/encoder-diagnose.c app/encoder.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ app/encoder-diagnose.c $(LDLIBS)

$(BUILD_DIR)/test_encoder: tests/test_encoder.c app/encoder.c app/encoder.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -Iapp $(LDFLAGS) -o $@ tests/test_encoder.c app/encoder.c $(LDLIBS)

$(BUILD_DIR)/test_encoder_filter: tests/test_encoder_filter.c app/encoder-filter.c app/encoder-filter.h app/encoder.c app/encoder.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -Iapp $(LDFLAGS) -o $@ tests/test_encoder_filter.c app/encoder-filter.c app/encoder.c $(LDLIBS)

$(BUILD_DIR)/test_encoder_config: tests/test_encoder_config.c app/encoder.c app/encoder.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -Iapp $(LDFLAGS) -Wl,--wrap=open -Wl,--wrap=ioctl -o $@ tests/test_encoder_config.c app/encoder.c $(LDLIBS)

$(BUILD_DIR)/test_lcd: tests/test_lcd.c app/lcd.c app/lcd.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -Iapp $(LDFLAGS) -pthread -o $@ tests/test_lcd.c app/lcd.c $(LDLIBS)

$(BUILD_DIR)/test_bmp180: tests/test_bmp180.c app/bmp180.c app/bmp180.h app/sensor.c app/sensor.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -Iapp $(LDFLAGS) -o $@ tests/test_bmp180.c app/bmp180.c app/sensor.c $(LDLIBS)

$(BUILD_DIR)/test_sensor: tests/test_sensor.c app/sensor.c app/sensor.h $(BUILD_DIR)/bmp180-monitor | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -Iapp $(LDFLAGS) -Wl,--wrap=execv -Wl,--wrap=fork -Wl,--wrap=waitpid -o $@ tests/test_sensor.c app/sensor.c $(LDLIBS)

$(BUILD_DIR)/test_auto_guard: tests/test_auto_guard.c $(CONTROL_SOURCES) $(CONTROL_HEADERS) app/encoder.c app/encoder.h app/lcd.c app/lcd.h app/sensor.c app/sensor.h include/smartfan_uapi.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -Iapp $(LDFLAGS) -pthread -Wl,--wrap=ioctl -o $@ tests/test_auto_guard.c $(CONTROL_SOURCES) app/encoder.c app/lcd.c app/sensor.c $(LDLIBS)

$(BUILD_DIR)/test_lcd_shutdown: tests/test_lcd_shutdown.c app/lcd.c app/lcd.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -Iapp $(LDFLAGS) -pthread -o $@ tests/test_lcd_shutdown.c $(LDLIBS)

$(PROCESS_FIXTURE): tests/bmp180_process_fixture.c app/bmp180-monitor.c app/bmp180.h app/sensor.h Makefile | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -Iapp $(LDFLAGS) -Wl,--wrap=nanosleep -o $@ app/bmp180-monitor.c tests/bmp180_process_fixture.c $(LDLIBS)

$(BUILD_DIR)/test_process: tests/test_process.c $(PROCESS_FIXTURE) $(CONTROL_SOURCES) $(CONTROL_HEADERS) app/encoder.c app/encoder.h app/lcd.c app/lcd.h app/sensor.c app/sensor.h Makefile | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -Iapp $(LDFLAGS) -pthread -o $@ tests/test_process.c $(CONTROL_SOURCES) app/encoder.c app/lcd.c app/sensor.c $(LDLIBS)

$(BUILD_DIR)/test_event_log: tests/test_event_log.c app/event-log.c app/event-log.h Makefile | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -Iapp $(LDFLAGS) -pthread -Wl,--wrap=write -Wl,--wrap=pthread_create -o $@ tests/test_event_log.c app/event-log.c $(LDLIBS)

$(BUILD_DIR)/test_lcd_concurrency: tests/test_lcd_concurrency.c app/lcd.c app/lcd.h Makefile | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -Iapp $(LDFLAGS) -pthread -o $@ tests/test_lcd_concurrency.c $(LDLIBS)

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

$(APP_BINARIES) $(TEST_BINARIES): Makefile

$(BUILD_DIR)/analysis $(BUILD_DIR)/sanitizers $(BUILD_DIR)/threads:
	mkdir -p $@

$(BUILD_DIR)/analysis/%.o: app/%.c $(wildcard app/*.h) include/smartfan_uapi.h Makefile | $(BUILD_DIR)/analysis
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iapp -pthread -fanalyzer -c $< -o $@

analyze: $(ANALYSIS_OBJECTS)

$(BUILD_DIR)/sanitizers/test_encoder: tests/test_encoder.c app/encoder.c app/encoder.h Makefile | $(BUILD_DIR)/sanitizers
	$(CC) $(CPPFLAGS) $(SAN_CFLAGS) -Iapp $(LDFLAGS) -o $@ tests/test_encoder.c app/encoder.c $(LDLIBS)

$(BUILD_DIR)/sanitizers/test_encoder_filter: tests/test_encoder_filter.c app/encoder-filter.c app/encoder-filter.h app/encoder.c app/encoder.h Makefile | $(BUILD_DIR)/sanitizers
	$(CC) $(CPPFLAGS) $(SAN_CFLAGS) -Iapp $(LDFLAGS) -o $@ tests/test_encoder_filter.c app/encoder-filter.c app/encoder.c $(LDLIBS)

$(BUILD_DIR)/sanitizers/test_bmp180: tests/test_bmp180.c app/bmp180.c app/bmp180.h app/sensor.c app/sensor.h Makefile | $(BUILD_DIR)/sanitizers
	$(CC) $(CPPFLAGS) $(SAN_CFLAGS) -Iapp $(LDFLAGS) -o $@ tests/test_bmp180.c app/bmp180.c app/sensor.c $(LDLIBS)

$(BUILD_DIR)/sanitizers/test_sensor: tests/test_sensor.c app/sensor.c app/sensor.h $(BUILD_DIR)/bmp180-monitor Makefile | $(BUILD_DIR)/sanitizers
	$(CC) $(CPPFLAGS) $(SAN_CFLAGS) -Iapp $(LDFLAGS) -Wl,--wrap=execv -Wl,--wrap=fork -Wl,--wrap=waitpid -o $@ tests/test_sensor.c app/sensor.c $(LDLIBS)

$(BUILD_DIR)/sanitizers/test_lcd_shutdown: tests/test_lcd_shutdown.c app/lcd.c app/lcd.h Makefile | $(BUILD_DIR)/sanitizers
	$(CC) $(CPPFLAGS) $(SAN_CFLAGS) -Iapp $(LDFLAGS) -pthread -o $@ tests/test_lcd_shutdown.c $(LDLIBS)

$(BUILD_DIR)/sanitizers/test_auto_guard: tests/test_auto_guard.c $(CONTROL_SOURCES) $(CONTROL_HEADERS) app/encoder.c app/encoder.h app/lcd.c app/lcd.h app/sensor.c app/sensor.h Makefile | $(BUILD_DIR)/sanitizers
	$(CC) $(CPPFLAGS) $(SAN_CFLAGS) -Iapp $(LDFLAGS) -pthread -Wl,--wrap=ioctl -o $@ tests/test_auto_guard.c $(CONTROL_SOURCES) app/encoder.c app/lcd.c app/sensor.c $(LDLIBS)

$(BUILD_DIR)/sanitizers/test_process: tests/test_process.c $(PROCESS_FIXTURE) $(CONTROL_SOURCES) $(CONTROL_HEADERS) app/encoder.c app/encoder.h app/lcd.c app/lcd.h app/sensor.c app/sensor.h Makefile | $(BUILD_DIR)/sanitizers
	$(CC) $(CPPFLAGS) $(SAN_CFLAGS) -Iapp $(LDFLAGS) -pthread -o $@ tests/test_process.c $(CONTROL_SOURCES) app/encoder.c app/lcd.c app/sensor.c $(LDLIBS)

$(BUILD_DIR)/sanitizers/test_event_log: tests/test_event_log.c app/event-log.c app/event-log.h Makefile | $(BUILD_DIR)/sanitizers
	$(CC) $(CPPFLAGS) $(SAN_CFLAGS) -Iapp $(LDFLAGS) -pthread -Wl,--wrap=write -Wl,--wrap=pthread_create -o $@ tests/test_event_log.c app/event-log.c $(LDLIBS)

$(BUILD_DIR)/sanitizers/test_lcd_concurrency: tests/test_lcd_concurrency.c app/lcd.c app/lcd.h Makefile | $(BUILD_DIR)/sanitizers
	$(CC) $(CPPFLAGS) $(SAN_CFLAGS) -Iapp $(LDFLAGS) -pthread -o $@ tests/test_lcd_concurrency.c $(LDLIBS)

sanitize: $(SAN_TESTS)
	./$(BUILD_DIR)/sanitizers/test_encoder
	./$(BUILD_DIR)/sanitizers/test_encoder_filter
	./$(BUILD_DIR)/sanitizers/test_bmp180
	./$(BUILD_DIR)/sanitizers/test_sensor
	./$(BUILD_DIR)/sanitizers/test_auto_guard
	./$(BUILD_DIR)/sanitizers/test_lcd_shutdown
	./$(BUILD_DIR)/sanitizers/test_process
	./$(BUILD_DIR)/sanitizers/test_event_log
	./$(BUILD_DIR)/sanitizers/test_lcd_concurrency

$(BUILD_DIR)/threads/test_event_log: tests/test_event_log.c app/event-log.c app/event-log.h Makefile | $(BUILD_DIR)/threads
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -O1 -g -fsanitize=thread -Iapp $(LDFLAGS) -pthread -Wl,--wrap=write -Wl,--wrap=pthread_create -o $@ tests/test_event_log.c app/event-log.c $(LDLIBS)

$(BUILD_DIR)/threads/test_lcd_concurrency: tests/test_lcd_concurrency.c app/lcd.c app/lcd.h Makefile | $(BUILD_DIR)/threads
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -O1 -g -fsanitize=thread -Iapp $(LDFLAGS) -pthread -o $@ tests/test_lcd_concurrency.c $(LDLIBS)

thread-check: $(THREAD_TESTS)
	./$(BUILD_DIR)/threads/test_event_log
	./$(BUILD_DIR)/threads/test_lcd_concurrency

test: app $(TEST_BINARIES)
	./$(BUILD_DIR)/test_encoder
	./$(BUILD_DIR)/test_encoder_filter
	./$(BUILD_DIR)/test_encoder_config
	./$(BUILD_DIR)/test_lcd
	./$(BUILD_DIR)/test_bmp180
	./$(BUILD_DIR)/test_sensor
	./$(BUILD_DIR)/test_auto_guard
	./$(BUILD_DIR)/test_lcd_shutdown
	./$(BUILD_DIR)/test_process
	./$(BUILD_DIR)/test_event_log
	./$(BUILD_DIR)/test_lcd_concurrency
	$(PYTHON) tests/test_fanctl.py
	$(PYTHON) tests/test_build.py
	$(PYTHON) tests/test_prepare_dt.py

clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)/driver" clean
	$(RM) $(APP_BINARIES) $(TEST_BINARIES) $(PROCESS_FIXTURE) $(DT_PREPROCESSED) $(DT_OVERLAY) $(ANALYSIS_OBJECTS) $(SAN_TESTS) $(THREAD_TESTS)
