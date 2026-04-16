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
DRIVER_URL="http://download.windowsupdate.com/c/msdownload/update/driver/drvs/2017/07/1cd6a87c-623f-4407-a52d-c31be49e925c_e19f60808bdcbfbd3c3df6be3e71ffc52e43261e.cab"
CAB_FILE="xbox_driver.cab"

check_dep() {
    if ! command -v "$1" &>/dev/null; then
        echo "Error: '$1' not found."
        echo "Installation: brew install $2"
        exit 1
    fi
}

check_dep curl curl
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

# -- Step 3: Prefer direct firmware file -------------------------------------
echo "[3/3] Searching for firmware blob (MT7612U)..."

TARGET="$EXTRACT_DIR/FW_ACC_00U.bin"

if [[ -f "$TARGET" ]]; then
    cp "$TARGET" "./$FW_OUT"
    echo "      Found FW_ACC_00U.bin → renamed to $FW_OUT"
    FW_FOUND=1
else
    FW_MAGIC_HEX="23000000"
    FW_FOUND=0

    for f in "$EXTRACT_DIR"/*; do
        [[ -f "$f" ]] || continue

        if xxd "$f" 2>/dev/null | grep -q "$FW_MAGIC_HEX"; then
            echo "      Found in: $f"

            OFFSET=$(xxd "$f" | grep -m1 "$FW_MAGIC_HEX" | awk '{print $1}' | tr -d ':')
            OFFSET_DEC=$((16#$OFFSET))

            dd if="$f" of="$FW_OUT" bs=1 skip="$OFFSET_DEC" 2>/dev/null
            FW_FOUND=1
            break
        fi
    done
fi

if [[ "${FW_FOUND:-0}" -eq 0 && ! -f "$FW_OUT" ]]; then
    BIN=$(find "$EXTRACT_DIR" -name "*.bin" -size +100k 2>/dev/null | head -1)
    if [[ -n "$BIN" ]]; then
        cp "$BIN" "$FW_OUT"
        echo "      Found .bin directly: $BIN"
        FW_FOUND=1
    fi
fi

if [[ "${FW_FOUND:-0}" -eq 0 ]]; then
    echo ""
    echo "ERROR: Firmware not found automatically."
    exit 1
fi

echo ""
echo "Firmware extracted successfully: $FW_OUT ($(du -h "$FW_OUT" | cut -f1))"
echo ""
echo "Next step:"
echo "  make && sudo ./xbox_daemon"
