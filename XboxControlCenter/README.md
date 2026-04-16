# xoneDarwin Control Center (macOS)

Small SwiftUI desktop app to operate the existing `XboxDaemon` workflow:

1. Pick an app bundle (`.app`) via file picker.
2. Launch the selected app through SDL injection.
3. Start `xbox_daemon` with admin privileges.

It also includes an expandable debug console and a live connected-controller counter derived from daemon/inject logs.

## Run

```bash
cd XboxControlCenter
swift run
```

## Notes

- The app expects this repository layout and resolves `XboxDaemon/` automatically.
- `xbox_daemon` must already be built (`make` in `XboxDaemon/`).
- Daemon launch uses:
  `env XBOX_EVENT_UDP=127.0.0.1:7947 XBOX_PAIR_CHANNEL=44 XBOX_WLAN_ACK=1 XBOX_MULTI_PAIR=1 ./xbox_daemon`
- macOS may cache admin authorization briefly, so repeated starts usually do not re-prompt immediately.
