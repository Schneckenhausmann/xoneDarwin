#!/usr/bin/env bash
# extract_firmware.sh
# Downloads the official Xbox Wireless Adapter Windows driver from Microsoft
# and extracts the MT76 firmware (mt7612us.bin) required by the 0x02FE dongle.
#
# Requires: curl, cabextract (brew install cabextract)
# Output: ./mt7612us.bin (~140 KB)

set -euo pipefail

FW_OUT="mt7612us.bin"

# Official Microsoft driver download URL (Xbox Wireless Adapter for Windows)
# Source: Microsoft Update Catalog
DRIVER_URL="https://download.microsoft.com/download/1/0/1/101a19ef-8a03-4230-8eb4-f1f37d54aafd/xb_acc_pcusba_2019_1210.cab"
CAB_FILE="xbox_driver.cab"

check_dep() {
    if ! command -v "$1" &>/dev/null; then
        echo "Error: '$1' not found."
        echo "Installation: brew install $2"
        exit 1
    fi
}

check_dep curl   curl
check_dep cabextract cabextract

echo "=== Xbox Wireless Adapter Firmware Extractor ==="
echo ""

# -- Step 1: Download driver CAB ---------------------------------------------
if [[ -f "$CAB_FILE" ]]; then
    echo "[1/3] CAB file already exists, skipping download."
else
    echo "[1/3] Downloading Microsoft Xbox Wireless Adapter driver..."
    curl -L --progress-bar -o "$CAB_FILE" "$DRIVER_URL"
    echo "      OK ($(du -h "$CAB_FILE" | cut -f1))"
fi

# -- Step 2: Extract CAB ------------------------------------------------------
echo "[2/3] Extracting CAB file..."
EXTRACT_DIR="xbox_driver_extracted"
mkdir -p "$EXTRACT_DIR"
cabextract -d "$EXTRACT_DIR" "$CAB_FILE" 2>/dev/null || true

# -- Step 3: Find firmware blob ----------------------------------------------
echo "[3/3] Searching for firmware blob (MT7612U)..."

# The firmware is embedded in a .sys or .bin file.
# Signature: MT76 firmware starts with magic "WAIO" or specific bytes.
# We search for the known MT7612U firmware magic: 0x23 0x00 0x00 0x00

FW_MAGIC_HEX="23000000"  # MT76 firmware header magic (little-endian 0x00000023)
FW_FOUND=0

for f in "$EXTRACT_DIR"/*; do
    [[ -f "$f" ]] || continue
    # Search magic bytes in the file
    if xxd "$f" 2>/dev/null | grep -q "$FW_MAGIC_HEX"; then
        echo "      Found in: $f"
        # Extract from magic offset to end of file
        OFFSET=$(xxd "$f" | grep -m1 "$FW_MAGIC_HEX" | awk '{print $1}' | tr -d ':')
        OFFSET_DEC=$((16#$OFFSET))
        dd if="$f" of="$FW_OUT" bs=1 skip="$OFFSET_DEC" 2>/dev/null
        FW_FOUND=1
        break
    fi
done

if [[ $FW_FOUND -eq 0 ]]; then
    # Fallback: search directly for .bin
    BIN=$(find "$EXTRACT_DIR" -name "*.bin" -size +100k 2>/dev/null | head -1)
    if [[ -n "$BIN" ]]; then
        cp "$BIN" "$FW_OUT"
        FW_FOUND=1
        echo "      Found .bin directly: $BIN"
    fi
fi

if [[ $FW_FOUND -eq 0 ]]; then
    echo ""
    echo "ERROR: Firmware not found automatically."
    echo ""
    echo "Manual alternative:"
    echo "  1. Start a Windows VM or Bootcamp"
    echo "  2. Install the Xbox Wireless Adapter driver"
    echo "  3. Find file: C:\\Windows\\System32\\drivers\\mt7612us.bin"
    echo "     or: C:\\Windows\\System32\\drivers\\MSFTWUDF.sys"
    echo "  4. Copy into this folder and rename to: mt7612us.bin"
    echo ""
    echo "Alternative: use xone project on Linux:"
    echo "  https://github.com/dlundqvist/xone"
    echo "  There: install/firmware.sh extracts firmware automatically"
    exit 1
fi

echo ""
echo "Firmware extracted successfully: $FW_OUT ($(du -h "$FW_OUT" | cut -f1))"
echo ""
echo "Next step:"
echo "  make && sudo ./xbox_daemon"
