// mt76_init.h — MT76 chip radio initialization for Xbox Wireless Adapter
//
// Implements the full post-firmware initialization sequence:
//   1. USB vendor control transfers to configure MAC/DMA registers
//   2. MCU commands (radio on, BBP load, calibration, channels, beacon)
//
// Based on medusalix/xow (GPL-2.0) — ported to C / libusb for macOS
//
// Call: mt76_radio_init(dev, fw_buf, fw_size)
//   - fw_buf: the raw FW_ACC_00U.bin buffer
//   - fw_size: size (70008 bytes)
// Returns 0 on success, -1 on error.

#pragma once
#include <libusb.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

// ──────────────────────────────────────────────
// Vendor request codes
// ──────────────────────────────────────────────
#define MT_VEND_DEV_MODE    0x01
#define MT_VEND_MULTI_WRITE 0x06
#define MT_VEND_MULTI_READ  0x07
#define MT_VEND_WRITE_FCE   0x42
#define MT_VEND_WRITE_CFG   0x46
#define MT_VEND_READ_CFG    0x47

// ──────────────────────────────────────────────
// Register addresses  (all little-endian 32-bit)
// ──────────────────────────────────────────────
#define MT_ASIC_VERSION              0x0000
#define MT_CMB_CTRL                  0x0020
#define MT_EFUSE_CTRL                0x0024
#define MT_EFUSE_DATA_BASE           0x0028
#define MT_XO_CTRL5                  0x0114
#define MT_XO_CTRL6                  0x0118
#define MT_RF_PATCH                  0x0130
#define MT_WPDMA_GLO_CFG             0x0208
#define MT_WMM_AIFSN                 0x0214
#define MT_WMM_CWMIN                 0x0218
#define MT_WMM_CWMAX                 0x021c
#define MT_FCE_DMA_ADDR              0x0230
#define MT_FCE_DMA_LEN               0x0234
#define MT_USB_DMA_CFG               0x0238
#define MT_TSO_CTRL                  0x0250
#define MT_PBF_SYS_CTRL              0x0400
#define MT_PBF_CFG                   0x0404
#define MT_PBF_TX_MAX_PCNT           0x0408
#define MT_FCE_PSE_CTRL              0x0800
#define MT_FCE_L2_STUFF              0x080c
#define MT_TX_CPU_FROM_FCE_BASE_PTR  0x09a0
#define MT_TX_CPU_FROM_FCE_MAX_COUNT 0x09a4
#define MT_TX_CPU_FROM_FCE_CPU_DESC_IDX 0x09a8
#define MT_FCE_PDMA_GLOBAL_CONF      0x09c4
#define MT_PAUSE_ENABLE_CONTROL1     0x0a38
#define MT_FCE_SKIP_FS               0x0a6c
#define MT_USB_U3DMA_CFG             0x9018

#define MT_MAC_CSR0                  0x1000
#define MT_MAC_SYS_CTRL              0x1004
#define MT_MAC_ADDR_DW0              0x1008
#define MT_MAC_ADDR_DW1              0x100c   // upper 2 bytes of MAC + flags
#define MT_MAC_BSSID_DW0             0x1010
#define MT_MAC_BSSID_DW1             0x1014   // upper 2 bytes of BSSID
#define MT_MAX_LEN_CFG               0x1018
#define MT_AMPDU_MAX_LEN_20M1S       0x1030
#define MT_AMPDU_MAX_LEN_20M2S       0x1034
#define MT_LDO_CTRL_1                0x0070
#define MT_PWR_PIN_CFG               0x1204
#define MT_RF_BYPASS_0               0x0504
#define MT_RF_SETTING_0              0x050c
#define MT_RF_PA_MODE_ADJ0           0x1228
#define MT_RF_PA_MODE_ADJ1           0x122c
#define MT_DACCLK_EN_DLY_CFG         0x1264
#define MT_EDCA_CFG_BASE             0x1300   // AC(n) = BASE + n*4
#define MT_TX_PIN_CFG                0x1328
#define MT_TX_SW_CFG0                0x1330
#define MT_TX_SW_CFG1                0x1334
#define MT_TXOP_CTRL_CFG             0x1340
#define MT_TX_RTS_CFG                0x1344
#define MT_TX_TIMEOUT_CFG            0x1348
#define MT_TX_RETRY_CFG              0x134c
#define MT_CCK_PROT_CFG              0x1364
#define MT_OFDM_PROT_CFG             0x1368
#define MT_MM20_PROT_CFG             0x136c
#define MT_GF20_PROT_CFG             0x1374
#define MT_GF40_PROT_CFG             0x1378
#define MT_EXP_ACK_TIME              0x1380
#define MT_TX0_RF_GAIN_CORR          0x13a0
#define MT_TX1_RF_GAIN_CORR          0x13a4
#define MT_TX_ALC_CFG_2              0x13a8
#define MT_TX_ALC_CFG_3              0x13ac
#define MT_TX_ALC_CFG_0              0x13b0
#define MT_TX_ALC_CFG_4              0x13c0
#define MT_TX_PROT_CFG6              0x13e0
#define MT_TX_PROT_CFG7              0x13e4
#define MT_TX_PROT_CFG8              0x13e8
#define MT_PIFS_TX_CFG               0x13ec
#define MT_RX_FILTR_CFG              0x1400
#define MT_AUTO_RSP_CFG              0x1404
#define MT_LEGACY_BASIC_RATE         0x1408
#define MT_HT_BASIC_RATE             0x140c
#define MT_EXT_CCA_CFG               0x141c
#define MT_PN_PAD_MODE               0x150c
#define MT_TXOP_HLDR_ET              0x1608
#define MT_XIFS_TIME_CFG             0x1100
#define MT_BKOFF_SLOT_CFG            0x1104
#define MT_CH_TIME_CFG               0x110c
#define MT_BEACON_TIME_CFG           0x1114
// BBP: MT_BBP(AGC, 8) = 0x2300 + 8*4 = 0x2320
#define MT_BBP_AGC8                  0x2320
#define MT_BBP_AGC9                  0x2324

#define MT_BEACON_BASE               0xc000
#define MT_REGISTER_OFFSET           0x410000
#define MT_WCID_ADDR_BASE            0x1800   // WCID MAC table: entry n at BASE + n*8
#define MT_WCID_ATTR_BASE            0x1000   // WCID attribute table
#define MT_WLAN_TX_EP                0x04     // bulk OUT endpoint for WLAN frames

// ──────────────────────────────────────────────
// Firmware / MCU constants
// ──────────────────────────────────────────────
#define MT_FW_CHUNK_SIZE    0x3800
#define MT_MCU_ILM_OFFSET   0x80000
#define MT_MCU_DLM_OFFSET   (0x100000 + 0x10800)
#define MT_FW_RESET_IVB     0x01
#define MT_FW_LOAD_IVB      0x12
#define MT_DMA_COMPLETE     0xc0000000

// MCU commands
#define MCU_CMD_FUN_SET_OP       1
#define MCU_CMD_LOAD_CR          2
#define MCU_CMD_INTERNAL_FW_OP   3
#define MCU_CMD_BURST_WRITE      8
#define MCU_CMD_POWER_SAVING_OP  20
#define MCU_CMD_SWITCH_CHANNEL   30
#define MCU_CMD_CALIBRATION_OP   31
#define MCU_CMD_BEACON_OP        32

// MCU functions / modes
#define MCU_Q_SELECT        1
#define MCU_RADIO_ON        0x31
#define MCU_RADIO_OFF       0x30
#define MCU_RF_BBP_CR       2

// MCU calibration types
#define MCU_CAL_TEMP_SENSOR 2
#define MCU_CAL_RXDCOC      3
#define MCU_CAL_RC          4

// Channel bandwidth
#define MCU_CH_BW_20        0
#define MCU_CH_BW_40        1
#define MCU_CH_BW_80        2

// Firmware commands
#define FW_CMD_MAC_ADDR_SET 0
#define FW_CMD_CLIENT_ADD   1
#define FW_CMD_CH_CANDIDATES 7

// DMA port
#define MCU_CPU_TX_PORT     2

// TxInfo types
#define TX_INFO_NORMAL      0
#define TX_INFO_CMD         1

// EFUSE addresses
#define MT_EE_MAC_ADDR      0x004
#define MT_EE_XTAL_TRIM_1   0x03a
#define MT_EE_XTAL_TRIM_2   0x09e

// WLAN frame constants
#define MT_WLAN_MANAGEMENT  0x00
#define MT_WLAN_BEACON      0x08
#define MT_PHY_TYPE_OFDM    1

// ──────────────────────────────────────────────
// TxInfoCommand bit layout (32-bit little-endian)
//   bits 15:0  = length
//   bits 19:16 = sequence
//   bits 26:20 = command
//   bits 29:27 = port
//   bits 31:30 = infoType
// ──────────────────────────────────────────────
static inline uint32_t mt76_make_txinfo_cmd(uint8_t cmd, uint16_t len)
{
    return (uint32_t)len
         | ((uint32_t)0         << 16)   // sequence = 0
         | ((uint32_t)cmd       << 20)
         | ((uint32_t)MCU_CPU_TX_PORT << 27)
         | ((uint32_t)TX_INFO_CMD     << 30);
}

static inline uint32_t mt76_make_txinfo_fw(uint16_t len)
{
    return (uint32_t)len
         | ((uint32_t)MCU_CPU_TX_PORT << 27)
         | ((uint32_t)TX_INFO_NORMAL  << 30);
}

// ──────────────────────────────────────────────
// USB control transfer helpers
// ──────────────────────────────────────────────
static inline uint32_t mt76_ctrl_read(libusb_device_handle *dev,
                                       uint16_t addr,
                                       uint8_t  req)
{
    uint32_t val = 0;
    int r = libusb_control_transfer(dev,
        LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_IN,
        req,
        0,      // wValue
        addr,   // wIndex = register address
        (uint8_t *)&val,
        4,
        1000);
    if (r != 4)
        fprintf(stderr, "[mt76]  ctrl_read 0x%04x req=0x%02x: %s\n",
                addr, req, libusb_strerror(r));
    return val;
}

static inline void mt76_ctrl_write(libusb_device_handle *dev,
                                    uint16_t addr,
                                    uint32_t val,
                                    uint8_t  req)
{
    int r;
    if (req == MT_VEND_DEV_MODE) {
        // For DEV_MODE, address goes in wValue, no data
        r = libusb_control_transfer(dev,
            LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT,
            req,
            addr,   // wValue = address (= IVB command code)
            0,
            NULL,
            0,
            1000);
        if (r != 0)
            fprintf(stderr, "[mt76]  ctrl_write DEV_MODE 0x%04x: %s\n",
                    addr, libusb_strerror(r));
    } else {
        r = libusb_control_transfer(dev,
            LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT,
            req,
            0,      // wValue
            addr,   // wIndex = register address
            (uint8_t *)&val,
            4,
            1000);
        if (r != 4)
            fprintf(stderr, "[mt76]  ctrl_write 0x%04x = 0x%08x req=0x%02x: %s\n",
                    addr, val, req, libusb_strerror(r));
    }
}

// Shorthand for the most common request (MT_VEND_MULTI_WRITE)
#define MT76_W(addr, val) mt76_ctrl_write(dev, (addr), (val), MT_VEND_MULTI_WRITE)
#define MT76_R(addr)      mt76_ctrl_read(dev, (addr), MT_VEND_MULTI_READ)

// ──────────────────────────────────────────────
// Poll helper — wait up to ~1s for condition
// condition_fn(dev) returns 1 while still waiting, 0 when done
// ──────────────────────────────────────────────
typedef int (*mt76_poll_fn)(libusb_device_handle *dev, uint32_t arg);

static inline int mt76_poll(libusb_device_handle *dev, mt76_poll_fn fn, uint32_t arg)
{
    for (int i = 0; i < 200; i++) {
        if (!fn(dev, arg)) return 0;
        usleep(5000);  // 5ms
    }
    return -1;  // timeout
}

// Poll until MT_FCE_DMA_LEN == expected
static int mt76_poll_fce_done(libusb_device_handle *dev, uint32_t expected) {
    return mt76_ctrl_read(dev, MT_FCE_DMA_LEN, MT_VEND_READ_CFG) != expected;
}

// Poll until MT_FCE_DMA_ADDR != val
static int mt76_poll_fce_addr(libusb_device_handle *dev, uint32_t val) __attribute__((unused));
static int mt76_poll_fce_addr(libusb_device_handle *dev, uint32_t val) {
    return mt76_ctrl_read(dev, MT_FCE_DMA_ADDR, MT_VEND_READ_CFG) == val;
}

// Poll until EFUSE kick bit clears
static int mt76_poll_efuse_kick(libusb_device_handle *dev, uint32_t _unused) {
    (void)_unused;
    return (mt76_ctrl_read(dev, MT_EFUSE_CTRL, MT_VEND_MULTI_READ) >> 30) & 1;
}

// ──────────────────────────────────────────────
// MCU command — bulk write to EP 0x04
// Format: [TxInfoCommand (4B)] + [data] + [padding to 4B] + [4B zero terminator]
// ──────────────────────────────────────────────
static inline int mt76_mcu_cmd(libusb_device_handle *dev,
                                uint8_t cmd,
                                const uint8_t *data, int dlen)
{
    int padding = (4 - (dlen & 3)) & 3;
    int pktlen  = 4 + dlen + padding + 4;
    uint8_t *buf = (uint8_t *)calloc(1, pktlen);
    if (!buf) return -1;

    uint32_t hdr = mt76_make_txinfo_cmd(cmd, dlen + padding);
    memcpy(buf, &hdr, 4);
    if (data && dlen) memcpy(buf + 4, data, dlen);
    // padding + 4-byte zero terminator are already zero (calloc)

    int tx = 0;
    int r = libusb_bulk_transfer(dev, 0x04, buf, pktlen, &tx, 3000);
    free(buf);

    if (r != LIBUSB_SUCCESS && r != LIBUSB_ERROR_TIMEOUT) {
        fprintf(stderr, "[mt76]  mcu_cmd %d: %s\n", cmd, libusb_strerror(r));
        return -1;
    }
    return 0;
}

// MCU helper: send single uint32_t argument
static inline int mt76_mcu_u32(libusb_device_handle *dev, uint8_t cmd, uint32_t val)
{
    return mt76_mcu_cmd(dev, cmd, (uint8_t *)&val, 4);
}

// ──────────────────────────────────────────────
// EFUSE read (reads a block of `len` bytes at `addr`)
// Returns 0 on success, fills `out` buffer
// ──────────────────────────────────────────────
static inline int mt76_efuse_read(libusb_device_handle *dev,
                                   uint8_t addr, uint8_t len, uint8_t *out)
{
    memset(out, 0xff, len);

    // Read in 16-byte aligned blocks
    uint8_t block = addr & ~0x0f;

    uint32_t ctrl = mt76_ctrl_read(dev, MT_EFUSE_CTRL, MT_VEND_MULTI_READ);
    // Clear addressIn (bits 25:16), mode (bits 7:6), kick (bit 30)
    ctrl &= ~((0x3ff << 16) | (3 << 6) | (1 << 30));
    // Set addressIn = block, mode = 0 (MT_EE_READ), kick = 1
    ctrl |= ((uint32_t)block << 16) | (1u << 30);

    mt76_ctrl_write(dev, MT_EFUSE_CTRL, ctrl, MT_VEND_MULTI_WRITE);

    // Wait for kick bit to clear
    if (mt76_poll(dev, mt76_poll_efuse_kick, 0) != 0) {
        fprintf(stderr, "[mt76]  efuse_read timeout at 0x%02x\n", addr);
        return -1;
    }

    // Read data words
    for (uint8_t i = 0; i < len; i += 4) {
        uint8_t offset = (addr & 0x0c) + i;
        uint32_t word = mt76_ctrl_read(dev, MT_EFUSE_DATA_BASE + offset,
                                        MT_VEND_MULTI_READ);
        uint8_t copy = (len - i < 4) ? (len - i) : 4;
        memcpy(out + i, &word, copy);
    }
    return 0;
}

// ──────────────────────────────────────────────
// Firmware loading — DMA-based, chunked
// (proper xow approach: control writes + TxInfoCommand headers)
// ──────────────────────────────────────────────
static inline int mt76_fw_load_part(libusb_device_handle *dev,
                                     uint32_t dma_offset,
                                     const uint8_t *data, uint32_t total)
{
    for (uint32_t done = 0; done < total; done += MT_FW_CHUNK_SIZE) {
        uint32_t chunk = total - done;
        if (chunk > MT_FW_CHUNK_SIZE) chunk = MT_FW_CHUNK_SIZE;

        // Packet: [TxInfoCommand (4B)] + [chunk] + [4B alignment pad]
        int pktlen = 4 + chunk + 4;
        uint8_t *buf = (uint8_t *)calloc(1, pktlen);
        if (!buf) return -1;

        uint32_t hdr = mt76_make_txinfo_fw((uint16_t)chunk);
        memcpy(buf, &hdr, 4);
        memcpy(buf + 4, data + done, chunk);

        uint32_t addr = dma_offset + done;
        mt76_ctrl_write(dev, MT_FCE_DMA_ADDR, addr, MT_VEND_WRITE_CFG);
        mt76_ctrl_write(dev, MT_FCE_DMA_LEN, (uint32_t)chunk << 16, MT_VEND_WRITE_CFG);

        int tx = 0;
        int r = libusb_bulk_transfer(dev, 0x04, buf, pktlen, &tx, 10000);
        free(buf);

        if (r != LIBUSB_SUCCESS && r != LIBUSB_ERROR_TIMEOUT) {
            fprintf(stderr, "[mt76]  fw_load_part: bulk error %s\n", libusb_strerror(r));
            return -1;
        }

        // Poll until DMA signals completion
        uint32_t expected = ((uint32_t)chunk << 16) | MT_DMA_COMPLETE;
        if (mt76_poll(dev, mt76_poll_fce_done, expected) != 0) {
            fprintf(stderr, "[mt76]  fw_load_part: DMA timeout at +%u\n", done);
            return -1;
        }

        printf("[fw]   DMA +%u/%u\n", done + chunk, total);
    }
    return 0;
}

// ──────────────────────────────────────────────
// Full firmware upload (replaces raw bulk approach)
// fw points to the FW_ACC_00U.bin buffer (70008 bytes)
// ──────────────────────────────────────────────
static inline int mt76_upload_fw_dma(libusb_device_handle *dev,
                                      const uint8_t *fw, size_t fw_size)
{
    // FwHeader layout: ilmLength(4) + dlmLength(4) + ... + buildTime(16)
    if (fw_size < 32) return -1;

    uint32_t ilm_len, dlm_len;
    memcpy(&ilm_len, fw + 0, 4);
    memcpy(&dlm_len, fw + 4, 4);

    printf("[fw]   ILM=%u DLM=%u\n", ilm_len, dlm_len);

    if (32 + ilm_len + dlm_len > fw_size) {
        fprintf(stderr, "[fw]   firmware file too small\n");
        return -1;
    }

    // Check if firmware already running
    uint32_t fce = mt76_ctrl_read(dev, MT_FCE_DMA_ADDR, MT_VEND_READ_CFG);
    if (fce != 0) {
        const char *force_reload_env = getenv("XBOX_FW_FORCE_RELOAD");
        bool force_reload = force_reload_env && force_reload_env[0] && strcmp(force_reload_env, "0") != 0;

        if (!force_reload) {
            printf("[fw]   Firmware already loaded (FCE=0x%08x), reusing resident firmware\n", fce);
            printf("[fw]   Tip: set XBOX_FW_FORCE_RELOAD=1 to force IVB reset+reload\n");
            return 0;
        }

        printf("[fw]   Firmware already loaded (FCE=0x%08x), forcing reset/reload...\n", fce);
        uint32_t patch = mt76_ctrl_read(dev, MT_RF_PATCH, MT_VEND_READ_CFG);
        patch &= ~(1u << 19);
        mt76_ctrl_write(dev, MT_RF_PATCH, patch, MT_VEND_WRITE_CFG);
        mt76_ctrl_write(dev, MT_FW_RESET_IVB, 0, MT_VEND_DEV_MODE);

        // Poll until FCE != 0x80000000
        for (int i = 0; i < 200; i++) {
            if (mt76_ctrl_read(dev, MT_FCE_DMA_ADDR, MT_VEND_READ_CFG) != 0x80000000) break;
            usleep(5000);
        }
    }

    // Set up DMA: enable rxBulk (bit22) and txBulk (bit23)
    mt76_ctrl_write(dev, MT_USB_U3DMA_CFG, (1u<<22)|(1u<<23), MT_VEND_WRITE_CFG);
    MT76_W(MT_FCE_PSE_CTRL, 0x01);
    MT76_W(MT_TX_CPU_FROM_FCE_BASE_PTR, 0x400230);
    MT76_W(MT_TX_CPU_FROM_FCE_MAX_COUNT, 0x01);
    MT76_W(MT_TX_CPU_FROM_FCE_CPU_DESC_IDX, 0x01);
    MT76_W(MT_FCE_PDMA_GLOBAL_CONF, 0x44);
    MT76_W(MT_FCE_SKIP_FS, 0x03);

    // Upload ILM (instruction local memory)
    printf("[fw]   Uploading ILM (%u bytes)...\n", ilm_len);
    if (mt76_fw_load_part(dev, MT_MCU_ILM_OFFSET, fw + 32, ilm_len) != 0)
        return -1;

    // Upload DLM (data local memory)
    printf("[fw]   Uploading DLM (%u bytes)...\n", dlm_len);
    if (mt76_fw_load_part(dev, MT_MCU_DLM_OFFSET, fw + 32 + ilm_len, dlm_len) != 0)
        return -1;

    // Start firmware: write IVB address and boot command
    mt76_ctrl_write(dev, MT_FCE_DMA_ADDR, 0, MT_VEND_WRITE_CFG);
    mt76_ctrl_write(dev, MT_FW_LOAD_IVB, 0, MT_VEND_DEV_MODE);

    // Poll until MT_FCE_DMA_ADDR != 0x01 (firmware has started)
    for (int i = 0; i < 400; i++) {
        if (mt76_ctrl_read(dev, MT_FCE_DMA_ADDR, MT_VEND_READ_CFG) != 0x01) {
            printf("[fw]   Firmware started (FCE=0x%08x)\n",
                   mt76_ctrl_read(dev, MT_FCE_DMA_ADDR, MT_VEND_READ_CFG));
            return 0;
        }
        usleep(5000);
    }
    fprintf(stderr, "[fw]   Firmware start timeout\n");
    return -1;
}

// ──────────────────────────────────────────────
// initRegisters — ~40 control writes to configure
// MAC, DMA, TX/RX hardware (ported from xow)
// ──────────────────────────────────────────────
static inline int mt76_init_registers(libusb_device_handle *dev)
{
    // Reset MAC and BBP
    MT76_W(MT_MAC_SYS_CTRL, 0x03);  // RESET_CSR | RESET_BBP
    MT76_W(MT_USB_DMA_CFG, 0);
    MT76_W(MT_MAC_SYS_CTRL, 0);
    MT76_W(MT_PWR_PIN_CFG, 0);
    MT76_W(MT_LDO_CTRL_1, 0x6b006464);
    MT76_W(MT_WPDMA_GLO_CFG, 0x70);
    MT76_W(MT_WMM_AIFSN, 0x2273);
    MT76_W(MT_WMM_CWMIN, 0x2344);
    MT76_W(MT_WMM_CWMAX, 0x34aa);
    MT76_W(MT_FCE_DMA_ADDR, 0x041200);
    MT76_W(MT_TSO_CTRL, 0);
    MT76_W(MT_PBF_SYS_CTRL, 0x080c00);
    MT76_W(MT_PBF_TX_MAX_PCNT, 0x1fbf1f1f);
    MT76_W(MT_FCE_PSE_CTRL, 0x01);
    MT76_W(MT_MAC_SYS_CTRL, 0x0c);   // ENABLE_TX | ENABLE_RX
    MT76_W(MT_AUTO_RSP_CFG, 0x13);
    MT76_W(MT_MAX_LEN_CFG, 0x3e3fff);
    MT76_W(MT_AMPDU_MAX_LEN_20M1S, 0xfffc9855);
    MT76_W(MT_AMPDU_MAX_LEN_20M2S, 0xff);
    MT76_W(MT_BKOFF_SLOT_CFG, 0x0109);
    MT76_W(MT_PWR_PIN_CFG, 0);
    MT76_W(MT_EDCA_CFG_BASE + 0*4, 0x064320);
    MT76_W(MT_EDCA_CFG_BASE + 1*4, 0x0a4700);
    MT76_W(MT_EDCA_CFG_BASE + 2*4, 0x043238);
    MT76_W(MT_EDCA_CFG_BASE + 3*4, 0x03212f);
    MT76_W(MT_TX_PIN_CFG, 0x150f0f);
    MT76_W(MT_TX_SW_CFG0, 0x101001);
    MT76_W(MT_TX_SW_CFG1, 0x010000);
    MT76_W(MT_TXOP_CTRL_CFG, 0x10583f);
    MT76_W(MT_TX_TIMEOUT_CFG, 0x0a0f90);
    MT76_W(MT_TX_RETRY_CFG, 0x47d01f0f);
    MT76_W(MT_CCK_PROT_CFG, 0x03f40003);
    MT76_W(MT_OFDM_PROT_CFG, 0x03f40003);
    MT76_W(MT_MM20_PROT_CFG, 0x01742004);
    MT76_W(MT_GF20_PROT_CFG, 0x01742004);
    MT76_W(MT_GF40_PROT_CFG, 0x03f42084);
    MT76_W(MT_EXP_ACK_TIME, 0x2c00dc);
    MT76_W(MT_TX_ALC_CFG_2, 0x22160a00);
    MT76_W(MT_TX_ALC_CFG_3, 0x22160a76);
    MT76_W(MT_TX_ALC_CFG_0, 0x3f3f1818);
    MT76_W(MT_TX_ALC_CFG_4, 0x0606);
    MT76_W(MT_PIFS_TX_CFG, 0x060fff);
    MT76_W(MT_RX_FILTR_CFG, 0x017f17);
    MT76_W(MT_LEGACY_BASIC_RATE, 0x017f);
    MT76_W(MT_HT_BASIC_RATE, 0x8003);
    MT76_W(MT_PN_PAD_MODE, 0x02);
    MT76_W(MT_TXOP_HLDR_ET, 0x02);
    MT76_W(MT_TX_PROT_CFG6, 0xe3f42004);
    MT76_W(MT_TX_PROT_CFG7, 0xe3f42084);
    MT76_W(MT_TX_PROT_CFG8, 0xe3f42104);
    MT76_W(MT_DACCLK_EN_DLY_CFG, 0);
    MT76_W(MT_RF_PA_MODE_ADJ0, 0xee000000);
    MT76_W(MT_RF_PA_MODE_ADJ1, 0xee000000);
    MT76_W(MT_TX0_RF_GAIN_CORR, 0x0f3c3c3c);
    MT76_W(MT_TX1_RF_GAIN_CORR, 0x0f3c3c3c);
    MT76_W(MT_PBF_CFG, 0x1efebcf5);
    MT76_W(MT_PAUSE_ENABLE_CONTROL1, 0x0a);
    MT76_W(MT_RF_BYPASS_0, 0x7f000000);
    MT76_W(MT_RF_SETTING_0, 0x1a800000);
    MT76_W(MT_XIFS_TIME_CFG, 0x33a40e0a);
    MT76_W(MT_FCE_L2_STUFF, 0x03ff0223);
    MT76_W(MT_TX_RTS_CFG, 0);
    MT76_W(MT_BEACON_TIME_CFG, 0x0064);  // beacon interval = 100 TU (102.4ms)
    MT76_W(MT_EXT_CCA_CFG, 0xf0e4);
    MT76_W(MT_CH_TIME_CFG, 0x015f);

    // Calibrate crystal: read trim from EFUSE
    uint8_t trim2[4] = {};
    mt76_efuse_read(dev, MT_EE_XTAL_TRIM_2, 4, trim2);
    uint16_t xtal = ((uint16_t)trim2[3] << 8) | trim2[2];
    int8_t offset = xtal & 0x7f;
    if ((xtal & 0xff) == 0xff) offset = 0;
    else if (xtal & 0x80) offset = -offset;

    uint8_t val = xtal >> 8;
    if (val == 0x00 || val == 0xff) {
        uint8_t trim1[4] = {};
        mt76_efuse_read(dev, MT_EE_XTAL_TRIM_1, 4, trim1);
        val = ((uint16_t)trim1[3] << 8 | trim1[2]) & 0xff;
        if (val == 0x00 || val == 0xff) val = 0x14;
    }
    val = (val & 0x7f) + offset;

    uint32_t xo5 = mt76_ctrl_read(dev, MT_XO_CTRL5, MT_VEND_MULTI_READ);
    xo5 &= ~0x00007f00u;                  // clear bits 14:8
    xo5 |= ((uint32_t)val << 8) & 0x00007f00u;
    mt76_ctrl_write(dev, MT_XO_CTRL5, xo5, MT_VEND_WRITE_CFG);
    mt76_ctrl_write(dev, MT_XO_CTRL6, 0x00007f00u, MT_VEND_WRITE_CFG);
    MT76_W(MT_CMB_CTRL, 0x0091a7ff);

    // AGC init
    MT76_W(MT_BBP_AGC8, 0x18365efa);
    MT76_W(MT_BBP_AGC9, 0x18365efa);

    return 0;
}

// ──────────────────────────────────────────────
// Read MAC address from EFUSE, correct prefix if needed
// ──────────────────────────────────────────────
static inline int mt76_read_mac(libusb_device_handle *dev, uint8_t mac[6])
{
    uint8_t raw[6] = {};
    if (mt76_efuse_read(dev, MT_EE_MAC_ADDR, 6, raw) != 0)
        return -1;

    memcpy(mac, raw, 6);

    // Controllers only connect to 62:45:bx:xx:xx:xx
    if (mac[0] != 0x62) {
        mac[0] = 0x62;
        mac[1] = 0x45;
        mac[2] = 0xbd;
    }

    printf("[mt76]  MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

// Write MAC to hardware registers via MCU CMD_BURST_WRITE
static inline int mt76_burst_write(libusb_device_handle *dev,
                                    uint32_t hw_addr,
                                    const uint8_t *data, int len)
{
    uint32_t idx = hw_addr + MT_REGISTER_OFFSET;
    int dlen = 4 + len;
    uint8_t *buf = (uint8_t *)calloc(1, dlen);
    if (!buf) return -1;
    memcpy(buf, &idx, 4);
    memcpy(buf + 4, data, len);
    int r = mt76_mcu_cmd(dev, MCU_CMD_BURST_WRITE, buf, dlen);
    free(buf);
    return r;
}

// Write MAC address + BSSID to chip registers.
// Must use DIRECT register writes (MT_VEND_MULTI_WRITE vendor control),
// NOT MCU burst_write which targets SRAM (wrong address space for 0x1008/0x1010).
// Matches xow: writeRegister(MT_MAC_ADDR_DW0/DW1) + writeRegister(MT_MAC_BSSID_DW0/DW1)
static inline int mt76_write_mac_hw(libusb_device_handle *dev, const uint8_t mac[6])
{
    // Pack MAC into two 32-bit words (little-endian)
    // DW0: mac[0..3], DW1: mac[4..5] (upper 16 bits = 0)
    uint32_t dw0 = (uint32_t)mac[0]
                 | ((uint32_t)mac[1] << 8)
                 | ((uint32_t)mac[2] << 16)
                 | ((uint32_t)mac[3] << 24);
    uint32_t dw1 = (uint32_t)mac[4]
                 | ((uint32_t)mac[5] << 8);

    // Write MAC address registers (direct vendor control = same as MT76_W)
    MT76_W(MT_MAC_ADDR_DW0,  dw0);
    MT76_W(MT_MAC_ADDR_DW1,  dw1);
    MT76_W(MT_MAC_BSSID_DW0, dw0);
    MT76_W(MT_MAC_BSSID_DW1, dw1);

    // Verify write by reading back
    uint32_t rb_dw0 = MT76_R(MT_MAC_ADDR_DW0);
    uint32_t rb_dw1 = MT76_R(MT_MAC_ADDR_DW1);
    printf("[mt76]  MAC_ADDR_DW0 = 0x%08x  (expect 0x%08x) %s\n",
           rb_dw0, dw0, rb_dw0 == dw0 ? "OK" : "MISMATCH!");
    printf("[mt76]  MAC_ADDR_DW1 = 0x%08x  (expect 0x%08x) %s\n",
           rb_dw1, dw1, rb_dw1 == dw1 ? "OK" : "MISMATCH!");
    printf("[mt76]  MAC/BSSID set: %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    return 0;
}

// ──────────────────────────────────────────────
// Channel configuration
// ──────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t  channel;
    uint8_t  pad1;
    uint16_t pad2;
    uint16_t txRxSetting;  // 0x0101 = stream 1
    uint16_t pad3;
    uint64_t pad4;
    uint8_t  bandwidth;
    uint8_t  txPower;
    uint8_t  scan;
    uint8_t  unknown;
} mt76_ch_cfg_t;

static inline int mt76_configure_channel(libusb_device_handle *dev,
                                          uint8_t ch, uint8_t bw,
                                          uint8_t scan, uint8_t power)
{
    mt76_ch_cfg_t cfg = {};
    cfg.channel     = ch;
    cfg.txRxSetting = 0x0101;
    cfg.bandwidth   = bw;
    cfg.txPower     = power;
    cfg.scan        = scan;
    return mt76_mcu_cmd(dev, MCU_CMD_SWITCH_CHANNEL, (uint8_t *)&cfg, sizeof(cfg));
}

static inline int mt76_init_channels(libusb_device_handle *dev)
{
    // Read TX power from EFUSE for better range
    uint8_t pwr = 0x18;  // default 24 dBm
    uint8_t efuse_pwr[4] = {};
    if (mt76_efuse_read(dev, 0x00, 4, efuse_pwr) == 0) {
        // xow reads power from offset 0x00 bytes 2-3
        uint16_t pwr_val = ((uint16_t)efuse_pwr[3] << 8) | efuse_pwr[2];
        if (pwr_val != 0 && pwr_val != 0xffff) {
            // Extract power value and clamp to safe range
            pwr = (pwr_val >> 8) & 0x3f;
            if (pwr < 0x10) pwr = 0x10;
            if (pwr > 0x30) pwr = 0x30;
        }
    }
    printf("[mt76]  Channel TX power: 0x%02x (%d dBm)\n", pwr, pwr);

    // Configure each channel
    printf("[mt76]  Configuring channels...\n");
    mt76_configure_channel(dev, 0x01, MCU_CH_BW_20, 1, pwr);
    mt76_configure_channel(dev, 0x06, MCU_CH_BW_20, 1, pwr);
    mt76_configure_channel(dev, 0x0b, MCU_CH_BW_20, 1, pwr);
    mt76_configure_channel(dev, 0x24, MCU_CH_BW_40, 1, pwr);
    mt76_configure_channel(dev, 0x28, MCU_CH_BW_40, 0, pwr);
    mt76_configure_channel(dev, 0x2c, MCU_CH_BW_40, 1, pwr);
    mt76_configure_channel(dev, 0x30, MCU_CH_BW_40, 0, pwr);
    mt76_configure_channel(dev, 0x95, MCU_CH_BW_80, 1, pwr);
    mt76_configure_channel(dev, 0x99, MCU_CH_BW_80, 0, pwr);
    mt76_configure_channel(dev, 0x9d, MCU_CH_BW_80, 1, pwr);
    mt76_configure_channel(dev, 0xa1, MCU_CH_BW_80, 0, pwr);
    mt76_configure_channel(dev, 0xa5, MCU_CH_BW_80, 0, pwr);

    // Send channel candidate list to firmware
    // Format: 14 pairs → 14 uint32_t values
    const uint8_t cands[] = {
        0x01, 0xa5,
        0x0b, 0x01,
        0x06, 0x0b,
        0x24, 0x28,
        0x2c, 0x30,
        0x95, 0x99,
        0x9d, 0xa1
    };
    uint8_t cand_vals[14 * 4] = {};
    for (int i = 0; i < 14; i++)
        cand_vals[i*4] = cands[i];  // little-endian uint32

    // sendFirmwareCommand(FW_CMD_CH_CANDIDATES=7, cand_vals)
    uint8_t fw_payload[4 + sizeof(cand_vals)] = {};
    uint32_t fw_cmd = FW_CMD_CH_CANDIDATES;
    memcpy(fw_payload, &fw_cmd, 4);
    memcpy(fw_payload + 4, cand_vals, sizeof(cand_vals));
    return mt76_mcu_cmd(dev, MCU_CMD_INTERNAL_FW_OP,
                        fw_payload, sizeof(fw_payload));
}

// Inform firmware about a newly associated peer (xow FW_CLIENT_ADD path).
static inline int mt76_fw_client_add(libusb_device_handle *dev, uint8_t wcid)
{
    if (wcid == 0) return -1;

    uint8_t fw_payload[12] = {};
    uint32_t fw_cmd = FW_CMD_CLIENT_ADD;
    memcpy(fw_payload, &fw_cmd, 4);

    // xow payload:
    // [wcid-1, 0x00, 0x00, 0x00, 0x40, 0x1f, 0x00, 0x00]
    fw_payload[4] = (uint8_t)(wcid - 1);
    fw_payload[8] = 0x40;
    fw_payload[9] = 0x1f;

    int r = mt76_mcu_cmd(dev, MCU_CMD_INTERNAL_FW_OP, fw_payload, sizeof(fw_payload));
    if (r == 0) {
        printf("[mt76] FW client add WCID %u\n", wcid);
    }
    return r;
}

// ──────────────────────────────────────────────
// Beacon write — matches xow exactly
//
// Frame layout (from xow/dongle/mt76.cpp):
//   TxWi (20B) + WlanFrame (24B) + BeaconFrame (14B) + VendorIE (18B) = 76B
//
// Key points verified against xow source:
//   - phyType = OFDM (1), NOT CCK
//   - capabilityInfo = 0xc631
//   - mpduByteCount = 56 (24+14+18, no extra IEs)
//   - BeaconFrame.ssid = 0x0000 (empty SSID element inline in struct)
//   - No Supported Rates IE, no DS Params IE
// ──────────────────────────────────────────────
static inline int mt76_write_beacon(libusb_device_handle *dev,
                                     const uint8_t mac[6], int pairing, uint8_t channel)
{
    // ── Vendor IE (18 bytes) — Microsoft Xbox pairing marker ──────────────
    uint8_t vendor_ie[18] = {
        0xdd, 0x10,                    // tag=0xdd (Vendor Specific), len=16
        0x00, 0x50, 0xf2,              // OUI: Microsoft (00:50:F2)
        0x11,                          // OUI type: Xbox wireless
        0x01, 0x10,                    // subtype / version
        0x00, 0x24, 0x9d, 0x99,        // observed on real dongle beacons
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00
    };

    // ── TxWi (20 bytes) ────────────────────────────────────────────────────
    // Verified from xow:
    //   phyType = OFDM = 1  → bits 29-31 of word0 → byte[3] = 0x20
    //   timestamp = 1       → bit 3 of word0       → byte[0] = 0x08
    //   nseq = 1            → bit 1 of word1        → byte[4] = 0x02
    //   mpduByteCount = 56  → bits 16-29 of word1   → byte[6] = 0x38
    uint8_t ds_ie[3] = { 0x03, 0x01, channel };

    // mpdu = WlanFrame(24) + BeaconFrame(14) + DS IE(3) + VendorIE(18) = 59 bytes
    uint8_t txwi[20] = {};
    txwi[0] = 0x08;   // timestamp=1 (bit3 of word0)
    txwi[3] = 0x20;   // phyType=OFDM(1) at bits 29-31 of word0 → byte[3]=0x20
    txwi[4] = 0x02;   // nseq=1 at bit 1 of word1
    txwi[6] = 0x3b;   // mpduByteCount=59, low byte (bits 16-23 of word1)
    txwi[7] = 0x00;   // mpduByteCount high bits (56 < 256, so zero)

    // ── 802.11 WlanFrame header (24 bytes) ─────────────────────────────────
    typedef struct __attribute__((packed)) {
        uint16_t fc;       // 0x0080 = Beacon
        uint16_t dur;      // 0
        uint8_t  dst[6];   // ff:ff:ff:ff:ff:ff
        uint8_t  src[6];   // our MAC
        uint8_t  bss[6];   // our MAC (BSSID)
        uint16_t seq;      // 0
    } wf_t;
    wf_t wf = {};
    wf.fc = 0x0080;
    memset(wf.dst, 0xff, 6);
    memcpy(wf.src, mac, 6);
    memcpy(wf.bss, mac, 6);

    // ── BeaconFrame fixed params (14 bytes) ────────────────────────────────
    // timestamp(8) + interval(2) + capability(2) + ssid_ie(2)
    // ssid_ie = 0x0000 = {tag=0, len=0} = empty SSID element
    typedef struct __attribute__((packed)) {
        uint64_t ts;       // 0 — MT76 fills TSF when TxWi.timestamp=1
        uint16_t interval; // 0x0064 = 100 TU (102.4ms)
        uint16_t cap;      // 0xc631 — matches xow exactly
        uint16_t ssid;     // 0x0000 = IE(tag=0, len=0) = empty SSID
    } bf_t;
    bf_t bf = {};
    bf.interval = 0x0064;
    bf.cap      = 0xc631;
    // bf.ssid = 0x0000 (zero-initialized = SSID IE with empty string)

    // ── Assemble burst_write payload ────────────────────────────────────────
    // Format: [hw_addr(4)] [txwi(20)] [wf(24)] [bf(14)] [ds_ie(3)] [vendor_ie(18)]
    uint32_t hw_addr = MT_BEACON_BASE + MT_REGISTER_OFFSET;
    uint8_t payload[4 + 20 + 24 + 14 + 3 + 18] = {};
    int off = 0;
    memcpy(payload + off, &hw_addr,   4);  off += 4;
    memcpy(payload + off, txwi,      20);  off += 20;
    memcpy(payload + off, &wf,       24);  off += 24;
    memcpy(payload + off, &bf,       14);  off += 14;
    memcpy(payload + off, ds_ie,      3);  off += 3;
    memcpy(payload + off, vendor_ie, 18);  off += 18;

    printf("[mt76]  Writing beacon (pairing=%d, len=%d)...\n", pairing, off);
    if (mt76_mcu_cmd(dev, MCU_CMD_BURST_WRITE, payload, off) != 0) {
        fprintf(stderr, "[mt76]  Beacon burst_write failed\n");
        return -1;
    }

    // Enable TSF timer, TBTT timer, beacon TX
    uint32_t btcfg = MT76_R(MT_BEACON_TIME_CFG);
    printf("[mt76]  BeaconTimeCfg before: 0x%08x\n", btcfg);
    btcfg |= (1u << 16)  // tsfTimerEnabled
           | (3u << 17)  // tsfSyncMode = 3
           | (1u << 19)  // tbttTimerEnabled
           | (1u << 20); // transmitBeacon
    MT76_W(MT_BEACON_TIME_CFG, btcfg);
    btcfg = MT76_R(MT_BEACON_TIME_CFG);
    printf("[mt76]  BeaconTimeCfg after: 0x%08x\n", btcfg);

    // Calibrate RXDCOC after beacon setup (val=0, as in xow)
    {
        uint8_t cal[8] = {};
        uint32_t type = MCU_CAL_RXDCOC, val = 0;
        memcpy(cal, &type, 4); memcpy(cal+4, &val, 4);
        mt76_mcu_cmd(dev, MCU_CMD_CALIBRATION_OP, cal, 8);
    }

    return 0;
}

// ──────────────────────────────────────────────
// WLAN TX: wrap 802.11 frame in TxInfo + TxWi and send on EP 0x04
// frame_data: raw 802.11 frame (MAC header + payload, NO FCS)
// wcid: 0xff for broadcast/unknown, 1-127 for known peers
// ──────────────────────────────────────────────
static inline int mt76_wlan_tx(libusb_device_handle *dev,
                                const uint8_t *frame_data, int frame_len,
                                uint8_t wcid)
{
    // TxInfoPacket (4B) + TxWi (20B) + frame + 0-3B padding + 4B terminator
    uint8_t pkt[4 + 20 + 2048 + 3 + 4];
    if (frame_len > 2048) {
        fprintf(stderr, "[tx]  frame too large: %d\n", frame_len);
        return -1;
    }

    int txwi_len = 20;
    int mpdu_len = txwi_len + frame_len;
    int pad_len = (4 - (mpdu_len & 3)) & 3;

    // TxInfoPacket word (matches xow sendWlanPacket path):
    // length(15:0), is80211(19), wiv(24), qsel=EDCA(26:25=2), port=WLAN(29:27=0)
    uint32_t txinfo = (uint32_t)(mpdu_len + pad_len);
    txinfo |= (1u << 19);          // is80211
    txinfo |= (1u << 24);          // wiv valid
    txinfo |= (2u << 25);          // qsel = EDCA
    memcpy(pkt, &txinfo, 4);

    // TxWi (20 bytes = 5 x uint32)
    // Keep this close to xow defaults for management/data packet sends:
    // word0: phyType(bits29-31)=1(OFDM)
    // word1: ack(bit0)=1 for unicast, wcid(bits8-15), mpduByteCount(bits16-29)=frame_len
    memset(pkt + 4, 0, 20);
    pkt[4 + 3] = 0x20;  // phyType = OFDM (1) in bits 29-31 of word0

    // Destination address is Addr1 at frame_data[4].
    // Set ACK required for unicast management/data responses.
    bool dst_is_multicast = (frame_len >= 10) && ((frame_data[4] & 0x01) != 0);
    pkt[4 + 4] = dst_is_multicast ? 0x00 : 0x01; // ack=1 for unicast

    pkt[4 + 5] = wcid;  // wcid in bits 8-15 of word1
    pkt[4 + 6] = (uint8_t)(frame_len & 0xff);          // mpduByteCount low
    pkt[4 + 7] = (uint8_t)((frame_len >> 8) & 0x3f);   // mpduByteCount high (14-bit field)

    memcpy(pkt + 24, frame_data, (size_t)frame_len);
    if (pad_len)
        memset(pkt + 24 + frame_len, 0, (size_t)pad_len);
    memset(pkt + 24 + frame_len + pad_len, 0, 4);

    int total = 4 + txwi_len + frame_len + pad_len + 4;
    int tx = 0;
    int r = libusb_bulk_transfer(dev, MT_WLAN_TX_EP,
                                 pkt, total, &tx, 2000);
    if (r != LIBUSB_SUCCESS) {
        fprintf(stderr, "[tx]  WLAN TX error: %s (sent %d/%d)\n",
                libusb_strerror(r), tx, total);
        return -1;
    }
    printf("[tx]   WLAN TX %d bytes on EP 0x%02x (frame=%d)\n",
           tx, MT_WLAN_TX_EP, frame_len);
    return 0;
}

// ──────────────────────────────────────────────
// WCID table: register a peer's MAC at slot wcid (1-based).
// Must be called after 802.11 association so MT76 knows the peer.
// ──────────────────────────────────────────────
static inline void mt76_add_wcid(libusb_device_handle *dev,
                                  uint8_t wcid, const uint8_t *mac)
{
    // WCID table entry: 8 bytes = MAC (6) + 2 reserved
    // Address: MT_WCID_ADDR_BASE + wcid * 8
    uint32_t base = MT_WCID_ADDR_BASE + (uint32_t)wcid * 8;

    uint32_t dw0, dw1;
    // Store MAC as: dw0 = mac[0..3] LE, dw1 = mac[4..5] | 0 padding
    memcpy(&dw0, mac, 4);
    dw1 = 0;
    memcpy(&dw1, mac + 4, 2);

    MT76_W(base,     dw0);
    MT76_W(base + 4, dw1);

    printf("[mt76] WCID[%d] set to %02x:%02x:%02x:%02x:%02x:%02x\n",
           wcid, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

// ──────────────────────────────────────────────
// Full radio init — call after firmware is running
// (Either from DMA upload or our previous raw approach)
// ──────────────────────────────────────────────
static inline int mt76_radio_init(libusb_device_handle *dev, uint8_t *mac_out)
{
    printf("[mt76]  Radio initialization...\n");

    // Quick sanity check: read ASIC version
    uint32_t asic = MT76_R(MT_ASIC_VERSION);
    printf("[mt76]  ASIC version: 0x%08x\n", asic);
    if (asic == 0x00000000 || asic == 0xffffffff) {
        fprintf(stderr, "[mt76]  ERROR: ASIC version read failed — "
                        "firmware may not be running\n");
        return -1;
    }

    // 1. Select RX ring buffer 1
    {
        uint8_t d[8] = {};
        uint32_t fn = MCU_Q_SELECT, val = 1;
        memcpy(d, &fn, 4); memcpy(d+4, &val, 4);
        mt76_mcu_cmd(dev, MCU_CMD_FUN_SET_OP, d, 8);
    }

    // 2. Turn radio on
    mt76_mcu_u32(dev, MCU_CMD_POWER_SAVING_OP, MCU_RADIO_ON);

    // 3. Load BBP command register
    mt76_mcu_u32(dev, MCU_CMD_LOAD_CR, MCU_RF_BBP_CR);

    // 4. Initialize MAC/DMA registers
    if (mt76_init_registers(dev) != 0) return -1;

    // 5. Read and write MAC address
    uint8_t mac[6] = {};
    if (mt76_read_mac(dev, mac) != 0) {
        // Use a default if EFUSE read fails
        mac[0]=0x62; mac[1]=0x45; mac[2]=0xbd;
        mac[3]=0x01; mac[4]=0x02; mac[5]=0x03;
        printf("[mt76]  Using fallback MAC\n");
    }
    mt76_write_mac_hw(dev, mac);

    // 6. Set MAC address in firmware
    {
        uint8_t fw_payload[4 + 6] = {};
        uint32_t fw_cmd = FW_CMD_MAC_ADDR_SET;
        memcpy(fw_payload, &fw_cmd, 4);
        memcpy(fw_payload + 4, mac, 6);
        mt76_mcu_cmd(dev, MCU_CMD_INTERNAL_FW_OP, fw_payload, sizeof(fw_payload));
    }

    // 7. Reset RF/BBP
    MT76_W(MT_MAC_SYS_CTRL, 0);
    MT76_W(MT_RF_BYPASS_0, 0);
    MT76_W(MT_RF_SETTING_0, 0);

    // 8. Calibrate chip
    {
        uint8_t cal[8] = {};
        uint32_t type, val;

        type=MCU_CAL_TEMP_SENSOR; val=0;
        memcpy(cal, &type, 4); memcpy(cal+4, &val, 4);
        mt76_mcu_cmd(dev, MCU_CMD_CALIBRATION_OP, cal, 8);

        type=MCU_CAL_RXDCOC; val=1;
        memcpy(cal, &type, 4); memcpy(cal+4, &val, 4);
        mt76_mcu_cmd(dev, MCU_CMD_CALIBRATION_OP, cal, 8);

        type=MCU_CAL_RC; val=0;
        memcpy(cal, &type, 4); memcpy(cal+4, &val, 4);
        mt76_mcu_cmd(dev, MCU_CMD_CALIBRATION_OP, cal, 8);
    }

    // 9. Enable TX and RX
    MT76_W(MT_MAC_SYS_CTRL, 0x0c);  // ENABLE_TX | ENABLE_RX

    // 9.5 Re-enable USB bulk DMA (was zeroed during initRegisters reset).
    // MT_USB_DMA_CFG: bit22=RX_BULK_EN, bit23=TX_BULK_EN
    // Also re-set U3DMA in case firmware reset it after boot.
    mt76_ctrl_write(dev, MT_USB_U3DMA_CFG, (1u<<22)|(1u<<23), MT_VEND_WRITE_CFG);
    MT76_W(MT_USB_DMA_CFG, (1u<<22)|(1u<<23));
    printf("[mt76]  USB DMA re-enabled (DMA_CFG=0x%08x)\n",
           MT76_R(MT_USB_DMA_CFG));

    // 10. Configure channels
    if (mt76_init_channels(dev) != 0) return -1;

    // 11. Start on channel 44. Existing captures show the dongle operating on
    // UNII-1 5 GHz channels (36/40/44/48), not 2.4 GHz pairing channels.
    if (mt76_configure_channel(dev, 0x2c, MCU_CH_BW_20, 1, 0x18) != 0) return -1;

    // 12. Write beacon (pairing=true, channel=44 initially)
    printf("[mt76]  Setting up beacon with pairing enabled on channel 44...\n");
    if (mt76_write_beacon(dev, mac, 1, 0x2c) != 0) return -1;
    printf("[mt76]  Beacon active, dongle ready for pairing\n");
    printf("[mt76]  Our BSSID: %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

    if (mac_out) memcpy(mac_out, mac, 6);

    // Final status dump
    {
        uint32_t mac_sys  = MT76_R(MT_MAC_SYS_CTRL);
        uint32_t usb_dma  = MT76_R(MT_USB_DMA_CFG);
        uint32_t btcfg    = MT76_R(MT_BEACON_TIME_CFG);
        uint32_t bssid_r  = MT76_R(MT_MAC_BSSID_DW0);
        printf("[mt76]  STATUS: MAC_SYS_CTRL=0x%08x USB_DMA=0x%08x BEACON_CFG=0x%08x\n",
               mac_sys, usb_dma, btcfg);
        printf("[mt76]  STATUS: BSSID_DW0=0x%08x (should be 0x%02x%02x%02x%02x)\n",
               bssid_r, mac[3],mac[2],mac[1],mac[0]);
        // Sanity: MAC_SYS_CTRL should be 0x0c (TX+RX enabled)
        //         USB_DMA should be 0x00c00000 (bits 22+23)
        //         BEACON_CFG should have bit 20 set (transmitBeacon)
        if ((mac_sys & 0x0c) != 0x0c)
            fprintf(stderr, "[mt76]  WARNING: MAC TX/RX not enabled!\n");
        if ((usb_dma & (1u<<22)) == 0)
            fprintf(stderr, "[mt76]  WARNING: USB RX DMA not enabled!\n");
        if ((btcfg & (1u<<20)) == 0)
            fprintf(stderr, "[mt76]  WARNING: Beacon TX not enabled!\n");
    }

    printf("[mt76]  Radio init complete\n");
    return 0;
}

// ──────────────────────────────────────────────
// Pairing channel rotation for small dongle (PID 0x02FE)
// The small dongle needs active channel scanning during pairing
// Based on xone PR #168: rotate through channels every 2 seconds
// ──────────────────────────────────────────────
static inline uint8_t mt76_pairing_channel_scan(libusb_device_handle *dev)
{
    // Channel list for pairing scan (from xone)
    const uint8_t scan_channels[] = { 0x24, 0x28, 0x2c, 0x30 };
    static int ch_idx = 0;
    static uint64_t last_switch = 0;
    static uint8_t mac[6] = {0};

    // Get MAC on first call
    if (mac[0] == 0) {
        uint8_t efuse[16];
        if (mt76_efuse_read(dev, 0x00, 16, efuse) == 0) {
            memcpy(mac, efuse + 4, 6);
            // Fix Xbox MAC prefix
            mac[0] = 0x62;
            mac[1] = 0x45;
            mac[2] = 0xbd;
        }
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    if (now - last_switch > 2000) {  // Switch every 2 seconds
        ch_idx = (ch_idx + 1) % 4;
        last_switch = now;
        printf("[mt76]  Pairing scan: switching to channel %d\n", scan_channels[ch_idx]);

        // Switch channel
        mt76_configure_channel(dev, scan_channels[ch_idx], MCU_CH_BW_20, 1, 0x18);

        // Update beacon with new channel (important for pairing!)
        // The beacon's vendor IE contains channel info
        if (mac[0] != 0) {
            mt76_write_beacon(dev, mac, 1, scan_channels[ch_idx]);
        }
    }

    return scan_channels[ch_idx];
}
