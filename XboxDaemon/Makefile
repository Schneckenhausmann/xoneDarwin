# Makefile — Xbox One Wireless Adapter daemon
# Requires: brew install libusb

CC      = clang
TARGET  = xbox_daemon
SRCS    = xbox_daemon.c
BRIDGE_TARGET = xbox_hid_bridge
BRIDGE_SRCS   = xbox_hid_bridge.c
INJECT_TARGET = libxbox_ryujinx_inject.dylib
INJECT_SRCS   = ryujinx_sdl_inject.c
INJECT_ARCHES = -arch arm64 -arch x86_64

# libusb via Homebrew (works on both Intel and Apple Silicon)
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0 2>/dev/null || \
    echo "-I/opt/homebrew/include/libusb-1.0 -I/usr/local/include/libusb-1.0")
LIBUSB_LIBS   := $(shell pkg-config --libs   libusb-1.0 2>/dev/null || \
    echo "-L/opt/homebrew/lib -L/usr/local/lib -lusb-1.0")

CFLAGS  = -std=c17 -O2 -Wall -Wextra \
          -arch arm64 \
          $(LIBUSB_CFLAGS)

LDFLAGS = $(LIBUSB_LIBS) \
          -framework IOKit \
          -framework CoreFoundation

all: $(TARGET) $(BRIDGE_TARGET) $(INJECT_TARGET)
	@echo ""
	@echo "Build OK → ./$(TARGET)"
	@echo "Build OK → ./$(BRIDGE_TARGET)"
	@echo "Build OK → ./$(INJECT_TARGET)"
	@echo "Usage:    sudo ./$(TARGET)"

$(TARGET): $(SRCS) xbox_protocol.h mt76_firmware.h mt76_init.h
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@

$(BRIDGE_TARGET): $(BRIDGE_SRCS) xbox_protocol.h
	$(CC) $(CFLAGS) $(BRIDGE_SRCS) $(LDFLAGS) -o $@

$(INJECT_TARGET): $(INJECT_SRCS) xbox_protocol.h
	$(CC) -std=c17 -O2 -Wall -Wextra $(INJECT_ARCHES) -dynamiclib \
		-undefined dynamic_lookup \
		$(INJECT_SRCS) -o $@

clean:
	rm -f $(TARGET) $(BRIDGE_TARGET) $(INJECT_TARGET)

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

# Run as current user (libusb may need sudo on first run if udev rules not set)
run: $(TARGET)
	sudo ./$(TARGET)

# Install a launchd plist so the daemon starts at login automatically
launchd:
	@PLIST=~/Library/LaunchAgents/com.xbox.wireless.plist; \
	DAEMON_PATH="$$(pwd)/$(TARGET)"; \
	cat > "$$PLIST" <<EOF \
	<?xml version="1.0" encoding="UTF-8"?> \
	<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" \
	    "http://www.apple.com/DTDs/PropertyList-1.0.dtd"> \
	<plist version="1.0"><dict> \
	  <key>Label</key><string>com.xbox.wireless</string> \
	  <key>ProgramArguments</key><array> \
	    <string>$$DAEMON_PATH</string> \
	  </array> \
	  <key>RunAtLoad</key><true/> \
	  <key>KeepAlive</key><true/> \
	</dict></plist> \
	EOF; \
	launchctl load "$$PLIST" && echo "Daemon registered with launchd"

.PHONY: all clean install run launchd
