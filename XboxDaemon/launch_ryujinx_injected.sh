#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INJECT_LIB="$SCRIPT_DIR/libxbox_ryujinx_inject.dylib"

RYUJINX_INPUT="${1:-/Applications/Ryujinx.app}"
UDP_ADDR="${XBOX_INJECT_UDP_ADDR:-127.0.0.1}"
UDP_PORT="${XBOX_INJECT_UDP_PORT:-7947}"
AXIS_LAYOUT="${XBOX_INJECT_AXIS_LAYOUT:-sdl}"
STICK_GAIN="${XBOX_INJECT_STICK_GAIN:-1}"
STICK_DEADZONE="${XBOX_INJECT_STICK_DEADZONE:-500}"

resolve_ryujinx_bin() {
  local in="$1"
  if [[ -d "$in" && "$in" == *.app ]]; then
    if [[ -x "$in/Contents/MacOS/Ryujinx" ]]; then
      printf "%s" "$in/Contents/MacOS/Ryujinx"
      return 0
    fi
    # Fallback: first executable in Contents/MacOS
    local cand
    for cand in "$in"/Contents/MacOS/*; do
      if [[ -x "$cand" && ! -d "$cand" ]]; then
        printf "%s" "$cand"
        return 0
      fi
    done
    return 1
  fi

  if [[ -x "$in" ]]; then
    printf "%s" "$in"
    return 0
  fi

  return 1
}

if ! RYUJINX_BIN="$(resolve_ryujinx_bin "$RYUJINX_INPUT")"; then
  echo "error: cannot resolve Ryujinx executable from: $RYUJINX_INPUT"
  echo "tip: pass either /Applications/Ryujinx.app or /Applications/Ryujinx.app/Contents/MacOS/Ryujinx"
  exit 1
fi

if [[ ! -f "$INJECT_LIB" ]]; then
  echo "error: $INJECT_LIB not found (run: make)"
  exit 1
fi

echo "[inject-launch] Ryujinx: $RYUJINX_BIN"
echo "[inject-launch] DYLD_INSERT_LIBRARIES=$INJECT_LIB"
echo "[inject-launch] UDP bind: $UDP_ADDR:$UDP_PORT"
echo "[inject-launch] Axis layout: $AXIS_LAYOUT"
echo "[inject-launch] Stick gain/deadzone: $STICK_GAIN/$STICK_DEADZONE"

export XBOX_INJECT_UDP_ADDR="$UDP_ADDR"
export XBOX_INJECT_UDP_PORT="$UDP_PORT"
export XBOX_INJECT_AXIS_LAYOUT="$AXIS_LAYOUT"
export XBOX_INJECT_STICK_GAIN="$STICK_GAIN"
export XBOX_INJECT_STICK_DEADZONE="$STICK_DEADZONE"
export DYLD_INSERT_LIBRARIES="$INJECT_LIB"

exec "$RYUJINX_BIN"
