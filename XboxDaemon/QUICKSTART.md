# Quickstart — xoneDarwin (XboxDaemon)

No entitlement, no SIP changes, no Xcode required for the daemon path.

For developer handoff, architecture, caveats, and open items, see:
`DEVELOPER_HANDOFF.md`

For Ryujinx-specific SDL injection (app-local), see:
`RYUJINX_INJECTION.md`

## 1. libusb installieren

```bash
brew install libusb pkg-config
```

## 2. Build

```bash
cd XboxDaemon
make
```

Firmware blobs are not included in this repository. Extract them first:

```bash
./extract_firmware.sh
```

## 3. Start

```bash
sudo ./xbox_daemon
```

Note on firmware reloads for PID `0x02FE`:

- Default: existing firmware on the dongle is reused (faster, more stable).
- Force a full reload only when needed:

```bash
XBOX_FW_FORCE_RELOAD=1 sudo ./xbox_daemon
```

Optional: emit JSON events locally over UDP (helpful when `IOHIDUserDevice` is blocked):

```bash
XBOX_EVENT_UDP=127.0.0.1:7947 sudo ./xbox_daemon
```

`sudo` is required so libusb can detach the USB driver from the dongle
(`libusb_detach_kernel_driver`). This is usually only needed on first startup or after a reboot.

## 4. Plug in dongle -> power on controller

The output should look like this:

```
Xbox One Wireless Adapter daemon (libusb + IOHIDUserDevice)

[xbox] Waiting for Xbox Wireless Adapter...
[xbox] Found dongle VID=0x045E PID=0x02FD
[xbox] Dongle initialised
[xbox] Read loop started (EP_IN=0x82, EP_OUT=0x02)
[xbox] Controller connected → slot 0
[xbox] Virtual gamepad created for slot 0
```

The controller now appears under:
- **System Settings → Bluetooth & Devices** (als HID Gamepad)
- In Spielen via `GCController` / DirectInput / SDL2

## 5. Start automatically at login (optional)

```bash
# Copy daemon to /usr/local/bin
sudo make install

# Install launchd agent
cp com.xbox.wireless.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.xbox.wireless.plist
```

## Troubleshooting

**"Could not claim interface 0"**
-> Another process is holding the dongle. Check with:
```bash
ioreg -r -c IOUSBDevice | grep -A5 "045e"
```
Then `sudo killall` that process or replug the dongle.

**Controller is not detected**
-> Hold the Xbox button on the controller for 3 seconds until the LED blinks.
   The dongle must already be initialized (daemon running).

**Virtual gamepad does not appear**
-> On some macOS versions, `IOHIDUserDevice` needs access to
   input devices. Allow it in:
   System Settings → Privacy & Security → Input Monitoring → `xbox_daemon`

If `IOHIDUserDeviceCreate` is blocked by runtime policy/entitlement,
use the UDP event stream to validate inputs:

```bash
# Terminal 1: Event-Viewer (robuster als nc)
python3 udp_event_viewer.py

# Terminal 2: daemon with UDP output
XBOX_EVENT_UDP=127.0.0.1:7947 XBOX_MULTI_PAIR=1 sudo ./xbox_daemon
```

Optional (raw data):

```bash
python3 udp_event_viewer.py --raw
```

### System-wide HID via bridge (recommended on macOS when the daemon runs under sudo)

In many setups, `IOHIDUserDevice` can fail in root context but work as a
regular user. For that, use `xbox_hid_bridge`:

```bash
# Terminal 1 (normal user): create virtual HID devices from UDP events
./xbox_hid_bridge

# Terminal 2 (sudo): USB/firmware/radio/controller handling
sudo env XBOX_EVENT_UDP=127.0.0.1:7947 XBOX_PAIR_CHANNEL=44 XBOX_WLAN_ACK=1 ./xbox_daemon
```

If the bridge works, gamepad apps (for example SDL2 tools) should see the
virtual controller as an HID device.

For 4-controller sessions, `XBOX_MULTI_PAIR=1` is recommended so the
pairing beacon stays active after the first controller (until all slots are occupied).

If the bridge exits with `HID creation probe failed`, the user context is
also blocked by runtime policy/entitlements. In that case, the only option is
code signing with HID entitlement:

```bash
./sign_hid_bridge.sh "Developer ID Application: YOUR NAME (TEAMID)"
```

Note: this entitlement must be enabled for your team.

Example events:

```json
{"type":"connected","slot":0}
{"type":"input","slot":0,"buttons":16,"lt":0,"rt":0,"lx":123,"ly":-456,"rx":0,"ry":0}
{"type":"guide","slot":0,"pressed":true}
```

## How it works (without entitlements)

```
Xbox Dongle (USB)
      │
      │  libusb bulk transfers (no kernel driver needed)
      ▼
 xbox_daemon  (userspace C process)
      │
      │  decode GIP protocol
      │
      │  IOHIDUserDeviceCreate()     <- public IOKit API
      │  IOHIDUserDeviceHandleReport()
      ▼
 macOS HID stack (sees the controller like a real USB gamepad)
      │
      ▼
 Games / Apps / Steam / joystick calibration
```
