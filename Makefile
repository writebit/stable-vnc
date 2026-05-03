APP_NAME = StableVNC
BUNDLE = $(APP_NAME).app
BUILD_DIR = build
BINARY = $(BUILD_DIR)/$(APP_NAME)

CC = clang
OBJC = clang

CFLAGS = -Wall -Wextra -O2 -std=c11
OBJCFLAGS = -Wall -Wextra -O2 -fobjc-arc

FRAMEWORKS = -framework Cocoa -framework CoreGraphics -framework ApplicationServices -framework Carbon -framework ScreenCaptureKit -framework CoreMedia -framework CoreVideo -framework Security
LIBS = -lz

C_SRCS = src/rfb_protocol.c src/input_inject.c src/vnc_server.c src/network_info.c src/logging.c
OBJC_SRCS = src/main.m src/screen_capture.m

C_OBJS = $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(C_SRCS))
OBJC_OBJS = $(patsubst src/%.m,$(BUILD_DIR)/%.o,$(OBJC_SRCS))
ALL_OBJS = $(C_OBJS) $(OBJC_OBJS)

.PHONY: all clean app run

all: app

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/%.m | $(BUILD_DIR)
	$(OBJC) $(OBJCFLAGS) -c $< -o $@

$(BINARY): $(ALL_OBJS)
	$(OBJC) $(ALL_OBJS) $(FRAMEWORKS) $(LIBS) -o $@

app: $(BINARY)
	mkdir -p $(BUNDLE)/Contents/MacOS
	mkdir -p $(BUNDLE)/Contents/Resources
	cp $(BINARY) $(BUNDLE)/Contents/MacOS/$(APP_NAME)
	cp resources/Info.plist $(BUNDLE)/Contents/
	@echo "Built $(BUNDLE)"

run: app
	open $(BUNDLE)

clean:
	rm -rf $(BUILD_DIR) $(BUNDLE)
