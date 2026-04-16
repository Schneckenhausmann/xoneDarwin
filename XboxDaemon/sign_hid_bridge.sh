#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 \"Developer ID Application: Your Name (TEAMID)\""
  exit 1
fi

IDENTITY="$1"
BIN="./xbox_hid_bridge"
ENT="./hid-virtual.entitlements"

if [[ ! -f "$BIN" ]]; then
  echo "error: $BIN not found (run: make)"
  exit 1
fi

if [[ ! -f "$ENT" ]]; then
  echo "error: $ENT not found"
  exit 1
fi

codesign --force --timestamp --options runtime --sign "$IDENTITY" --entitlements "$ENT" "$BIN"
codesign --verify --deep --strict --verbose=2 "$BIN"

echo "signed: $BIN"
echo "note: entitlement must be present in your provisioning profile"
