// XboxGIPProtocol.h
// GIP = Gaming Input Protocol (Xbox One Wireless Controller Protocol)
// Reverse-engineered from xow/xone Linux driver projects
// USB VID: 0x045E (Microsoft)
// USB PID: 0x02E6 (original dongle), 0x02FD (new dongle)

#pragma once
#include <stdint.h>

// ──────────────────────────────────────────────
// USB IDs
// ──────────────────────────────────────────────
#define XBOX_VENDOR_ID          0x045E
#define XBOX_DONGLE_PID_V1      0x02E6  // Original Xbox One Wireless Adapter
#define XBOX_DONGLE_PID_V2      0x02FD  // New Xbox One Wireless Adapter (smaller)

// ──────────────────────────────────────────────
// GIP Frame Header (4 bytes)
// ──────────────────────────────────────────────
struct __attribute__((packed)) GIPHeader {
    uint8_t command;    // Command type
    uint8_t client;     // Client ID (0 = host, 1-8 = controller slot)
    uint8_t sequence;   // Sequence number
    uint8_t length;     // Payload length
};

// ──────────────────────────────────────────────
// GIP Commands (host → dongle)
// ──────────────────────────────────────────────
#define GIP_CMD_ACKNOWLEDGE         0x01
#define GIP_CMD_ANNOUNCE            0x02
#define GIP_CMD_STATUS              0x03
#define GIP_CMD_IDENTIFY            0x04
#define GIP_CMD_POWER               0x05
#define GIP_CMD_AUTHENTICATE        0x06
#define GIP_CMD_GUIDE_BTN           0x07
#define GIP_CMD_AUDIO_CONTROL       0x08
#define GIP_CMD_LED_MODE            0x0A
#define GIP_CMD_RUMBLE              0x09
#define GIP_CMD_SERIAL              0x1E
#define GIP_CMD_INPUT               0x20  // Controller input report

// ──────────────────────────────────────────────
// Dongle-specific commands
// ──────────────────────────────────────────────
#define DONGLE_CMD_INIT             0x05  // Initialize dongle
#define DONGLE_CMD_PAIRING_START    0x06  // Start pairing mode
#define DONGLE_CMD_CONTROLLER_ADDED 0x0E  // Controller connected notification
#define DONGLE_CMD_CONTROLLER_LOST  0x0F  // Controller disconnected notification

// ──────────────────────────────────────────────
// Dongle initialization sequence
// ──────────────────────────────────────────────
// Sent once after USB enumeration to wake up the dongle
static const uint8_t kDongleInitPacket[] = {
    0x05, 0x20, 0x00, 0x01, 0x00
};

// ──────────────────────────────────────────────
// LED mode packet (solid green = connected)
// ──────────────────────────────────────────────
static const uint8_t kLEDModePacket[] = {
    0x0A, 0x20, 0x00, 0x03,
    0x00,   // LED mode: 0x00 = off, 0x01 = blink, 0x03 = solid
    0x00,
    0x4F    // brightness
};

// ──────────────────────────────────────────────
// Rumble command payload
// ──────────────────────────────────────────────
struct __attribute__((packed)) GIPRumblePayload {
    uint8_t flags;          // 0x00 = apply
    uint8_t magnitude_l;    // Left motor (0-100)
    uint8_t magnitude_r;    // Right motor (0-100)
    uint8_t magnitude_lt;   // Left trigger motor (0-100)
    uint8_t magnitude_rt;   // Right trigger motor (0-100)
    uint8_t duration;       // Duration in 10ms units
    uint8_t delay;          // Delay before start
    uint8_t repeat;         // Repeat count
};

// ──────────────────────────────────────────────
// Controller input report (GIP_CMD_INPUT = 0x20)
// Total payload: 18 bytes
// ──────────────────────────────────────────────
struct __attribute__((packed)) GIPInputReport {
    // Byte 0: digital buttons (low)
    uint8_t btn_sync    : 1;
    uint8_t btn_menu    : 1;  // "hamburger" / Start
    uint8_t btn_view    : 1;  // "back" / Select
    uint8_t btn_a       : 1;
    uint8_t btn_b       : 1;
    uint8_t btn_x       : 1;
    uint8_t btn_y       : 1;
    uint8_t btn_dpad_up : 1;

    // Byte 1: digital buttons (high)
    uint8_t btn_dpad_down   : 1;
    uint8_t btn_dpad_left   : 1;
    uint8_t btn_dpad_right  : 1;
    uint8_t btn_lb          : 1;
    uint8_t btn_rb          : 1;
    uint8_t btn_ls          : 1;  // Left stick click
    uint8_t btn_rs          : 1;  // Right stick click
    uint8_t reserved        : 1;

    // Bytes 2-3: analog triggers (0-1023)
    uint16_t trigger_left;
    uint16_t trigger_right;

    // Bytes 6-9: left stick (signed, -32768..32767)
    int16_t stick_left_x;
    int16_t stick_left_y;

    // Bytes 10-13: right stick
    int16_t stick_right_x;
    int16_t stick_right_y;

    // Bytes 14-17: reserved / extension
    uint8_t reserved2[4];
};

// ──────────────────────────────────────────────
// HID Descriptor for a standard gamepad
// Maps Xbox inputs → USB HID Gamepad
// ──────────────────────────────────────────────
static const uint8_t kXboxHIDReportDescriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)

    // Buttons: 11 digital buttons
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (Button 1 = A)
    0x29, 0x0B,        //   Usage Maximum (Button 11 = RS)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x0B,        //   Report Count (11)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    // Padding to byte boundary
    0x75, 0x05,        //   Report Size (5)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x03,        //   Input (Constant)

    // D-Pad as Hat Switch
    0x05, 0x01,        //   Usage Page (Generic Desktop)
    0x09, 0x39,        //   Usage (Hat switch)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x07,        //   Logical Maximum (7)
    0x35, 0x00,        //   Physical Minimum (0)
    0x46, 0x3B, 0x01,  //   Physical Maximum (315)
    0x65, 0x14,        //   Unit (Eng Rot: Degree)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x42,        //   Input (Data, Variable, Absolute, Null state)
    0x65, 0x00,        //   Unit (None)
    0x75, 0x04,        //   Report Size (4) - padding
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x03,        //   Input (Constant)

    // Left Stick X/Y (signed 16-bit)
    0x09, 0x30,        //   Usage (X)
    0x09, 0x31,        //   Usage (Y)
    // Right Stick X/Y
    0x09, 0x33,        //   Usage (Rx)
    0x09, 0x34,        //   Usage (Ry)
    0x16, 0x00, 0x80,  //   Logical Minimum (-32768)
    0x26, 0xFF, 0x7F,  //   Logical Maximum (32767)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x04,        //   Report Count (4)
    0x81, 0x06,        //   Input (Data, Variable, Relative)

    // Triggers (10-bit analog, 0-1023)
    0x09, 0x32,        //   Usage (Z - Left Trigger)
    0x09, 0x35,        //   Usage (Rz - Right Trigger)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)

    0xC0               // End Collection
};

// ──────────────────────────────────────────────
// HID Report structure (matches descriptor above)
// ──────────────────────────────────────────────
struct __attribute__((packed)) XboxHIDReport {
    // 11 buttons + 5 padding bits = 2 bytes
    uint8_t  btn_a        : 1;
    uint8_t  btn_b        : 1;
    uint8_t  btn_x        : 1;
    uint8_t  btn_y        : 1;
    uint8_t  btn_lb       : 1;
    uint8_t  btn_rb       : 1;
    uint8_t  btn_menu     : 1;
    uint8_t  btn_view     : 1;
    uint8_t  btn_ls       : 1;
    uint8_t  btn_rs       : 1;
    uint8_t  btn_guide    : 1;
    uint8_t  _pad         : 5;

    // Hat switch (0-7, 8=neutral)
    uint8_t  hat          : 4;
    uint8_t  _pad2        : 4;

    // Axes
    int16_t  left_x;
    int16_t  left_y;
    int16_t  right_x;
    int16_t  right_y;
    uint16_t trigger_left;
    uint16_t trigger_right;
};

// ──────────────────────────────────────────────
// Utility: build a GIP packet into a buffer
// Returns total packet size (header + payload)
// ──────────────────────────────────────────────
static inline size_t GIPBuildPacket(
    uint8_t *buf, size_t bufSize,
    uint8_t cmd, uint8_t client, uint8_t seq,
    const uint8_t *payload, uint8_t payloadLen)
{
    if (bufSize < (size_t)(4 + payloadLen)) return 0;
    buf[0] = cmd;
    buf[1] = client;
    buf[2] = seq;
    buf[3] = payloadLen;
    if (payload && payloadLen) {
        for (uint8_t i = 0; i < payloadLen; i++)
            buf[4 + i] = payload[i];
    }
    return 4 + payloadLen;
}

// ──────────────────────────────────────────────
// Utility: translate GIPInputReport → XboxHIDReport
// ──────────────────────────────────────────────
static inline void GIPToHID(const GIPInputReport *gip, XboxHIDReport *hid)
{
    hid->btn_a    = gip->btn_a;
    hid->btn_b    = gip->btn_b;
    hid->btn_x    = gip->btn_x;
    hid->btn_y    = gip->btn_y;
    hid->btn_lb   = gip->btn_lb;
    hid->btn_rb   = gip->btn_rb;
    hid->btn_menu = gip->btn_menu;
    hid->btn_view = gip->btn_view;
    hid->btn_ls   = gip->btn_ls;
    hid->btn_rs   = gip->btn_rs;
    hid->btn_guide = 0; // guide button comes in a separate command

    // D-Pad → hat switch
    // hat: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8=neutral
    const bool up    = gip->btn_dpad_up;
    const bool down  = gip->btn_dpad_down;
    const bool left  = gip->btn_dpad_left;
    const bool right = gip->btn_dpad_right;

    if (up    && !down && !left && !right) hid->hat = 0;
    else if (up    && !down && !left &&  right) hid->hat = 1;
    else if (!up   && !down && !left &&  right) hid->hat = 2;
    else if (!up   &&  down && !left &&  right) hid->hat = 3;
    else if (!up   &&  down && !left && !right) hid->hat = 4;
    else if (!up   &&  down &&  left && !right) hid->hat = 5;
    else if (!up   && !down &&  left && !right) hid->hat = 6;
    else if (up    && !down &&  left && !right) hid->hat = 7;
    else                                         hid->hat = 8; // neutral

    hid->left_x        = gip->stick_left_x;
    hid->left_y        = gip->stick_left_y;
    hid->right_x       = gip->stick_right_x;
    hid->right_y       = gip->stick_right_y;
    hid->trigger_left  = gip->trigger_left;
    hid->trigger_right = gip->trigger_right;
}
