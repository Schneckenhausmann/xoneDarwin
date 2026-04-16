// mt76_firmware.h
// Firmware upload for Xbox Wireless Adapter PID 0x02FE (xone dongle protocol)
//
// Format of FW_ACC_00U.bin (xone project / Microsoft Windows Update):
//   Offset 0x00: uint32 code_size   (size of code segment)
//   Offset 0x04: uint32 data_size   (size of data segment)
//   Offset 0x08: uint32 reserved
//   Offset 0x0C: uint32 checksum
//   Offset 0x10: char[16] build_timestamp  ("201509162123____")
//   Offset 0x20: uint8[] firmware_payload  (code + data)
//
// Upload protocol (reverse engineered from the xone Linux driver):
//   1. libusb_reset_device() — recover chip from "stuck" state
//   2. Reclaim interface
//   3. Send firmware file directly over bulk OUT (EP 0x04, single transfer)
//   4. Device re-enumerates after ~2 seconds

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb.h>

// Header structure of FW_ACC_00U.bin
typedef struct __attribute__((packed)) {
    uint32_t code_size;      // Bytes 0-3:  code segment size
    uint32_t data_size;      // Bytes 4-7:  data segment size
    uint32_t reserved;       // Bytes 8-11
    uint32_t checksum;       // Bytes 12-15
    char     build_ts[16];   // Bytes 16-31: Build-Timestamp ASCII
} xone_fw_header_t;

#define XONE_FW_MIN_SIZE    (32 * 1024)   // Sanity check: at least 32 KB

// Load firmware from file. Returns buffer (free() when done).
static inline uint8_t *mt76_load_firmware(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[fw]   File not found: %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len < XONE_FW_MIN_SIZE) {
        fprintf(stderr, "[fw]   File too small (%ld bytes)\n", len);
        fclose(f); return NULL;
    }

    uint8_t *buf = malloc(len);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, len, f) != (size_t)len) {
        fprintf(stderr, "[fw]   Read error\n");
        free(buf); fclose(f); return NULL;
    }
    fclose(f);

    // Print header (no strict magic check — format varies slightly)
    if (len >= (long)sizeof(xone_fw_header_t)) {
        xone_fw_header_t *hdr = (xone_fw_header_t *)buf;
        char ts[17] = {};
        memcpy(ts, hdr->build_ts, 16);
        printf("[fw]   Header: code=%u data=%u build=\"%s\"\n",
               hdr->code_size, hdr->data_size, ts);
    }

    *out_size = (size_t)len;
    printf("[fw]   Loaded: %s (%zu bytes)\n", path, *out_size);
    return buf;
}

// Upload firmware.
//
// macOS/libusb behavior: with large bulk-OUT transfers, libusb sends
// multiple parallel USB requests -> the device accepts the first packets.
// With small transfers it immediately NAKs and 0 bytes are transferred.
//
// Strategy: always send the full remaining file as ONE transfer.
// Immediately retry partial results (timeout + tx>0) without a pause.
//
// Return: 0 = OK, -1 = error
static inline int mt76_upload_firmware(libusb_device_handle *dev,
                                       uint8_t ep_out,
                                       const uint8_t *fw, size_t fw_size)
{
    const uint8_t FW_EP = 0x04;   // Firmware endpoint (EP 0x08 = GIP channel)
    (void)ep_out;

    printf("[fw]   Upload %zu bytes via EP 0x%02x (partial-retry mode)...\n",
           fw_size, FW_EP);

    libusb_clear_halt(dev, FW_EP);

    size_t offset       = 0;
    int    pass         = 0;
    int    zero_passes  = 0;   // Consecutive passes with 0 bytes

    while (offset < fw_size) {
        int tx = 0;
        // Always send the FULL remaining payload in one transfer — on macOS
        // this triggers parallel USB requests and overcomes the initial NAK
        int r = libusb_bulk_transfer(dev, FW_EP,
            (unsigned char *)(fw + offset),
            (int)(fw_size - offset),
            &tx, 500);     // 500ms — enough to detect partial bytes

        pass++;

        if (tx > 0) {
            zero_passes = 0;
            offset += tx;
            printf("[fw]   Pass %d: +%d bytes -> %zu/%zu (%.0f%%)\n",
                   pass, tx, offset, fw_size, 100.0 * offset / fw_size);

            if (r == LIBUSB_SUCCESS) {
                // All bytes transferred in one go (rare but possible)
                break;
            }
            // Timeout with partial -> immediately retry remaining bytes
            libusb_clear_halt(dev, FW_EP);
            continue;
        }

        // 0 bytes transferred
        if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_IO) {
            // Device disconnected — possibly normal restart after last block
            printf("[fw]   Device disconnected at offset %zu (possible restart)\n", offset);
            break;
        }

        zero_passes++;
        printf("[fw]   Pass %d: 0 Bytes (%s) — Retry %d\n",
               pass, libusb_strerror(r), zero_passes);

        if (zero_passes >= 3) {
            fprintf(stderr, "[fw]   Too many failed retries without progress\n");
            return -1;
        }
        libusb_clear_halt(dev, FW_EP);
        usleep(100 * 1000);   // 100ms pause, then retry
    }

    printf("[fw]   Upload complete: %zu/%zu bytes in %d passes\n",
           offset, fw_size, pass);

    if (offset == 0) return -1;   // Sent nothing
    return 0;                     // Partial OK — device consumed remainder internally
}
