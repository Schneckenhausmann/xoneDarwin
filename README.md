# xoneDarwin

`xoneDarwin` is a macOS userspace implementation for the Xbox Wireless Adapter
focused on stable multi-controller sessions (3-4 controllers in parallel), with
Ryujinx injection support and a small macOS control app.

If you only need one controller and low setup friction, Bluetooth is usually the
better choice on macOS.

## Why this project exists

- Reliable Xbox Wireless Adapter path on macOS for multi-pad local play.
- Practical workflow for emulator use, especially Ryujinx SDL injection.
- Debuggability via UDP event stream and tools.

## Project layout

- `XboxDaemon/` - core daemon, adapter protocol handling, injection tooling.
- `XboxControlCenter/` - SwiftUI macOS app to run the workflow.
- `XboxOneWirelessDriver/` - earlier DriverKit work-in-progress artifacts.

## Prerequisites

- macOS (Apple Silicon or Intel)
- Xbox Wireless Adapter + controller(s)
- Homebrew

Install required tools:

```bash
brew install libusb pkg-config cabextract
```

## Quick start (terminal)

In one shell:

```bash
cd XboxDaemon
make

# firmware is not bundled in this repository; fetch it first
./extract_firmware.sh
```

Start the daemon (Terminal 1):

```bash
cd XboxDaemon
sudo env XBOX_EVENT_UDP=127.0.0.1:7947 XBOX_PAIR_CHANNEL=44 XBOX_WLAN_ACK=1 XBOX_MULTI_PAIR=1 ./xbox_daemon
```

Start Ryujinx with injection (Terminal 2):

```bash
cd XboxDaemon
XBOX_INJECT_UDP_PORT=7947 XBOX_INJECT_AXIS_LAYOUT=sdl XBOX_INJECT_STICK_GAIN=1 XBOX_INJECT_STICK_DEADZONE=500 ./launch_ryujinx_injected.sh /Applications/Ryujinx.app
```

## Quick start (GUI)

```bash
cd XboxControlCenter
swift run
```

Inside the app: select emulator app -> unlock admin -> launch with SDL injection -> start daemon.

## Troubleshooting

- **No controller input in Ryujinx**
  - Confirm both commands use port `7947`.
  - Ensure no other tool is bound to `7947` (mapper/viewer can conflict).
- **Daemon fails to start**
  - Run from `XboxDaemon/` and use `sudo`.
  - Rebuild with `make`.
- **Firmware not found**
  - Re-run `./extract_firmware.sh` from `XboxDaemon/`.
- **Still broken after retries**
  - Kill stale processes and restart:

```bash
pkill -f xbox_daemon || true
pkill -f "/Applications/Ryujinx.app/Contents/MacOS/Ryujinx" || true
```

## Caveats

- This is a best-effort reverse-engineered userspace stack, not an official driver.
- macOS runtime/security behavior can differ between machines and versions.
- Virtual HID exposure can be entitlement-sensitive depending on environment.
- Firmware and adapter behavior may vary across dongle/controller revisions.
- Microsoft firmware blobs are **not distributed** in this repository; obtain them via scripts.

## Credits

This project stands on the excellent reverse-engineering work by:

- [`xone`](https://github.com/dlundqvist/xone)
- [`xow`](https://github.com/medusalix/xow)

We are grateful for the groundwork those projects provided.

`FIRMWARE/xone-install.sh` is included from xone (Linux-oriented reference installer).

## License

GPL-2.0-or-later. See `LICENSE`.
