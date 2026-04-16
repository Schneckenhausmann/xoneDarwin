# XboxDaemon Developer Handoff

This document is a technical handoff for the current macOS port of the Xbox wireless dongle stack.
It focuses on what works, what does not, why, and what to do next.

## 1) Executive Status

- USB + firmware + MT76 radio initialization: **working** for PID `0x02FE`.
- Controller pairing + association + data path: **working**.
- GIP post-association handshake: **working enough to get sustained input stream**.
- Live controller input (`cmd=0x20`) from dongle: **working**.
- UDP event stream for app integration/debugging: **working**.
- System-wide virtual gamepad on macOS via `IOHIDUserDevice`: **blocked by runtime/entitlement policy on this machine**.
- Ryujinx-specific SDL injection path: **working** with SDL3-capable builds when using `axis_layout=sdl` defaults.

Bottom line: transport/protocol foundation is solid; system HID exposure is the current blocker.

## 2) Repository Components

- `xbox_daemon.c`: main daemon (USB, firmware/radio boot, MT76 frame handling, GIP decode, optional HID + UDP events).
- `mt76_init.h`: low-level MT76 helpers (register/control ops, DMA firmware upload, radio init, channel/beacon functions).
- `xbox_protocol.h`: GIP constants, HID descriptor/report mapping, input conversion helpers.
- `udp_event_viewer.py`: UDP event listener/decoder for validation.
- `xbox_hid_bridge.c`: user-space bridge from daemon UDP events to `IOHIDUserDevice` (for split privilege model).
- `hid-virtual.entitlements`: entitlement template for HID virtual device capability.
- `sign_hid_bridge.sh`: helper for codesigning bridge with entitlements.
- `QUICKSTART.md`: operator-focused usage notes.

## 3) Architecture (Current)

### 3.1 Runtime split

1. `xbox_daemon` (typically `sudo`) handles:
   - libusb device access
   - firmware/radio init
   - 802.11 management/data handling
   - GIP parsing + slot state
   - optional UDP event output

2. `xbox_hid_bridge` (normal user) handles:
   - UDP receive from daemon
   - creation of virtual HID gamepads
   - forwarding input/guide/disconnect into HID reports

Reason: on many macOS setups, HID virtual device creation may fail in root context but can work in user context.

### 3.2 Controller pipeline

Dongle USB bulk IN -> MT76 Wi-Fi frame decode -> GIP decode -> slot state ->
- UDP event JSON (always available when enabled)
- HID report injection (if HID backend is allowed)

## 4) Protocol Findings / Important Behavior

### 4.1 Critical fix: outbound controller packets must use `CMD_PACKET_TX`

The biggest stability breakthrough was switching GIP outbound traffic to xow-like `CMD_PACKET_TX` payload framing
instead of naive raw WLAN TX for controller-directed packets.

That change enabled proper progression beyond repeated hello/assoc loops and yielded sustained `cmd=0x20` input frames.

### 4.2 GIP frame interpretation

Header byte 1 is treated as:
- low nibble: device id
- high nibble: frame type flags (`COMMAND`, `ACK`, `REQUEST`)

This aligns behavior with xow expectations and improved interoperability.

### 4.3 Handshake sequence currently used (host->controller)

On announce (`cmd=0x02`, request, device 0), host sends once:
- PowerOn (`cmd=0x05`, request)
- LED dim (`cmd=0x0A`, request)
- Serial request (`cmd=0x1E`, request|ack, payload `0x04`)

Observed controller responses now include:
- Serial (`0x1E`, ack/request typed)
- Status (`0x03`)
- Repeated short `0x1F` packets (acked)
- Continuous Input (`0x20`) frames

### 4.4 Input payload layout

Parser supports xow/xone-style 16-byte input base:
- `[buttons:4][lt:2][rt:2][lx:2][ly:2][rx:2][ry:2]`
and keeps a fallback path for older compact layout.

This was required; previous assumptions dropped valid input.

## 5) Environment Variables

Defined/used in `xbox_daemon.c`:

- `XBOX_EVENT_UDP` (e.g. `127.0.0.1:7947`): enable JSON UDP events.
- `XBOX_WLAN_ACK` (`1/0`): WLAN-level GIP ack behavior.
- `XBOX_USB_GIP_ACK` (`1/0`): legacy USB ack path.
- `XBOX_CLIENT_LOST_EVENTS` (`1/0`): process MT76 client-lost events.
- `XBOX_MULTI_PAIR` (`1/0`): keep pairing beacon active for multi-controller pairing.
- `XBOX_PAIR_CHANNEL` (`36|40|44|48`): force pairing channel.
- `XBOX_USB_RESET_ON_START` (`1/0`): USB reset at startup.
- `XBOX_VERBOSE_AIR` (`1/0`): verbose WLAN logging.
- `XBOX_FW_FORCE_RELOAD` (`1/0`): force firmware reset/reload even if already resident.

## 6) Multi-Controller Foundation (4-player readiness)

The code supports up to `MAX_SLOTS=8`.

Multi-pair hardening added:
- `XBOX_MULTI_PAIR=1` keeps pairing beacon open while active slots < max.
- beacon no longer always disabled after first data path in multi-pair mode.
- beacon can be re-enabled from dongle button event and client-lost flow.

For 4-controller sessions, use:

```bash
sudo env XBOX_EVENT_UDP=127.0.0.1:7947 XBOX_PAIR_CHANNEL=44 XBOX_WLAN_ACK=1 XBOX_MULTI_PAIR=1 ./xbox_daemon
```

## 7) Firmware Reload Caveat (replug issue)

Issue observed: when firmware was already loaded, forced reset path could time out on macOS.

Current behavior:
- If firmware appears resident (`FCE != 0`), daemon **reuses** resident firmware by default.
- Full reset/reload only when `XBOX_FW_FORCE_RELOAD=1` is set.

This avoids frequent replug requirements after normal restarts.

## 8) HID/System-Wide Controller Caveat (Major Open Problem)

On this machine, both daemon probe and bridge probe can fail with:
- `HID creation probe failed ...`
- `virtual HID device creation unavailable in this runtime`

Meaning:
- protocol stack is not the blocker anymore,
- macOS runtime policy/entitlement is.

`IOHIDUserDevice` system-wide exposure likely requires entitlement-backed signed binary (`com.apple.developer.hid.virtual.device`).

### 8.1 Current workaround status

- UDP path works and is useful for debugging/integration.
- `udp_event_viewer.py` confirms controller events.
- `xbox_hid_bridge` exists, but still depends on OS allowing HID virtual device creation.

## 9) Logging Interpretation Cheat-Sheet

- `DATA cmd=0x20 ...`: controller input reports flowing (good).
- `DATA cmd=0x03 ...`: status/heartbeat class traffic.
- `DATA cmd=0x1f ...`: currently treated as unknown+acked heartbeat-like traffic.
- repeated `Reassoc` storm: usually handshake/session mismatch (greatly reduced versus earlier state).
- `hid_unavailable`: transport works, but system HID output is blocked.

## 10) Known Open Points

1. System-wide HID exposure on macOS without entitlement friction.
2. Analog and trigger behavior may still require per-game/per-emulator sensitivity tuning.
3. Validate future Ryujinx/SDL updates for API and mapping regressions.
4. Formal handling/documentation of `cmd=0x1f` semantics (currently acked + ignored payload).
5. Cleaner log levels / production mode (reduce verbose frame spam).
6. Better persistence/recovery for stale peers in long-running sessions.
7. More extensive multi-controller soak test matrix (4 controllers, reconnect churn).

## 14) Recent Work Log (Latest Session)

- Added UDP diagnostics tooling:
  - `udp_event_viewer.py` (human-readable stream viewer)
  - `xbox_mapper_tui.py` (interactive mapper wizard + JSON session logging)
- Added app-specific Ryujinx path:
  - `ryujinx_sdl_inject.c` (SDL virtual joystick injection via `DYLD_INSERT_LIBRARIES`)
  - `launch_ryujinx_injected.sh` (launcher helper)
  - `RYUJINX_INJECTION.md` (usage + caveats)
- Added HID bridge path:
  - `xbox_hid_bridge.c` for UDP->IOHIDUserDevice split-process model
  - entitlement/sign helper files (`hid-virtual.entitlements`, `sign_hid_bridge.sh`)
- Firmware restart stability improvement:
  - default behavior now reuses resident firmware if already loaded
  - forced reload gated behind `XBOX_FW_FORCE_RELOAD=1`
- Multi-controller foundation hardening:
  - `XBOX_MULTI_PAIR=1` keeps pairing beacon open until slots are occupied
- Current caveat on Ryujinx path:
  - controller detection can be sensitive to UDP port collisions (e.g. mapper/listener using same port)

### 14.1 Evidence from `mapper_session.json`

- Wizard results captured in latest validated run:
  - Buttons, D-pad, LT/RT, and all stick directions detected.
  - Trigger range normalized (`0..1023`) and stick range observed (`-32768..32767`).

## 11) Suggested Next Steps (Priority)

1. **Entitlement path validation**
   - Determine if target Apple account can obtain/use `com.apple.developer.hid.virtual.device`.
   - Sign `xbox_hid_bridge` with entitlement and verify via `codesign -d --entitlements :-`.
   - Re-test bridge probe and app visibility (SDL tools, game runtime).

2. **If entitlement blocked**
   - Decide strategic alternative:
     - DriverKit/system extension route, or
     - app-specific integrations (non-system-wide), or
     - platform switch for gaming runtime.

3. **Protocol cleanup**
   - Keep current stable handshake.
   - Add explicit parser for currently unknown but recurring command classes (`0x1f`) if needed.

## 12) Minimal Reproduction / Smoke Tests

### 12.1 Transport/protocol

```bash
python3 udp_event_viewer.py
sudo env XBOX_EVENT_UDP=127.0.0.1:7947 XBOX_PAIR_CHANNEL=44 XBOX_WLAN_ACK=1 XBOX_MULTI_PAIR=1 ./xbox_daemon
```

Expected: connected event + continuous input events while moving sticks/buttons.

### 12.2 Bridge probe

```bash
./xbox_hid_bridge
```

Expected success: bridge stays running and creates slot devices on connect events.
Current known result on this machine: probe fails due to runtime policy.

## 13) Practical Notes for Next Developer

- Do not regress `CMD_PACKET_TX` path for controller-directed frames.
- Keep `XBOX_FW_FORCE_RELOAD` default-off behavior; force reload should be opt-in.
- Use `XBOX_MULTI_PAIR=1` for any serious multiplayer validation.
- Treat HID failure and transport failure as separate problem classes.
- Validate with UDP first before touching HID code.

---

If you need a compact runbook, start with `QUICKSTART.md`; use this file for engineering context and decision history.
