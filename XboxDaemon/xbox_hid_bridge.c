#include "xbox_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <IOKit/hid/IOHIDKeys.h>
#include <CoreFoundation/CoreFoundation.h>

typedef struct __IOHIDUserDevice *IOHIDUserDeviceRef;
extern IOHIDUserDeviceRef IOHIDUserDeviceCreate(CFAllocatorRef allocator,
                                                CFDictionaryRef properties);
extern IOReturn IOHIDUserDeviceHandleReport(IOHIDUserDeviceRef device,
                                            uint8_t *report,
                                            CFIndex reportLength);

#define MAX_SLOTS 8

typedef struct {
    bool active;
    bool guide_pressed;
    IOHIDUserDeviceRef hid_device;
    xbox_hid_report_t last_report;
} slot_t;

static slot_t g_slots[MAX_SLOTS] = {0};

static const uint8_t kHIDDescriptorFallback[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x65, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34,
    0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02,
    0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
    0xC0
};

static IOHIDUserDeviceRef make_hid_device(uint8_t slot, const uint8_t *desc_bytes, size_t desc_len)
{
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!props) return NULL;

#define SET_INT_KEY(key, val) do { \
    int32_t _v = (val); \
    CFNumberRef _n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &_v); \
    if (_n) { CFDictionarySetValue(props, (key), _n); CFRelease(_n); } \
} while(0)

    SET_INT_KEY(CFSTR("VendorID"), XBOX_VID);
    SET_INT_KEY(CFSTR("ProductID"), XBOX_PID_V2);
    SET_INT_KEY(CFSTR("VersionNumber"), 0x0100);
    SET_INT_KEY(CFSTR("CountryCode"), 0);
    SET_INT_KEY(CFSTR("PrimaryUsagePage"), 0x01);
    SET_INT_KEY(CFSTR("PrimaryUsage"), 0x05);
    SET_INT_KEY(CFSTR("MaxInputReportSize"), (int32_t)sizeof(xbox_hid_report_t));

    CFDictionarySetValue(props, CFSTR("Transport"), CFSTR("USB"));

    char name[64];
    snprintf(name, sizeof(name), "Xbox Wireless Controller (Slot %d)", slot + 1);
    CFStringRef product = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    if (product) {
        CFDictionarySetValue(props, CFSTR("Product"), product);
        CFRelease(product);
    }
    CFDictionarySetValue(props, CFSTR("Manufacturer"), CFSTR("Microsoft Corporation"));

    CFDataRef desc = CFDataCreate(NULL, desc_bytes, (CFIndex)desc_len);
    if (desc) {
        CFDictionarySetValue(props, CFSTR("ReportDescriptor"), desc);
        CFRelease(desc);
    }

    IOHIDUserDeviceRef dev = IOHIDUserDeviceCreate(kCFAllocatorDefault, props);
    CFRelease(props);
    return dev;
}

static IOHIDUserDeviceRef create_virtual_gamepad(uint8_t slot)
{
    IOHIDUserDeviceRef dev = make_hid_device(slot, kHIDDescriptor, sizeof(kHIDDescriptor));
    if (!dev)
        dev = make_hid_device(slot, kHIDDescriptorFallback, sizeof(kHIDDescriptorFallback));
    return dev;
}

static void post_hid_report(slot_t *s)
{
    if (!s || !s->hid_device) return;
    IOReturn r = IOHIDUserDeviceHandleReport(
        s->hid_device,
        (uint8_t *)&s->last_report,
        sizeof(s->last_report));
    if (r != kIOReturnSuccess)
        fprintf(stderr, "[bridge] HandleReport error 0x%08x\n", r);
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;

    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = (int)v;
    return true;
}

static bool json_get_bool(const char *json, const char *key, bool *out)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static bool json_has_type(const char *json, const char *type)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"type\":\"%s\"", type);
    return strstr(json, needle) != NULL;
}

static void handle_connected(int slot)
{
    if (slot < 0 || slot >= MAX_SLOTS) return;
    slot_t *s = &g_slots[slot];
    if (s->active) return;

    s->hid_device = create_virtual_gamepad((uint8_t)slot);
    if (!s->hid_device) {
        fprintf(stderr, "[bridge] failed creating HID gamepad for slot %d\n", slot);
        return;
    }

    s->active = true;
    s->guide_pressed = false;
    memset(&s->last_report, 0, sizeof(s->last_report));
    s->last_report.hat = 8;
    post_hid_report(s);

    printf("[bridge] slot %d connected (virtual HID created)\n", slot);
}

static void handle_disconnected(int slot)
{
    if (slot < 0 || slot >= MAX_SLOTS) return;
    slot_t *s = &g_slots[slot];
    if (!s->active) return;
    if (s->hid_device) {
        CFRelease(s->hid_device);
        s->hid_device = NULL;
    }
    memset(s, 0, sizeof(*s));
    printf("[bridge] slot %d disconnected\n", slot);
}

static void handle_guide(int slot, bool pressed)
{
    if (slot < 0 || slot >= MAX_SLOTS) return;
    slot_t *s = &g_slots[slot];
    if (!s->active) return;
    s->guide_pressed = pressed;
    if (pressed) s->last_report.buttons |= HID_BTN_GUIDE;
    else s->last_report.buttons &= ~HID_BTN_GUIDE;
    post_hid_report(s);
}

static void handle_input(const char *json)
{
    int slot = -1;
    int buttons = 0;
    int lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;

    if (!json_get_int(json, "slot", &slot)) return;
    if (slot < 0 || slot >= MAX_SLOTS) return;
    slot_t *s = &g_slots[slot];
    if (!s->active) return;

    json_get_int(json, "buttons", &buttons);
    json_get_int(json, "lt", &lt);
    json_get_int(json, "rt", &rt);
    json_get_int(json, "lx", &lx);
    json_get_int(json, "ly", &ly);
    json_get_int(json, "rx", &rx);
    json_get_int(json, "ry", &ry);

    gip_input_t g = {0};
    g.buttons = (uint16_t)buttons;
    g.trigger_l = (uint16_t)lt;
    g.trigger_r = (uint16_t)rt;
    g.stick_lx = (int16_t)lx;
    g.stick_ly = (int16_t)ly;
    g.stick_rx = (int16_t)rx;
    g.stick_ry = (int16_t)ry;

    gip_to_hid(&g, &s->last_report, s->guide_pressed);
    post_hid_report(s);
}

static void handle_event_line(const char *line)
{
    if (json_has_type(line, "connected")) {
        int slot = -1;
        if (json_get_int(line, "slot", &slot)) handle_connected(slot);
        return;
    }
    if (json_has_type(line, "disconnected")) {
        int slot = -1;
        if (json_get_int(line, "slot", &slot)) handle_disconnected(slot);
        return;
    }
    if (json_has_type(line, "guide")) {
        int slot = -1;
        bool pressed = false;
        if (json_get_int(line, "slot", &slot) && json_get_bool(line, "pressed", &pressed))
            handle_guide(slot, pressed);
        return;
    }
    if (json_has_type(line, "input")) {
        handle_input(line);
        return;
    }
    if (json_has_type(line, "hid_unavailable")) {
        printf("[bridge] daemon reports HID unavailable (expected in sudo mode)\n");
        return;
    }
}

int main(int argc, char **argv)
{
    const char *bind_addr = "127.0.0.1";
    int bind_port = 7947;

    if (argc >= 2) bind_addr = argv[1];
    if (argc >= 3) bind_port = atoi(argv[2]);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "[bridge] socket failed: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)bind_port);
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "[bridge] invalid bind address: %s\n", bind_addr);
        close(sock);
        return 1;
    }

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[bridge] bind %s:%d failed: %s\n", bind_addr, bind_port, strerror(errno));
        close(sock);
        return 1;
    }

    IOHIDUserDeviceRef probe = create_virtual_gamepad(0);
    if (!probe) {
        fprintf(stderr, "[bridge] HID creation probe failed in this user/runtime\n");
        close(sock);
        return 2;
    }
    CFRelease(probe);

    printf("[bridge] listening on %s:%d\n", bind_addr, bind_port);
    printf("[bridge] waiting for daemon events...\n");

    uint8_t buf[4096];
    for (;;) {
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[bridge] recv failed: %s\n", strerror(errno));
            break;
        }
        buf[n] = '\0';

        char *save = NULL;
        char *line = strtok_r((char *)buf, "\r\n", &save);
        while (line) {
            if (*line) handle_event_line(line);
            line = strtok_r(NULL, "\r\n", &save);
        }
    }

    close(sock);
    return 0;
}
