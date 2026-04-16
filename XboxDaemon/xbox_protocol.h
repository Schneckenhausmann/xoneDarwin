// xbox_protocol.h
// GIP (Gaming Input Protocol) definitions for Xbox One Wireless Adapter
// USB VID 0x045E, PID 0x02E6 (v1) / 0x02FD (v2)

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// ──────────────────────────────────────────────
// USB identifiers
// ──────────────────────────────────────────────
#define XBOX_VID            0x045E
#define XBOX_PID_V1         0x02E6   // original large dongle
#define XBOX_PID_V2         0x02FD   // slim white dongle
#define XBOX_PID_V3         0x02FE   // Xbox Series X/S wireless adapter

#define USB_BUF_SIZE        4096   // MT76 RX frames can span many 512-byte USB packets

// ──────────────────────────────────────────────
// GIP frame header (4 bytes, little-endian)
// ──────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t command;
    uint8_t client;    // 0 = dongle, 1-8 = controller slot
    uint8_t sequence;
    uint8_t length;    // payload length (after header)
} gip_header_t;

// ──────────────────────────────────────────────
// GIP commands
// ──────────────────────────────────────────────
#define GIP_CMD_ACKNOWLEDGE     0x01
#define GIP_CMD_ANNOUNCE        0x02
#define GIP_CMD_STATUS          0x03
#define GIP_CMD_POWER           0x05
#define GIP_CMD_GUIDE_BTN       0x07
#define GIP_CMD_RUMBLE          0x09
#define GIP_CMD_LED             0x0A
#define GIP_CMD_SERIAL          0x1E
#define GIP_CMD_INPUT           0x20
#define GIP_CMD_INPUT_OVERFLOW  0x26
#define GIP_CMD_CTRL_ADDED      0x0E
#define GIP_CMD_CTRL_LOST       0x0F

// GIP frame types (high nibble of header byte 1)
#define GIP_TYPE_COMMAND        0x00
#define GIP_TYPE_ACK            0x01
#define GIP_TYPE_REQUEST        0x02

// ──────────────────────────────────────────────
// Dongle wake-up packet (sent once after open)
// ──────────────────────────────────────────────
static const uint8_t kInitPacket[] = { 0x05, 0x20, 0x00, 0x01, 0x00 };

// ──────────────────────────────────────────────
// Controller input payload (18 bytes)
// ──────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t buttons;       // bitmask, see BTN_* below
    uint16_t trigger_l;     // 0-1023
    uint16_t trigger_r;     // 0-1023
    int16_t  stick_lx;      // -32768..32767
    int16_t  stick_ly;
    int16_t  stick_rx;
    int16_t  stick_ry;
    uint8_t  reserved[4];
} gip_input_t;

// Button bitmask positions in gip_input_t.buttons
#define BTN_SYNC        (1 << 0)
#define BTN_MENU        (1 << 2)   // Start / hamburger
#define BTN_VIEW        (1 << 3)   // Back / view
#define BTN_A           (1 << 4)
#define BTN_B           (1 << 5)
#define BTN_X           (1 << 6)
#define BTN_Y           (1 << 7)
#define BTN_DPAD_UP     (1 << 8)
#define BTN_DPAD_DOWN   (1 << 9)
#define BTN_DPAD_LEFT   (1 << 10)
#define BTN_DPAD_RIGHT  (1 << 11)
#define BTN_LB          (1 << 12)
#define BTN_RB          (1 << 13)
#define BTN_LS          (1 << 14)  // left stick click
#define BTN_RS          (1 << 15)  // right stick click

// ──────────────────────────────────────────────
// Rumble payload
// ──────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t flags;        // 0x00
    uint8_t motor_l;      // 0-100
    uint8_t motor_r;      // 0-100
    uint8_t trigger_l;    // 0-100
    uint8_t trigger_r;    // 0-100
    uint8_t duration;     // units of 10ms
    uint8_t delay;
    uint8_t repeat;
} gip_rumble_t;

// ──────────────────────────────────────────────
// HID report descriptor — Generic Desktop / Gamepad
// ──────────────────────────────────────────────
static const uint8_t kHIDDescriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)

      // ── 13 buttons ────────────────────────────
      0x05, 0x09,      //   Usage Page (Button)
      0x19, 0x01,      //   Usage Minimum (1)
      0x29, 0x0D,      //   Usage Maximum (13)
      0x15, 0x00,      //   Logical Minimum (0)
      0x25, 0x01,      //   Logical Maximum (1)
      0x75, 0x01,      //   Report Size (1)
      0x95, 0x0D,      //   Report Count (13)
      0x81, 0x02,      //   Input (Data, Var, Abs)
      // 3-bit padding
      0x75, 0x03,
      0x95, 0x01,
      0x81, 0x03,

      // ── D-Pad hat switch ──────────────────────
      0x05, 0x01,      //   Usage Page (Generic Desktop)
      0x09, 0x39,      //   Usage (Hat switch)
      0x15, 0x00,      //   Logical Min (0 = North)
      0x25, 0x07,      //   Logical Max (7 = NW)
      0x35, 0x00,
      0x46, 0x3B, 0x01,
      0x65, 0x14,      //   Unit (degrees)
      0x75, 0x04,
      0x95, 0x01,
      0x81, 0x42,      //   Input (Null state)
      0x65, 0x00,
      // 4-bit padding
      0x75, 0x04,
      0x95, 0x01,
      0x81, 0x03,

      // ── Left stick X/Y (signed 16-bit) ────────
      0x09, 0x30,      //   Usage (X)
      0x09, 0x31,      //   Usage (Y)
      // ── Right stick X/Y ───────────────────────
      0x09, 0x33,      //   Usage (Rx)
      0x09, 0x34,      //   Usage (Ry)
      0x16, 0x00, 0x80, // Logical Min (-32768)
      0x26, 0xFF, 0x7F, // Logical Max (32767)
      0x75, 0x10,
      0x95, 0x04,
      0x81, 0x02,      //   Input (Data, Var, Abs)

      // ── Triggers (0-1023, unsigned) ───────────
      0x09, 0x32,      //   Usage (Z)
      0x09, 0x35,      //   Usage (Rz)
      0x15, 0x00,
      0x26, 0xFF, 0x03,// Logical Max (1023)
      0x75, 0x10,
      0x95, 0x02,
      0x81, 0x02,

    0xC0               // End Collection
};

// ──────────────────────────────────────────────
// HID input report (matches descriptor above)
// Total: 2 + 1 + 8 + 4 = 15 bytes
// ──────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    // 13 buttons (A B X Y LB RB Menu View LS RS Guide Share Sync) + 3 pad bits = 2 bytes
    uint16_t buttons;
    // Hat switch (4 bits) + 4 pad bits = 1 byte
    uint8_t  hat;
    // Axes: left X/Y, right X/Y (signed 16-bit each)
    int16_t  lx, ly, rx, ry;
    // Triggers (unsigned 16-bit each)
    uint16_t lt, rt;
} xbox_hid_report_t;

// Button positions in xbox_hid_report_t.buttons (HID usage 1-13)
#define HID_BTN_A       (1 << 0)
#define HID_BTN_B       (1 << 1)
#define HID_BTN_X       (1 << 2)
#define HID_BTN_Y       (1 << 3)
#define HID_BTN_LB      (1 << 4)
#define HID_BTN_RB      (1 << 5)
#define HID_BTN_MENU    (1 << 6)
#define HID_BTN_VIEW    (1 << 7)
#define HID_BTN_LS      (1 << 8)
#define HID_BTN_RS      (1 << 9)
#define HID_BTN_GUIDE   (1 << 10)
#define HID_BTN_SHARE   (1 << 11)
#define HID_BTN_SYNC    (1 << 12)

// ──────────────────────────────────────────────
// Translate GIP input → HID report
// ──────────────────────────────────────────────
static inline void gip_to_hid(const gip_input_t *g, xbox_hid_report_t *h, bool guide)
{
    h->buttons = 0;
    if (g->buttons & BTN_A)    h->buttons |= HID_BTN_A;
    if (g->buttons & BTN_B)    h->buttons |= HID_BTN_B;
    if (g->buttons & BTN_X)    h->buttons |= HID_BTN_X;
    if (g->buttons & BTN_Y)    h->buttons |= HID_BTN_Y;
    if (g->buttons & BTN_LB)   h->buttons |= HID_BTN_LB;
    if (g->buttons & BTN_RB)   h->buttons |= HID_BTN_RB;
    if (g->buttons & BTN_MENU) h->buttons |= HID_BTN_MENU;
    if (g->buttons & BTN_VIEW) h->buttons |= HID_BTN_VIEW;
    if (g->buttons & BTN_LS)   h->buttons |= HID_BTN_LS;
    if (g->buttons & BTN_RS)   h->buttons |= HID_BTN_RS;
    if (g->buttons & BTN_SYNC) h->buttons |= HID_BTN_SYNC;
    if (guide)                 h->buttons |= HID_BTN_GUIDE;

    // D-Pad → hat (0=N,1=NE,2=E,3=SE,4=S,5=SW,6=W,7=NW, 8=neutral)
    bool up    = (g->buttons & BTN_DPAD_UP)    != 0;
    bool down  = (g->buttons & BTN_DPAD_DOWN)  != 0;
    bool left  = (g->buttons & BTN_DPAD_LEFT)  != 0;
    bool right = (g->buttons & BTN_DPAD_RIGHT) != 0;

    if      ( up && !down && !left && !right) h->hat = 0;
    else if ( up && !down && !left &&  right) h->hat = 1;
    else if (!up && !down && !left &&  right) h->hat = 2;
    else if (!up &&  down && !left &&  right) h->hat = 3;
    else if (!up &&  down && !left && !right) h->hat = 4;
    else if (!up &&  down &&  left && !right) h->hat = 5;
    else if (!up && !down &&  left && !right) h->hat = 6;
    else if ( up && !down &&  left && !right) h->hat = 7;
    else                                       h->hat = 8;

    h->lx = g->stick_lx;
    h->ly = g->stick_ly;
    h->rx = g->stick_rx;
    h->ry = g->stick_ry;
    h->lt = g->trigger_l;
    h->rt = g->trigger_r;
}

// ──────────────────────────────────────────────
// Build a GIP packet into buf[]. Returns total size.
// ──────────────────────────────────────────────
static inline size_t gip_build(uint8_t *buf, size_t cap,
    uint8_t cmd, uint8_t client, uint8_t seq,
    const uint8_t *payload, uint8_t plen)
{
    if (cap < (size_t)(4 + plen)) return 0;
    buf[0] = cmd;
    buf[1] = client;
    buf[2] = seq;
    buf[3] = plen;
    if (payload && plen) memcpy(buf + 4, payload, plen);
    return 4 + plen;
}
