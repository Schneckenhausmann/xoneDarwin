# Xbox Wireless Adapter Daemon for macOS

A userspace driver for the Xbox One/Series X Wireless Adapter using libusb and IOHIDUserDevice.

## Project Structure

```
XboxDaemon/
├── xbox_daemon.c       # Main daemon with USB/HID handling
├── xbox_protocol.h     # GIP protocol definitions, HID descriptors
├── mt76_firmware.h     # Firmware loading (kept for compatibility)
├── mt76_init.h         # Full MT76 chip initialization (DMA upload, radio init)
├── mt7612us.bin        # Firmware binary (extracted from Windows driver)
├── extract_firmware.sh # Script to extract firmware from cab file
├── Makefile            # Build configuration
└── com.xbox.wireless.plist # launchd plist for auto-start

```

## Key Components

### 1. Firmware Upload (`mt76_init.h`)

The proper DMA-based firmware upload sequence for PID 0x02FE (Series X/S adapter):

1. **Check if firmware already loaded** - Read FCE_DMA_ADDR, reset if needed
2. **Set up DMA registers** - USB_U3DMA_CFG, FCE_PSE_CTRL, etc.
3. **Upload ILM** (Instruction Local Memory) - Chunked via EP 0x04 with TxInfoCommand headers
4. **Upload DLM** (Data Local Memory) - Same chunked approach
5. **Start firmware** - Write IVB address, send LOAD_IVB command
6. **Poll for completion** - Wait until FCE_DMA_ADDR != 0x01

Key insight: Each firmware chunk is wrapped in a TxInfoCommand header and uses control
transfers to configure DMA before bulk transfer:
```c
uint32_t hdr = mt76_make_txinfo_fw(chunk_size);  // infoType=0, port=2
// Control transfer: set FCE_DMA_ADDR, FCE_DMA_LEN
// Bulk transfer: send [hdr][chunk][padding] to EP 0x04
// Poll FCE_DMA_LEN for DMA completion
```

### 2. Radio Initialization (`mt76_radio_init()`)

After firmware upload, full radio initialization is required:

1. **Read ASIC version** - Sanity check that firmware is running
2. **MCU commands via EP 0x04** - All MCU commands use TxInfoCommand headers:
   - `MCU_CMD_FUN_SET_OP` - Select RX ring buffer
   - `MCU_CMD_POWER_SAVING_OP` - Turn radio ON (0x31)
   - `MCU_CMD_LOAD_CR` - Load BBP command register
3. **Register writes (~70)** - Control transfers to configure MAC/DMA/TX/RX
4. **EFUSE read** - Read MAC address, crystal trim
5. **Burst writes** - Write MAC/BSSID to chip registers via MCU_CMD_BURST_WRITE
6. **Channel config** - MCU_CMD_SWITCH_CHANNEL for all 11 channels
7. **Beacon setup** - Write beacon template, enable TSF/TBTT timers

### 3. GIP Protocol (`xbox_protocol.h`)

Gaming Input Protocol (GIP) frame format:
```
[command:1][client:1][sequence:1][length:1][payload...]
```

Key commands:
- `0x01` ACKNOWLEDGE - Response to status/guide button
- `0x02` ANNOUNCE - Controller connecting
- `0x03` STATUS - Controller status packet
- `0x0E` CTRL_ADDED - Controller connected
- `0x0F` CTRL_LOST - Controller disconnected
- `0x20` INPUT - Controller input data

### 4. HID Integration

Virtual gamepads are created using IOHIDUserDevice (semi-private IOKit API):
- No driver entitlements needed
- No SIP disabling required
- Controllers appear as native HID gamepads to macOS

## Build & Run

```bash
# Install dependencies
brew install libusb pkg-config

# Build
make

# Run (sudo required for USB device access)
sudo ./xbox_daemon
```

## Endpoints

For PID 0x02FE (Series X/S adapter):
- **EP 0x84** - Bulk IN (controller data from dongle)
- **EP 0x85** - Bulk IN (second channel)
- **EP 0x04** - Bulk OUT (MCU commands, firmware upload)
- **EP 0x05-0x09** - Bulk OUT (per-controller commands)

## References

- Based on xow by medusalix (GPL-2.0): https://github.com/medusalix/xow
- Xbox One Wireless Adapter protocol: https://github.com/medusalix/xow/issues/7
- MT76 chip documentation (OpenWrt): https://github.com/openwrt/mt76
