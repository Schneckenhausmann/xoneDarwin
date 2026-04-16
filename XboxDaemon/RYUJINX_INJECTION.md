# Ryujinx SDL Injection Guide (Current Practical Path)

This guide explains how to use the UDP event stream from `xbox_daemon` and inject a
virtual SDL controller directly into Ryujinx.

This is intentionally app-specific. It does not create a system-wide gamepad.

## Current Status / Open Issues

- Injection supports both SDL2- and SDL3-based Ryujinx builds.
- Open point: analog sticks are still not mapped correctly/stably in the current build.
- Open point: LT detection/mapping is unreliable in the current build.
- Recommended workflow: capture with `xbox_mapper_tui.py`, then patch mapping based on evidence.

## 1) What this does

- `xbox_daemon` receives dongle/controller input and emits JSON UDP events.
- `libxbox_ryujinx_inject.dylib` is injected into the Ryujinx process.
- The dylib listens to UDP and creates virtual SDL joysticks via:
  - `SDL_JoystickAttachVirtual`
  - `SDL_JoystickSetVirtualAxis`
  - `SDL_JoystickSetVirtualButton`
  - `SDL_JoystickSetVirtualHat`

## 2) Build

```bash
cd XboxDaemon
make
```

Artifacts:

- `xbox_daemon`
- `libxbox_ryujinx_inject.dylib`
- `launch_ryujinx_injected.sh`

## 3) Run sequence

Terminal 1 (dongle daemon):

```bash
sudo env XBOX_EVENT_UDP=127.0.0.1:7947 XBOX_PAIR_CHANNEL=44 XBOX_WLAN_ACK=1 XBOX_MULTI_PAIR=1 ./xbox_daemon
```

Terminal 2 (launch Ryujinx with injection):

```bash
./launch_ryujinx_injected.sh
```

If Ryujinx is not in `/Applications`:

```bash
./launch_ryujinx_injected.sh "/path/to/Ryujinx"
```

## 4) Configure in Ryujinx

- Open input settings.
- Select the injected SDL virtual controller(s).
- Map controls as needed.

Slots are created on controller connect events and detached on disconnect events.

## 5) Multi-controller notes

- `XBOX_MULTI_PAIR=1` keeps pairing active for additional controllers.
- Injection side supports up to 8 slots (`MAX_SLOTS`), practical target is 4.

## 6) Caveats

1. This is process-local to Ryujinx.
   - Other apps will not see these virtual controllers.

2. Injection can be blocked by app/runtime policy.
   - `DYLD_INSERT_LIBRARIES` may be restricted for hardened apps in some setups.

3. App updates can break this path.
   - You explicitly said you will keep one known-good Ryujinx version.

4. Binding conflicts.
   - Injected dylib listens on `127.0.0.1:7947` by default.
   - Do not run another listener on that same port simultaneously.

## 7) Custom UDP address/port

You can override bind values for injection:

```bash
XBOX_INJECT_UDP_ADDR=127.0.0.1 XBOX_INJECT_UDP_PORT=7948 ./launch_ryujinx_injected.sh
```

Then run daemon with matching destination:

```bash
sudo env XBOX_EVENT_UDP=127.0.0.1:7948 ./xbox_daemon
```

## 8) Debug output

Injection logs are printed to Ryujinx stderr, e.g.:

- `[inject] Ryujinx SDL injector loaded`
- `[inject] listening on 127.0.0.1:7947`
- `[inject] slot N attached as virtual SDL joystick ...`

If you see no injector lines, the dylib did not load.

### Stick scaling

Current daemon input payload has small stick magnitudes. The injector scales stick
axes before forwarding to SDL.

Default:

- `XBOX_INJECT_STICK_GAIN=8`
- `XBOX_INJECT_STICK_DEADZONE=1200`
- `XBOX_INJECT_SWAP_LR=0`
- `XBOX_INJECT_SWAP_STICKS=0`

Override example:

```bash
XBOX_INJECT_STICK_GAIN=12 ./launch_ryujinx_injected.sh /Applications/Ryujinx.app
```

Disable L/R swap if your build/hardware does not need it:

```bash
XBOX_INJECT_SWAP_LR=0 ./launch_ryujinx_injected.sh /Applications/Ryujinx.app
```

Swap left/right sticks if needed for your current stream layout:

```bash
XBOX_INJECT_SWAP_STICKS=1 ./launch_ryujinx_injected.sh /Applications/Ryujinx.app
```

## 9) Recovery checklist

- Verify daemon is receiving `cmd=0x20` frames.
- Ensure UDP destination/port matches injector bind.
- Ensure no other process owns injector UDP port.
- Launch Ryujinx through `launch_ryujinx_injected.sh` (not Finder).
- Do not run `xbox_mapper_tui.py` on the same UDP port while testing injection.

## 10) Mapping TUI (recommended before fine-tuning)

There is a small TUI tool to inspect live slot data and run a guided mapping capture:

```bash
python3 xbox_mapper_tui.py --listen 127.0.0.1:7947 --log ./mapper_session.json
```

Controls:

- `q`: quit
- `w`: start mapping wizard
- `1..8`: select slot for wizard
- `s`: save current session log JSON

Use this first to confirm which axes/buttons move for each physical control,
then apply injector mapping tweaks with confidence.
