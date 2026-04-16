// xbox_daemon.c — Xbox One Wireless Adapter userspace daemon
// libusb + IOHIDUserDevice (no entitlement, no SIP needed)

#include "xbox_protocol.h"
#include "mt76_firmware.h"
#include "mt76_init.h"

#define FW_PATH "mt7612us.bin"   // produced by extract_firmware.sh

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include <libusb.h>

#include <IOKit/hid/IOHIDKeys.h>
#include <CoreFoundation/CoreFoundation.h>

// IOHIDUserDevice — semi-private IOKit API, forward-declared
typedef struct __IOHIDUserDevice *IOHIDUserDeviceRef;
extern IOHIDUserDeviceRef IOHIDUserDeviceCreate(CFAllocatorRef  allocator,
                                                CFDictionaryRef properties);
extern IOReturn IOHIDUserDeviceHandleReport(IOHIDUserDeviceRef device,
                                            uint8_t           *report,
                                            CFIndex            reportLength);

// ──────────────────────────────────────────────
// Globals
// ──────────────────────────────────────────────
#define MAX_SLOTS 8

typedef struct {
    bool               active;
    uint8_t            client_id;
    bool               guide_pressed;
    IOHIDUserDeviceRef hid_device;
    xbox_hid_report_t  last_report;
} slot_t;

static libusb_context       *g_ctx     = NULL;
static libusb_device_handle *g_dev     = NULL;
static uint8_t               g_ep_in   = 0;
static uint8_t               g_ep_out  = 0;
static uint8_t               g_host_seq = 0;
static slot_t                g_slots[MAX_SLOTS] = {};
static uint8_t               g_ep_in2  = 0;   // second IN endpoint (0x85)
static volatile bool         g_running = true;
static uint8_t               g_our_mac[6] = {0x62,0x45,0xbd,0x01,0x02,0x03}; // set after EFUSE read
static uint8_t               g_pairing_channel = 0x2c;
static uint8_t               g_forced_pairing_channel = 0;
static uint8_t               g_next_wcid = 1; // 1-based; 0 is reserved/broadcast
static uint64_t              g_channel_lock_until_ms = 0;
static bool                  g_usb_reset_on_start = true;

typedef struct {
    bool    active;
    uint8_t mac[6];
    uint8_t wcid;
    bool    handshake_done;
    uint8_t slot;
    bool    fw_client_added;
    bool    session_initialized;
    bool    initial_commands_sent;
} peer_t;

static peer_t                g_peers[128] = {};
static bool                  g_verbose_air = false;
static bool                  g_wlan_ack_enabled = false;
static bool                  g_usb_gip_ack_enabled = false;
static bool                  g_handle_client_lost = false;
static bool                  g_hid_supported = true;
static bool                  g_hid_probe_done = false;
static bool                  g_event_udp_enabled = false;
static int                   g_event_udp_sock = -1;
static struct sockaddr_in    g_event_udp_addr = {};
static bool                  g_pairing_beacon_enabled = true;
static bool                  g_multi_pair_mode = false;
static uint32_t              g_awdl_ignored = 0;
static uint64_t              g_awdl_last_log_ms = 0;

static void handle_signal(int s) { (void)s; g_running = false; }

// ──────────────────────────────────────────────
// Hex dump helper (for raw USB debugging)
// ──────────────────────────────────────────────
static void hexdump(const char *label, const uint8_t *buf, int len)
{
    printf("[usb] %s (%d bytes): ", label, len);
    for (int i = 0; i < len && i < 32; i++) printf("%02x ", buf[i]);
    if (len > 32) printf("...");
    printf("\n");
}

static void event_udp_send(const char *fmt, ...)
{
    if (!g_event_udp_enabled || g_event_udp_sock < 0) return;

    char msg[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(msg) - 2) n = (int)sizeof(msg) - 2;

    // Make UDP stream human-friendly for netcat listeners.
    msg[n++] = '\n';
    msg[n] = '\0';

    ssize_t sent = sendto(g_event_udp_sock, msg, (size_t)n, 0,
                          (const struct sockaddr *)&g_event_udp_addr,
                          sizeof(g_event_udp_addr));
    if (sent < 0) {
        fprintf(stderr, "[event] UDP send failed: %s\n", strerror(errno));
    }
}

static void init_event_udp_from_env(void)
{
    const char *cfg = getenv("XBOX_EVENT_UDP");
    if (!cfg || !cfg[0]) {
        g_event_udp_enabled = false;
        return;
    }

    char ip[64] = {0};
    int port = 0;

    if (strchr(cfg, ':')) {
        if (sscanf(cfg, "%63[^:]:%d", ip, &port) != 2) {
            fprintf(stderr, "[event] Invalid XBOX_EVENT_UDP='%s' (expected ip:port)\n", cfg);
            return;
        }
    } else {
        if (sscanf(cfg, "%d", &port) != 1) {
            fprintf(stderr, "[event] Invalid XBOX_EVENT_UDP='%s' (expected port or ip:port)\n", cfg);
            return;
        }
        strncpy(ip, "127.0.0.1", sizeof(ip) - 1);
    }

    if (port < 1 || port > 65535) {
        fprintf(stderr, "[event] Invalid UDP port %d\n", port);
        return;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "[event] socket() failed: %s\n", strerror(errno));
        return;
    }

    memset(&g_event_udp_addr, 0, sizeof(g_event_udp_addr));
    g_event_udp_addr.sin_family = AF_INET;
    g_event_udp_addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip, &g_event_udp_addr.sin_addr) != 1) {
        fprintf(stderr, "[event] Invalid IPv4 address '%s'\n", ip);
        close(sock);
        return;
    }

    g_event_udp_sock = sock;
    g_event_udp_enabled = true;
    printf("[event] UDP event stream enabled -> %s:%d\n", ip, port);
}

static bool mac_equal(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static bool mac_is_awdl_bssid(const uint8_t *mac)
{
    return mac[0] == 0x00 && mac[1] == 0x25 && mac[2] == 0x00;
}

static bool mac_has_xbox_prefix(const uint8_t *mac)
{
    return mac[0] == 0x62 && mac[1] == 0x45 && mac[2] == 0xbd;
}

static uint64_t now_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void log_awdl_ignored(void)
{
    g_awdl_ignored++;
    uint64_t now = now_monotonic_ms();
    if (g_awdl_last_log_ms == 0 || now - g_awdl_last_log_ms >= 5000) {
        printf("[awdl] Ignored %u neighbor action frames\n", g_awdl_ignored);
        g_awdl_last_log_ms = now;
        g_awdl_ignored = 0;
    }
}

static void lock_pairing_channel(const char *reason, const uint8_t *src)
{
    g_channel_lock_until_ms = now_monotonic_ms() + 5000;
    printf("[mt76]  Locking channel %u for 5s (%s from %02x:%02x:%02x:%02x:%02x:%02x)\n",
           g_pairing_channel, reason,
           src[0], src[1], src[2], src[3], src[4], src[5]);
}

static void set_pairing_beacon(bool enabled, const char *reason)
{
    if (!g_dev) return;
    if (g_pairing_beacon_enabled == enabled) return;
    mt76_write_beacon(g_dev, g_our_mac, enabled ? 1 : 0, g_pairing_channel);
    g_pairing_beacon_enabled = enabled;
    printf("[mt76]  Pairing beacon %s (%s)\n",
           enabled ? "enabled" : "disabled",
           reason ? reason : "state change");
}

static int active_slot_count(void)
{
    int active = 0;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (g_slots[i].active) active++;
    }
    return active;
}

static peer_t *find_peer_by_mac(const uint8_t *mac)
{
    for (size_t i = 1; i < sizeof(g_peers) / sizeof(g_peers[0]); i++) {
        if (g_peers[i].active && mac_equal(g_peers[i].mac, mac))
            return &g_peers[i];
    }
    return NULL;
}

static peer_t *remember_peer(const uint8_t *mac)
{
    peer_t *peer = find_peer_by_mac(mac);
    if (peer) return peer;

    uint8_t wcid = g_next_wcid;
    if (wcid >= sizeof(g_peers) / sizeof(g_peers[0]))
        return NULL;

    peer = &g_peers[wcid];
    peer->active = true;
    peer->wcid = wcid;
    peer->handshake_done = false;
    peer->slot = UINT8_MAX;
    peer->fw_client_added = false;
    peer->session_initialized = false;
    peer->initial_commands_sent = false;
    memcpy(peer->mac, mac, sizeof(peer->mac));

    if (g_next_wcid < sizeof(g_peers) / sizeof(g_peers[0]) - 1)
        g_next_wcid++;

    mt76_add_wcid(g_dev, wcid, mac);
    printf("[xbox] Peer %02x:%02x:%02x:%02x:%02x:%02x assigned WCID %u\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], wcid);
    return peer;
}

// ══════════════════════════════════════════════
// Virtual HID gamepad
// ══════════════════════════════════════════════

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
    if (!g_hid_supported) return NULL;

    IOHIDUserDeviceRef dev = make_hid_device(slot, kHIDDescriptor, sizeof(kHIDDescriptor));
    if (!dev) {
        fprintf(stderr, "[hid]  Primary descriptor failed, trying fallback descriptor\n");
        dev = make_hid_device(slot, kHIDDescriptorFallback, sizeof(kHIDDescriptorFallback));
    }

#undef SET_INT_KEY

    if (dev)
        printf("[hid]  Virtual gamepad created for slot %d\n", slot);
    else
        fprintf(stderr, "[hid]  ERROR: IOHIDUserDeviceCreate failed for slot %d\n", slot);

    return dev;
}

static void probe_hid_support(void)
{
    if (g_hid_probe_done) return;
    g_hid_probe_done = true;

    static const uint8_t kProbeDesc[] = {
        0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
        0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
        0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
        0x95, 0x01, 0x75, 0x08, 0x81, 0x03,
        0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
        0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
        0xC0
    };

    IOHIDUserDeviceRef probe = make_hid_device(0, kProbeDesc, sizeof(kProbeDesc));
    if (!probe) {
        g_hid_supported = false;
        fprintf(stderr, "[hid]  Probe failed: virtual HID device creation unavailable in this runtime\n");
        fprintf(stderr, "[hid]  Hint: this macOS may require HID virtual-device entitlement\n");
        event_udp_send("{\"type\":\"hid_unavailable\"}");
        return;
    }

    CFRelease(probe);
    printf("[hid]  Probe OK: virtual HID device creation available\n");
}

static void post_hid_report(slot_t *s)
{
    if (!s->hid_device) return;
    IOReturn r = IOHIDUserDeviceHandleReport(
        s->hid_device,
        (uint8_t *)&s->last_report,
        sizeof(s->last_report));
    if (r != kIOReturnSuccess)
        fprintf(stderr, "[hid]  HandleReport error 0x%08x\n", r);
}

// ══════════════════════════════════════════════
// USB send
// ══════════════════════════════════════════════

static int usb_send(const uint8_t *buf, int len)
{
    if (!g_dev || !g_ep_out) return -1;
    int tx = 0;
    int r = libusb_bulk_transfer(g_dev, g_ep_out,
                                 (unsigned char *)buf, len, &tx, 2000);
    if (r != LIBUSB_SUCCESS) {
        fprintf(stderr, "[usb]  send error: %s\n", libusb_strerror(r));
        return -1;
    }
    hexdump("OUT", buf, tx);
    return tx;
}

static void send_ack(uint8_t client, uint8_t seq __attribute__((unused)))
{
    uint8_t payload[] = { 0x00 };
    uint8_t pkt[8];
    size_t n = gip_build(pkt, sizeof(pkt),
        GIP_CMD_ACKNOWLEDGE, client, g_host_seq++, payload, 1);
    usb_send(pkt, (int)n);
}

// Probe a single IN endpoint with a short timeout.
// Returns true if any data arrived (prints hex dump).
static bool probe_in_ep(uint8_t ep, int timeout_ms) __attribute__((unused));
static bool probe_in_ep(uint8_t ep, int timeout_ms)
{
    uint8_t buf[USB_BUF_SIZE];
    int n = 0;
    int r = libusb_bulk_transfer(g_dev, ep, buf, sizeof(buf), &n, timeout_ms);
    if (r == LIBUSB_SUCCESS && n > 0) {
        printf("[probe] EP 0x%02x RESPONDED! ", ep);
        hexdump("data", buf, n);
        return true;
    }
    printf("[probe] EP 0x%02x: %s\n", ep,
           r == LIBUSB_ERROR_TIMEOUT ? "timeout (silent)" : libusb_strerror(r));
    return false;
}

// (send_dongle_init inline in main init block)

// ══════════════════════════════════════════════
// GIP frame handling
// ══════════════════════════════════════════════

static void on_connect(uint8_t client)
{
    if (client >= MAX_SLOTS || g_slots[client].active) return;
    slot_t *s = &g_slots[client];
    s->active    = true;
    s->client_id = client;
    s->last_report.hat = 8;
    s->hid_device = create_virtual_gamepad(client);
    printf("[gip]  Controller CONNECTED → slot %d\n", client);
    event_udp_send("{\"type\":\"connected\",\"slot\":%u}", client);
}

static void on_disconnect(uint8_t client)
{
    if (client >= MAX_SLOTS || !g_slots[client].active) return;
    slot_t *s = &g_slots[client];
    if (s->hid_device) { CFRelease(s->hid_device); s->hid_device = NULL; }
    memset(s, 0, sizeof(*s));
    printf("[gip]  Controller DISCONNECTED from slot %d\n", client);
    event_udp_send("{\"type\":\"disconnected\",\"slot\":%u}", client);
}

static void on_input(uint8_t client, const uint8_t *payload, uint8_t plen)
{
    if (client >= MAX_SLOTS || !g_slots[client].active) return;

    // Wireless cmd=0x20 frames often carry an 8-byte wrapper before the
    // actual 16-byte controller state:
    // [00 00 00 00][20 00 <seq> 2c][buttons/triggers/sticks...]
    const uint8_t *in = payload;
    uint8_t in_len = plen;
    if (plen >= 24 && payload[4] == GIP_CMD_INPUT && payload[7] >= 14) {
        in = payload + 8;
        in_len = (uint8_t)(plen - 8);
    }

    // xone-style input payload can be either:
    // A) [buttons:2][lt:2][rt:2][lx:2][ly:2][rx:2][ry:2]  (14 bytes)
    // B) [buttons:4][lt:2][rt:2][lx:2][ly:2][rx:2][ry:2]  (16 bytes)
    // Some controllers append additional bytes.
    if (in_len < 14) return;

    gip_input_t parsed14 = {0};
    gip_input_t parsed16 = {0};
    bool ok14 = false;
    bool ok16 = false;

    // Layout A: 2-byte button field (xone gamepad packet)
    {
        memcpy(&parsed14.buttons,   in + 0, 2);
        memcpy(&parsed14.trigger_l, in + 2, 2);
        memcpy(&parsed14.trigger_r, in + 4, 2);
        memcpy(&parsed14.stick_lx,  in + 6, 2);
        memcpy(&parsed14.stick_ly,  in + 8, 2);
        memcpy(&parsed14.stick_rx,  in + 10, 2);
        memcpy(&parsed14.stick_ry,  in + 12, 2);
        ok14 = true;
    }

    // Layout B: 4-byte button field (xow packet)
    if (in_len >= 16) {
        uint32_t buttons32 = 0;
        memcpy(&buttons32, in + 0, 4);

        if (buttons32 & (1u << 0))  parsed16.buttons |= BTN_SYNC;
        if (buttons32 & (1u << 2))  parsed16.buttons |= BTN_MENU;
        if (buttons32 & (1u << 3))  parsed16.buttons |= BTN_VIEW;
        if (buttons32 & (1u << 4))  parsed16.buttons |= BTN_A;
        if (buttons32 & (1u << 5))  parsed16.buttons |= BTN_B;
        if (buttons32 & (1u << 6))  parsed16.buttons |= BTN_X;
        if (buttons32 & (1u << 7))  parsed16.buttons |= BTN_Y;
        if (buttons32 & (1u << 8))  parsed16.buttons |= BTN_DPAD_UP;
        if (buttons32 & (1u << 9))  parsed16.buttons |= BTN_DPAD_DOWN;
        if (buttons32 & (1u << 10)) parsed16.buttons |= BTN_DPAD_LEFT;
        if (buttons32 & (1u << 11)) parsed16.buttons |= BTN_DPAD_RIGHT;
        if (buttons32 & (1u << 12)) parsed16.buttons |= BTN_LB;
        if (buttons32 & (1u << 13)) parsed16.buttons |= BTN_RB;
        if (buttons32 & (1u << 14)) parsed16.buttons |= BTN_LS;
        if (buttons32 & (1u << 15)) parsed16.buttons |= BTN_RS;

        memcpy(&parsed16.trigger_l, in + 4, 2);
        memcpy(&parsed16.trigger_r, in + 6, 2);
        // xow/xone documented order:
        // [buttons][lt][rt][lx][ly][rx][ry]
        memcpy(&parsed16.stick_lx,  in + 8, 2);
        memcpy(&parsed16.stick_ly,  in + 10, 2);
        memcpy(&parsed16.stick_rx,  in + 12, 2);
        memcpy(&parsed16.stick_ry,  in + 14, 2);
        ok16 = true;
    }

    gip_input_t parsed = {0};
    if (ok14 && ok16) {
        // Prefer the layout with realistic trigger ranges.
        bool trig14_ok = parsed14.trigger_l <= 4095 && parsed14.trigger_r <= 4095;
        bool trig16_ok = parsed16.trigger_l <= 4095 && parsed16.trigger_r <= 4095;

        if (trig14_ok && !trig16_ok) {
            parsed = parsed14;
        } else if (!trig14_ok && trig16_ok) {
            parsed = parsed16;
        } else {
            // Tie-breaker: 14-byte layout is what xone uses for gamepad input.
            parsed = parsed14;
        }
    } else if (ok16) {
        parsed = parsed16;
    } else if (ok14) {
        parsed = parsed14;
    } else {
        return;
    }

    slot_t *s = &g_slots[client];
    const gip_input_t *g = &parsed;
    gip_to_hid(g, &s->last_report, s->guide_pressed);
    post_hid_report(s);
    event_udp_send(
        "{\"type\":\"input\",\"slot\":%u,\"buttons\":%u,\"lt\":%u,\"rt\":%u,\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d}",
        client,
        (unsigned)g->buttons,
        (unsigned)g->trigger_l,
        (unsigned)g->trigger_r,
        (int)g->stick_lx,
        (int)g->stick_ly,
        (int)g->stick_rx,
        (int)g->stick_ry);
}

static void on_guide(uint8_t client, bool pressed)
{
    if (client >= MAX_SLOTS) return;
    g_slots[client].guide_pressed = pressed;
    if (pressed)  g_slots[client].last_report.buttons |=  HID_BTN_GUIDE;
    else          g_slots[client].last_report.buttons &= ~HID_BTN_GUIDE;
    post_hid_report(&g_slots[client]);
    event_udp_send("{\"type\":\"guide\",\"slot\":%u,\"pressed\":%s}",
                   client, pressed ? "true" : "false");
}

static void process_frame(const uint8_t *buf, int len) __attribute__((unused));
static void process_frame(const uint8_t *buf, int len)
{
    if (len < 4) return;
    const gip_header_t *h = (const gip_header_t *)buf;
    const uint8_t *payload = buf + 4;
    uint8_t plen = (uint8_t)((len - 4 < h->length) ? (len - 4) : h->length);

    printf("[gip]  cmd=0x%02x client=%d seq=%d len=%d\n",
           h->command, h->client, h->sequence, h->length);

    switch (h->command) {
        case GIP_CMD_CTRL_ADDED:   on_connect(h->client); break;
        case GIP_CMD_CTRL_LOST:    on_disconnect(h->client); break;
        case GIP_CMD_ANNOUNCE:
            send_ack(h->client, h->sequence);
            if (!g_slots[h->client].active) on_connect(h->client);
            break;
        case GIP_CMD_STATUS:
            send_ack(h->client, h->sequence);
            break;
        case GIP_CMD_INPUT:
            on_input(h->client, payload, plen);
            break;
        case GIP_CMD_GUIDE_BTN:
            if (plen >= 1) on_guide(h->client, payload[0] != 0);
            send_ack(h->client, h->sequence);
            break;
        default:
            printf("[gip]  unknown cmd=0x%02x (ignored)\n", h->command);
            break;
    }
}

// ══════════════════════════════════════════════
// USB device setup
// ══════════════════════════════════════════════

// List all connected USB devices (for debugging)
static void list_usb_devices(void)
{
    libusb_device **list;
    ssize_t count = libusb_get_device_list(g_ctx, &list);
    printf("[usb]  Scanning %zd USB devices:\n", count);
    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS) continue;
        if (desc.idVendor == XBOX_VID)
            printf("[usb]  *** Microsoft VID=0x%04x PID=0x%04x (bus %d dev %d) ***\n",
                   desc.idVendor, desc.idProduct,
                   libusb_get_bus_number(list[i]),
                   libusb_get_device_address(list[i]));
    }
    libusb_free_device_list(list, 1);
}

static libusb_device_handle *open_dongle(void)
{
    static const uint16_t pids[] = { XBOX_PID_V1, XBOX_PID_V2, XBOX_PID_V3 };
    for (size_t i = 0; i < sizeof(pids)/sizeof(pids[0]); i++) {
        libusb_device_handle *h =
            libusb_open_device_with_vid_pid(g_ctx, XBOX_VID, pids[i]);
        if (h) {
            printf("[usb]  Opened dongle VID=0x%04x PID=0x%04x\n", XBOX_VID, pids[i]);
            return h;
        }
    }
    return NULL;
}

// Walk all interfaces/endpoints.
// For the Series adapter (0x02FE) there are 6 OUT endpoints:
//   0x04 = dongle control (init, global cmds)  ← we want this one
//   0x05-0x09 = per-slot (rumble etc.)
// Strategy: EP 0x04 is the MCU/dongle control endpoint for GIP commands.
static bool find_endpoints(libusb_device *dev)
{
    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(dev, &cfg) != LIBUSB_SUCCESS) {
        fprintf(stderr, "[usb]  get_active_config failed\n");
        return false;
    }

    printf("[usb]  Config has %d interface(s)\n", cfg->bNumInterfaces);
    g_ep_in = g_ep_in2 = g_ep_out = 0;

    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        for (int a = 0; a < cfg->interface[i].num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt =
                &cfg->interface[i].altsetting[a];

            printf("[usb]  Interface %d alt %d class=0x%02x endpoints=%d\n",
                   i, a, alt->bInterfaceClass, alt->bNumEndpoints);

            for (int e = 0; e < alt->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                uint8_t type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                uint8_t addr = ep->bEndpointAddress;
                uint8_t dir  = addr & LIBUSB_ENDPOINT_DIR_MASK;

                printf("[usb]    EP 0x%02x  type=%s  dir=%s\n",
                    addr,
                    type == LIBUSB_TRANSFER_TYPE_BULK ? "BULK" :
                    type == LIBUSB_TRANSFER_TYPE_INTERRUPT ? "INT" : "other",
                    dir == LIBUSB_ENDPOINT_IN ? "IN" : "OUT");

                if (type == LIBUSB_TRANSFER_TYPE_BULK) {
                    if (dir == LIBUSB_ENDPOINT_IN) {
                        if (!g_ep_in)       g_ep_in  = addr;  // first IN (0x84)
                        else if (!g_ep_in2) g_ep_in2 = addr;  // second IN (0x85)
                    } else {
                        // For Series X/S adapter: Use EP 0x08 for GIP commands
                        // EP 0x04 is reserved for MCU/firmware commands
                        if (addr == 0x08) g_ep_out = addr;
                    }
                }
            }
        }
    }

    libusb_free_config_descriptor(cfg);
    printf("[usb]  Selected EP_IN=0x%02x  EP_IN2=0x%02x  EP_OUT=0x%02x\n",
           g_ep_in, g_ep_in2, g_ep_out);
    return g_ep_in != 0 && g_ep_out != 0;
}

// ══════════════════════════════════════════════
// Main read loop
// ══════════════════════════════════════════════

// ── Reader thread per IN endpoint ───────────────────────────────────────────
// Starts BEFORE the init send — so no responses are lost.

typedef struct { uint8_t ep; } reader_arg_t;

// ══════════════════════════════════════════════
// Xbox vendor action frame handling (post-association)
// ══════════════════════════════════════════════

// Microsoft OUI for Xbox wireless: 00:17:F2
#define XBOX_ACTION_OUI0 0x00
#define XBOX_ACTION_OUI1 0x17
#define XBOX_ACTION_OUI2 0xf2
#define XBOX_ACTION_TYPE 0x08   // Xbox wireless action type

// Build and send an Xbox vendor action frame response.
// dst: controller MAC; our_mac: our BSSID/src
static void send_xbox_action_response(const uint8_t *dst,
                                      const uint8_t *our_mac,
                                      uint8_t wcid)
{
    // 802.11 Management frame: Action (fc=0x00d0)
    // Header: FC(2)+Dur(2)+DA(6)+SA(6)+BSSID(6)+SeqCtrl(2) = 24B
    // Body: Category(1)+OUI(3)+ActionType(1)+subtype(1)+wcid(1)+...
    uint8_t frame[24 + 32];
    memset(frame, 0, sizeof(frame));

    // Frame Control = Action management frame (0x00d0)
    frame[0] = 0xd0; frame[1] = 0x00;
    // Duration = 0
    // DA = controller
    memcpy(frame + 4, dst, 6);
    // SA = our MAC
    memcpy(frame + 10, our_mac, 6);
    // BSSID = our MAC
    memcpy(frame + 16, our_mac, 6);
    // SeqCtrl = 0

    // Action body — Xbox vendor-specific association ack
    int body = 24;
    frame[body++] = 0x7f;              // Category: Vendor Specific
    frame[body++] = XBOX_ACTION_OUI0;
    frame[body++] = XBOX_ACTION_OUI1;
    frame[body++] = XBOX_ACTION_OUI2;
    frame[body++] = XBOX_ACTION_TYPE;
    frame[body++] = 0x11;              // subtype: association ack
    frame[body++] = wcid;              // assigned WCID
    frame[body++] = 0x00;
    // Pad to 8 bytes body
    while (body < 24 + 8) frame[body++] = 0x00;

    mt76_wlan_tx(g_dev, frame, body, wcid);
}

static void send_probe_response(const uint8_t *dst,
                                const uint8_t *our_mac,
                                uint8_t channel)
{
    uint8_t frame[24 + 14 + 3 + 18];
    memset(frame, 0, sizeof(frame));

    frame[0] = 0x50; frame[1] = 0x00;       // Probe Response
    memcpy(frame + 4, dst, 6);              // DA
    memcpy(frame + 10, our_mac, 6);         // SA
    memcpy(frame + 16, our_mac, 6);         // BSSID

    int body = 24;
    body += 8;                              // TSF filled by hardware/firmware
    frame[body++] = 0x64; frame[body++] = 0x00; // Beacon interval
    frame[body++] = 0x31; frame[body++] = 0xc6; // Capability info

    frame[body++] = 0x00; frame[body++] = 0x00; // Empty SSID IE
    frame[body++] = 0x03; frame[body++] = 0x01; // DS Params IE
    frame[body++] = channel;

    const uint8_t vendor_ie[18] = {
        0xdd, 0x10,
        0x00, 0x50, 0xf2,
        0x11,
        0x01, 0x10,
        0x00, 0x24, 0x9d, 0x99,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00
    };
    memcpy(frame + body, vendor_ie, sizeof(vendor_ie));
    body += sizeof(vendor_ie);

    printf("[xbox] ProbeResp -> %02x:%02x:%02x:%02x:%02x:%02x on ch %u\n",
           dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], channel);
    peer_t *peer = remember_peer(dst);
    mt76_wlan_tx(g_dev, frame, body, peer ? peer->wcid : 0xff);
}

static void send_assoc_response_common(const uint8_t *dst,
                                       const uint8_t *our_mac,
                                       bool reassoc)
{
    uint8_t frame[24 + 6 + 8];
    memset(frame, 0, sizeof(frame));

    frame[0] = reassoc ? 0x30 : 0x10;       // Reassoc/Assoc Response
    frame[1] = 0x00;
    memcpy(frame + 4, dst, 6);              // DA
    memcpy(frame + 10, our_mac, 6);         // SA
    memcpy(frame + 16, our_mac, 6);         // BSSID

    int body = 24;
    frame[body++] = 0x00; frame[body++] = 0x00; // Capability info
    frame[body++] = 0x00; frame[body++] = 0x00; // Status code 0x0000 (success)
    frame[body++] = 0x01; frame[body++] = 0xc0; // Association ID 1 (bits 14..15 set)

    // Real captures show four empty SSID IEs (8 bytes total).
    for (int i = 0; i < 4; i++) {
        frame[body++] = 0x00;
        frame[body++] = 0x00;
    }

    printf("[xbox] %sResp -> %02x:%02x:%02x:%02x:%02x:%02x\n",
           reassoc ? "Reassoc" : "Assoc",
           dst[0], dst[1], dst[2], dst[3], dst[4], dst[5]);
    peer_t *peer = remember_peer(dst);
    if (peer && !peer->fw_client_added) {
        mt76_fw_client_add(g_dev, peer->wcid);
        peer->fw_client_added = true;
    }

    // xow uses WCID 0xff for association management responses.
    mt76_wlan_tx(g_dev, frame, body, 0xff);
}

static void send_assoc_response(const uint8_t *dst,
                                const uint8_t *our_mac)
{
    send_assoc_response_common(dst, our_mac, false);
}

static void send_reassoc_response(const uint8_t *dst,
                                  const uint8_t *our_mac)
{
    send_assoc_response_common(dst, our_mac, true);
}

static void send_auth_response(const uint8_t *dst,
                               const uint8_t *our_mac,
                               const uint8_t *payload,
                               int payload_len,
                               uint8_t req_subtype)
{
    uint8_t frame[24 + 6];
    memset(frame, 0, sizeof(frame));

    // The controller currently targets us with fc=0x0070 and a 4-byte body
    // (algorithm + sequence), not a standard 0x00b0 auth + status exchange.
    // Mirror that wire format exactly for subtype 7, and only use the
    // standard 6-byte auth body for subtype 11.
    frame[0] = (req_subtype == 7) ? 0x70 : 0xb0;
    frame[1] = 0x00;
    memcpy(frame + 4, dst, 6);              // DA
    memcpy(frame + 10, our_mac, 6);         // SA
    memcpy(frame + 16, our_mac, 6);         // BSSID

    uint16_t algorithm = 0x0000;            // Open System
    uint16_t sequence  = 0x0002;            // Response to seq=1
    uint16_t status    = 0x0000;            // Successful

    if (req_subtype == 7) {
        // Reserved pairing subtype: use the fixed wire values observed in xow captures.
        algorithm = 0x0170;
        sequence = 0x0002;
    } else {
        if (payload_len >= 2) memcpy(&algorithm, payload + 0, 2);
        if (payload_len >= 4) {
            uint16_t req_seq = 0;
            memcpy(&req_seq, payload + 2, 2);
            if (req_seq != 0)
                sequence = (uint16_t)(req_seq + 1);
        }
    }

    int body = 24;
    memcpy(frame + body, &algorithm, 2); body += 2;
    memcpy(frame + body, &sequence,  2); body += 2;
    if (req_subtype != 7) {
        memcpy(frame + body, &status, 2); body += 2;
    }

    printf("[xbox] AuthResp -> %02x:%02x:%02x:%02x:%02x:%02x fc=0x%02x alg=0x%04x seq=%u len=%d\n",
           dst[0], dst[1], dst[2], dst[3], dst[4], dst[5],
           frame[0], algorithm, sequence, body);
    uint8_t tx_wcid = 0xff;
    if (req_subtype != 7) {
        peer_t *peer = remember_peer(dst);
        if (peer) {
            tx_wcid = peer->wcid;
            printf("[xbox] AuthResp WCID %u for %02x:%02x:%02x:%02x:%02x:%02x\n",
                   peer->wcid, dst[0], dst[1], dst[2], dst[3], dst[4], dst[5]);
        } else {
            fprintf(stderr, "[xbox] AuthResp has no WCID for peer\n");
        }
    }
    mt76_wlan_tx(g_dev, frame, body, tx_wcid);
}

static void send_pair_response(const uint8_t *dst,
                               const uint8_t *our_mac)
{
    // Mirror xow's reserved pairing response payload.
    static const uint8_t pair_payload[] = {
        0x70, 0x02, 0x00, 0x45,
        0x55, 0x01, 0x0f, 0x8f,
        0xff, 0x87, 0x1f
    };

    uint8_t frame[24 + sizeof(pair_payload)];
    memset(frame, 0, sizeof(frame));

    // Management subtype 7 (reserved/pairing)
    frame[0] = 0x70;
    frame[1] = 0x00;
    memcpy(frame + 4, dst, 6);      // DA
    memcpy(frame + 10, our_mac, 6); // SA
    memcpy(frame + 16, our_mac, 6); // BSSID

    int body = 24;
    memcpy(frame + body, pair_payload, sizeof(pair_payload));
    body += (int)sizeof(pair_payload);

    printf("[xbox] PairResp -> %02x:%02x:%02x:%02x:%02x:%02x len=%d\n",
           dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], body);

    // Pairing response is pre-association; MT76 expects unknown/broadcast WCID as 0xff.
    mt76_wlan_tx(g_dev, frame, body, 0xff);
}

// Called when we see an Xbox vendor action frame addressed to us.
// src: controller MAC, payload: action body (after 24B hdr), plen: payload len
//
// Layout of payload:
//   [0]   = 0x7f  (Category: Vendor Specific — checked by caller)
//   [1-3] = 00:17:F2 (Xbox OUI)
//   [4]   = 0x08  (Xbox action type)
//   [5..] = vendor payload (used for one-time controller handshake)
static uint8_t peer_slot(peer_t *peer)
{
    if (!peer) return UINT8_MAX;
    if (peer->slot < MAX_SLOTS) return peer->slot;

    uint8_t slot = (uint8_t)(peer->wcid - 1);
    if (slot >= MAX_SLOTS) return UINT8_MAX;
    peer->slot = slot;
    return slot;
}

static void wlan_send_gip(peer_t *peer, const uint8_t *gip, size_t gip_len, const char *tag)
{
    if (!peer || !peer->active || gip_len == 0) return;

    const int wlan_hdr_len = 24 + 2; // 802.11 header + QoS control
    const int txwi_len = 20;
    const int frame_pad = (4 - ((txwi_len + wlan_hdr_len) & 3)) & 3;
    const int data_pad = (4 - ((int)gip_len & 3)) & 3;

    uint8_t payload[4 + 4 + 20 + 26 + 3 + 96 + 3];
    int off = 0;

    // xow-compatible packet prefix for CMD_PACKET_TX: WCID index in big-endian,
    // followed by a 4-byte zero block.
    uint32_t wcid_be = __builtin_bswap32((uint32_t)(peer->wcid - 1));
    memcpy(payload + off, &wcid_be, 4); off += 4;
    memset(payload + off, 0, 4); off += 4;

    // TxWi
    uint8_t txwi[20];
    memset(txwi, 0, sizeof(txwi));
    txwi[3] = 0x20; // phyType = OFDM
    txwi[4] = 0x01; // ack=1
    uint16_t mpdu_len = (uint16_t)(wlan_hdr_len + (int)gip_len);
    txwi[6] = (uint8_t)(mpdu_len & 0xff);
    txwi[7] = (uint8_t)((mpdu_len >> 8) & 0x3f);
    memcpy(payload + off, txwi, sizeof(txwi));
    off += (int)sizeof(txwi);

    // 802.11 QoS data header (from DS)
    payload[off++] = 0x88;
    payload[off++] = 0x02;
    payload[off++] = 0x90; // duration 144us (xow behavior)
    payload[off++] = 0x00;
    memcpy(payload + off, peer->mac, 6); off += 6;      // DA
    memcpy(payload + off, g_our_mac, 6); off += 6;      // SA
    memcpy(payload + off, g_our_mac, 6); off += 6;      // BSSID
    payload[off++] = 0x00;
    payload[off++] = 0x00;
    payload[off++] = 0x00; // QoS control
    payload[off++] = 0x00;

    if (frame_pad) {
        memset(payload + off, 0, frame_pad);
        off += frame_pad;
    }

    if (off + (int)gip_len + data_pad > (int)sizeof(payload)) {
        fprintf(stderr, "[gip]  %s payload too large (%zu)\n", tag ? tag : "TX", gip_len);
        return;
    }
    memcpy(payload + off, gip, gip_len);
    off += (int)gip_len;

    if (data_pad) {
        memset(payload + off, 0, data_pad);
        off += data_pad;
    }

    if (mt76_mcu_cmd(g_dev, 0 /* CMD_PACKET_TX */, payload, off) != 0) {
        fprintf(stderr, "[gip]  %s send failed\n", tag ? tag : "TX");
        return;
    }
    printf("[tx]   GIP TX %d bytes via CMD_PACKET_TX (%s)\n", off, tag ? tag : "TX");
}

static uint8_t gip_next_sequence(void)
{
    g_host_seq++;
    if (g_host_seq == 0)
        g_host_seq = 1;
    return g_host_seq;
}

static void send_wlan_gip_frame(peer_t *peer,
                                uint8_t cmd,
                                uint8_t device_id,
                                uint8_t type,
                                uint8_t seq,
                                const uint8_t *payload,
                                uint8_t payload_len,
                                const char *tag)
{
    uint8_t gip[4 + 64];
    uint8_t flags = (uint8_t)(((type & 0x0f) << 4) | (device_id & 0x0f));
    size_t gip_len = gip_build(gip, sizeof(gip), cmd, flags, seq, payload, payload_len);
    if (gip_len == 0) return;

    wlan_send_gip(peer, gip, gip_len, tag);
}

static void send_wlan_power_on(peer_t *peer)
{
    const uint8_t payload[1] = { 0x00 }; // POWER_ON
    uint8_t seq = gip_next_sequence();
    send_wlan_gip_frame(peer, GIP_CMD_POWER, 0, GIP_TYPE_REQUEST, seq,
                        payload, 1, "POWER_ON");
    printf("[gip]  PowerOn sent (seq=%u)\n", seq);
}

static void send_wlan_led_dim(peer_t *peer)
{
    const uint8_t payload[3] = { 0x00, 0x01, 0x14 }; // unknown, LED_ON, brightness
    uint8_t seq = gip_next_sequence();
    send_wlan_gip_frame(peer, GIP_CMD_LED, 0, GIP_TYPE_REQUEST, seq,
                        payload, 3, "LED_DIM");
    printf("[gip]  LED dim sent (seq=%u)\n", seq);
}

static void send_wlan_serial_request(peer_t *peer)
{
    const uint8_t payload[1] = { 0x04 };
    uint8_t seq = gip_next_sequence();
    send_wlan_gip_frame(peer, GIP_CMD_SERIAL, 0, (uint8_t)(GIP_TYPE_REQUEST | GIP_TYPE_ACK), seq,
                        payload, 1, "SERIAL_REQ");
    printf("[gip]  Serial request sent (seq=%u)\n", seq);
}

static void send_wlan_gip_ack(peer_t *peer,
                              uint8_t rx_cmd,
                              uint8_t rx_device,
                              uint8_t rx_seq,
                              uint8_t rx_len)
{
    if (!peer || !peer->active) return;

    uint8_t payload[9] = {0};
    payload[1] = rx_cmd;
    payload[2] = (uint8_t)((GIP_TYPE_REQUEST << 4) | (rx_device & 0x0f));
    payload[3] = rx_len;

    send_wlan_gip_frame(peer,
                        GIP_CMD_ACKNOWLEDGE,
                        rx_device,
                        GIP_TYPE_REQUEST,
                        rx_seq,
                        payload,
                        (uint8_t)sizeof(payload),
                        "ACK");
    printf("[gip]  ACK sent for cmd=0x%02x dev=%u seq=%u len=%u\n",
           rx_cmd, rx_device, rx_seq, rx_len);
}

static void dispatch_wlan_gip(peer_t *peer, uint8_t slot, const uint8_t *gip, int gip_len, const char *src)
{
    if (slot >= MAX_SLOTS || gip_len < 4) return;

    const gip_header_t *h = (const gip_header_t *)gip;
    int avail = gip_len - 4;
    uint8_t plen = (uint8_t)((h->length < avail) ? h->length : avail);

    uint8_t device_id = (uint8_t)(h->client & 0x0f);
    uint8_t frame_type = (uint8_t)((h->client >> 4) & 0x0f);

    printf("[gip]  %s cmd=0x%02x dev=%u type=%u slot=%u seq=%u len=%u\n",
           src, h->command, device_id, frame_type, slot, h->sequence, h->length);

    if (frame_type & GIP_TYPE_ACK) {
        send_wlan_gip_ack(peer, h->command, device_id, h->sequence, h->length);
    }

    switch (h->command) {
        case GIP_CMD_CTRL_ADDED:
            on_connect(slot);
            break;
        case GIP_CMD_CTRL_LOST:
            on_disconnect(slot);
            break;
        case GIP_CMD_ANNOUNCE:
            if (device_id == 0 && frame_type == GIP_TYPE_REQUEST) {
                // xow-compatible init sequence after announce.
                if (!peer->initial_commands_sent) {
                    send_wlan_power_on(peer);
                    send_wlan_led_dim(peer);
                    send_wlan_serial_request(peer);
                    peer->initial_commands_sent = true;
                }
                peer->session_initialized = true;
            }
            break;
        case GIP_CMD_STATUS:
            break;
        case GIP_CMD_INPUT:
            on_input(slot, gip + 4, plen);
            break;
        case GIP_CMD_INPUT_OVERFLOW:
            on_input(slot, gip + 4, plen);
            break;
        case GIP_CMD_GUIDE_BTN:
            if (plen >= 1) on_guide(slot, gip[4] != 0);
            break;
        default:
            printf("[gip]  %s unknown cmd=0x%02x (ignored)\n", src, h->command);
            break;
    }
}

static void on_xbox_action(const uint8_t *src, const uint8_t *payload, int plen)
{
    (void)payload;
    if (plen < 5) return;

    peer_t *peer = remember_peer(src);
    if (!peer) {
        fprintf(stderr, "[xbox] No free WCID for peer\n");
        return;
    }

    uint8_t slot = peer_slot(peer);
    if (slot >= MAX_SLOTS) {
        fprintf(stderr, "[xbox] WCID %u cannot map to local slot\n", peer->wcid);
        return;
    }

    // If this peer hasn't been set up yet, do the one-time handshake.
    if (!peer->handshake_done) {
        printf("[xbox] New controller %02x:%02x:%02x:%02x:%02x:%02x WCID=%u slot=%u\n",
               src[0],src[1],src[2],src[3],src[4],src[5], peer->wcid, slot);
        send_xbox_action_response(src, g_our_mac, peer->wcid);
        on_connect(slot);
        peer->handshake_done = true;
    }
}

static void on_xbox_data(const uint8_t *src, uint8_t rx_wcid,
                         const uint8_t *payload, int plen)
{
    if (plen <= 4) return;

    peer_t *peer = find_peer_by_mac(src);
    if (!peer)
        peer = remember_peer(src);
    if (!peer) {
        fprintf(stderr, "[xbox] No free WCID for data peer\n");
        return;
    }

    if (rx_wcid && rx_wcid != peer->wcid) {
        printf("[xbox] WCID mismatch src=%02x:%02x:%02x:%02x:%02x:%02x rx=%u mapped=%u\n",
               src[0],src[1],src[2],src[3],src[4],src[5], rx_wcid, peer->wcid);
    }

    uint8_t slot = peer_slot(peer);
    if (slot >= MAX_SLOTS) {
        fprintf(stderr, "[xbox] WCID %u cannot map to local slot\n", peer->wcid);
        return;
    }

    if (!peer->handshake_done) {
        on_connect(slot);
        peer->handshake_done = true;
        printf("[xbox] Data path established for slot %u\n", slot);
        if (!g_multi_pair_mode) {
            set_pairing_beacon(false, "controller data path established");
        } else {
            if (active_slot_count() < MAX_SLOTS)
                set_pairing_beacon(true, "multi-pair mode");
            else
                set_pairing_beacon(false, "all slots occupied");
        }
    }

    // QoS control (2B) + MT76 alignment pad (2B), then raw GIP frame.
    const uint8_t *gip = payload + 4;
    int gip_len = plen - 4;
    dispatch_wlan_gip(peer, slot, gip, gip_len, "DATA");
}

static void *reader_thread(void *arg)
{
    reader_arg_t *ra = (reader_arg_t *)arg;
    uint8_t ep = ra->ep;
    free(ra);

    printf("[thr]  Reader started on EP 0x%02x\n", ep);

    uint8_t buf[USB_BUF_SIZE];
    int idle = 0;

    while (g_running) {
        int n = 0;
        int r = libusb_bulk_transfer(g_dev, ep, buf, sizeof(buf), &n, 2000);

        if (r == LIBUSB_ERROR_TIMEOUT) {
            if (++idle % 15 == 0)   // ~30s without data
                printf("[thr]  EP 0x%02x: waiting for data...\n", ep);
            continue;
        }
        if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_IO) {
            printf("[thr]  EP 0x%02x: device disconnected\n", ep);
            g_running = false;
            break;
        }
        if (r == LIBUSB_ERROR_PIPE) {
            // EP is STALLed — Linux clears this automatically, macOS needs explicit clear.
            // Send CLEAR_FEATURE(ENDPOINT_HALT) then retry.
            printf("[thr]  EP 0x%02x: STALL — clearing halt...\n", ep);
            libusb_clear_halt(g_dev, ep);
            usleep(10 * 1000);  // 10ms settling
            continue;
        }
        if (r != LIBUSB_SUCCESS) {
            fprintf(stderr, "[thr]  EP 0x%02x Fehler: %s\n", ep, libusb_strerror(r));
            continue;
        }
        if (n > 0) {
            idle = 0;

            // ── MT76 receive packet format ─────────────────────────────
            // [RxInfo(4B)] [RxWi(32B)] [WlanFrame(24B)] [payload] [end(4B)]
            // RxInfo bits: infoType(31:30), port(29:27), is80211(19), length(13:0)
            if (n < 8) {
                printf("[usb]  IN←0x%02x (%d bytes, too short)\n", ep, n);
                continue;
            }

            uint32_t rxinfo;
            memcpy(&rxinfo, buf, 4);
            uint8_t  info_type = (rxinfo >> 30) & 3;
            uint8_t  port      = (rxinfo >> 27) & 7;
            int      pkt_len   = (int)(rxinfo & 0x3fff);
            int      is80211   = (rxinfo >> 19) & 1;
            uint8_t  event_type = (uint8_t)((rxinfo >> 20) & 0x0f);

            // RxWi size: dmaLength(4) + word1(4) + word2(4) + rssi[4](4) + bbpRxInfo[16](16) = 32B
            #define RXWI_SIZE 32
            bool is_wlan_rx =
                (info_type == 0 && port == 0 && is80211) ||
                (info_type == 1 && port == 1 && event_type == 0x0c); // EVT_PACKET_RX

            if (is_wlan_rx && n >= 4 + RXWI_SIZE + 24) {
                // WLAN frame: skip RxInfo(4B) + RxWi(32B) → WlanFrame at byte 36
                const uint8_t *wf = buf + 4 + RXWI_SIZE;  // WlanFrame start
                uint16_t fc;
                memcpy(&fc, wf, 2);
                uint8_t  fc_type    = (fc >> 2) & 3;   // 0=mgmt 1=ctrl 2=data
                uint8_t  fc_subtype = (fc >> 4) & 0xf; // 0=assoc 4=probe 8=beacon
                uint32_t rxwi_word1 = 0;
                memcpy(&rxwi_word1, buf + 8, 4);      // RxWi[1], wcid is bits 7:0
                uint8_t  rx_wcid = (uint8_t)(rxwi_word1 & 0xff);
                const uint8_t *dst  = wf + 4;
                const uint8_t *src  = wf + 10;
                const uint8_t *bss  = wf + 16;

                const char *type_str =
                    fc_type == 0 ? (fc_subtype == 0 ? "AssocReq" :
                                    fc_subtype == 7 ? "PairReq"  :
                                    fc_subtype == 2 ? "Reassoc"  :
                                    fc_subtype == 4 ? "ProbeReq" :
                                    fc_subtype == 8 ? "Beacon"   :
                                    fc_subtype ==11 ? "Auth"     : "Mgmt")    :
                    fc_type == 1 ? "Ctrl" :
                    fc_type == 2 ? "Data" : "???";

                // Detect frames matching our BSSID or our MAC
                bool to_us = (mac_equal(dst, g_our_mac) ||
                              mac_equal(bss, g_our_mac));
                bool is_directed_probe = (fc_type == 0 &&
                                          fc_subtype == 4 &&
                                          to_us);

                int payload_off = 4 + RXWI_SIZE + 24;
                int payload_len = n - payload_off - 4; // subtract end marker
                const uint8_t *pl = buf + payload_off;

                bool likely_xbox_peer =
                    mac_has_xbox_prefix(src) || mac_has_xbox_prefix(dst) || mac_has_xbox_prefix(bss);
                bool is_awdl_neighbor_action =
                    (fc_type == 0 && fc_subtype == 13 && !to_us && mac_is_awdl_bssid(bss));
                bool should_log_frame =
                    g_verbose_air ||
                    to_us ||
                    likely_xbox_peer ||
                    (fc_type == 0 && (fc_subtype == 0 || fc_subtype == 1 || fc_subtype == 4 || fc_subtype == 5 || fc_subtype == 13));

                if (is_awdl_neighbor_action && !g_verbose_air)
                    should_log_frame = false;

                if (should_log_frame) {
                    printf("[wlan] EP0x%02x %s%s fc=0x%04x len=%d\n"
                           "       dst=%02x:%02x:%02x:%02x:%02x:%02x\n"
                           "       src=%02x:%02x:%02x:%02x:%02x:%02x\n"
                           "       bss=%02x:%02x:%02x:%02x:%02x:%02x\n",
                        ep, to_us ? "*** " : "", type_str, fc, pkt_len,
                        dst[0],dst[1],dst[2],dst[3],dst[4],dst[5],
                        src[0],src[1],src[2],src[3],src[4],src[5],
                        bss[0],bss[1],bss[2],bss[3],bss[4],bss[5]);

                    if (payload_len > 0) {
                        int show = payload_len < 48 ? payload_len : 48;
                        printf("       payload(%d): ", payload_len);
                        for (int i = 0; i < show; i++)
                            printf("%02x ", buf[payload_off + i]);
                        if (payload_len > 48) printf("...");
                        printf("\n");
                    }
                }

                if (fc_type == 2 && fc_subtype == 8 && to_us && payload_len > 4) {
                    on_xbox_data(src, rx_wcid, pl, payload_len);
                }

                // ── Xbox vendor action frame handler ─────────────────────
                // Triggered when a controller associates and sends the Xbox
                // handshake: Category=0x7f, OUI=00:17:f2, Action=0x08
                // We respond with WCID assignment + association ack.
                bool is_xbox_action = (fc_type == 0 &&
                                       fc_subtype == 13 &&        // Action
                                       payload_len >= 5 &&
                                       pl[0] == 0x7f &&           // Vendor Specific
                                       pl[1] == XBOX_ACTION_OUI0 &&
                                       pl[2] == XBOX_ACTION_OUI1 &&
                                       pl[3] == XBOX_ACTION_OUI2 &&
                                       pl[4] == XBOX_ACTION_TYPE);

                if (is_xbox_action && to_us) {
                    on_xbox_action(src, pl, payload_len);
                } else if (is_xbox_action && !to_us) {
                    // Apple's AWDL uses the same vendor-specific action envelope,
                    // so filter it out explicitly to avoid misclassifying local
                    // peer-to-peer Wi-Fi traffic as Xbox handshakes.
                    if (mac_is_awdl_bssid(bss)) {
                        log_awdl_ignored();
                    } else {
                        printf("[xbox] Neighbor action (bss=%02x:%02x:%02x:%02x:%02x:%02x)\n",
                               bss[0],bss[1],bss[2],bss[3],bss[4],bss[5]);
                    }
                }

                // ── Association request handler ───────────────────────────
                // MT76 auto-responds in HW, but we still need to note the
                // controller's MAC so we can set up the GIP session.
                if (fc_type == 0 && fc_subtype == 4 && is_directed_probe) {
                    lock_pairing_channel("probe", src);
                    bool empty_ssid = payload_len >= 2 && pl[0] == 0x00 && pl[1] == 0x00;
                    if (empty_ssid) {
                        send_probe_response(src, g_our_mac, g_pairing_channel);
                    }
                }

                if (fc_type == 0 && fc_subtype == 7 && to_us) {
                    lock_pairing_channel("pair", src);
                    printf("[pair] Request from %02x:%02x:%02x:%02x:%02x:%02x\n",
                           src[0],src[1],src[2],src[3],src[4],src[5]);
                    bool pair_stage1 = (payload_len >= 2 && pl[0] == 0x70 && pl[1] == 0x01);
                    bool pair_stage2 = (payload_len >= 2 && pl[0] == 0x70 && pl[1] == 0x04);

                    if (pair_stage1) {
                        // Match xow flow for reserved pairing request type 0x01:
                        // send reserved pairing response and disable pairing mode.
                        send_pair_response(src, g_our_mac);
                        if (!g_multi_pair_mode) {
                            set_pairing_beacon(false, "pair stage1 completed");
                        } else if (active_slot_count() < MAX_SLOTS) {
                            set_pairing_beacon(true, "multi-pair mode");
                        }
                    } else if (pair_stage2) {
                        printf("[pair] Stage2 request observed (len=%d)\n", payload_len);
                    } else {
                        printf("[pair] Unknown subtype-7 payload head=%02x %02x len=%d\n",
                               payload_len > 0 ? pl[0] : 0,
                               payload_len > 1 ? pl[1] : 0,
                               payload_len);
                    }
                }

                if (fc_type == 0 && fc_subtype == 11 && to_us) {
                    lock_pairing_channel("auth", src);
                    printf("[auth] Request from %02x:%02x:%02x:%02x:%02x:%02x\n",
                           src[0],src[1],src[2],src[3],src[4],src[5]);
                    send_auth_response(src, g_our_mac, pl, payload_len, fc_subtype);
                }

                if (fc_type == 0 && fc_subtype == 0 && to_us) {
                    lock_pairing_channel("assoc", src);
                    peer_t *peer = find_peer_by_mac(src);
                    if (peer && peer->session_initialized) {
                        printf("[assoc] AssocReq from %02x:%02x:%02x:%02x:%02x:%02x — already initialized, skipping explicit response\n",
                               src[0],src[1],src[2],src[3],src[4],src[5]);
                    } else {
                        printf("[assoc] AssocReq from %02x:%02x:%02x:%02x:%02x:%02x — sending explicit response\n",
                               src[0],src[1],src[2],src[3],src[4],src[5]);
                        send_assoc_response(src, g_our_mac);
                    }
                }

                if (fc_type == 0 && fc_subtype == 2 && to_us) {
                    lock_pairing_channel("reassoc", src);
                    peer_t *peer = find_peer_by_mac(src);
                    if (peer && peer->session_initialized) {
                        printf("[assoc] ReassocReq from %02x:%02x:%02x:%02x:%02x:%02x — session active, letting firmware handle\n",
                               src[0],src[1],src[2],src[3],src[4],src[5]);
                    } else {
                        printf("[assoc] ReassocReq from %02x:%02x:%02x:%02x:%02x:%02x — sending explicit response\n",
                               src[0],src[1],src[2],src[3],src[4],src[5]);
                        send_reassoc_response(src, g_our_mac);
                    }
                }
            } else {
                // CPU RX events (EP0x85) often carry useful link state.
                if (info_type == 1 && port == 1) {
                    if (g_handle_client_lost && event_type == 0x0e && n >= 8) { // EVT_CLIENT_LOST
                        uint8_t lost_wcid = buf[4];
                        if (lost_wcid > 0 && lost_wcid < MAX_SLOTS + 1) {
                            printf("[xbox] Client lost event WCID=%u\n", lost_wcid);
                            on_disconnect((uint8_t)(lost_wcid - 1));
                            if (g_multi_pair_mode)
                                set_pairing_beacon(true, "multi-pair reconnect window");
                        }
                    } else if (event_type == 0x04) {
                        printf("[xbox] Dongle button event\n");
                        set_pairing_beacon(true, "dongle button pressed");
                    }
                }

                // CPU event or unknown — print raw
                printf("[usb]  IN←0x%02x type=%d port=%d len=%d: ",
                       ep, info_type, port, pkt_len);
                for (int i = 0; i < n && i < 24; i++) printf("%02x ", buf[i]);
                printf("\n");
            }
        }
    }
    return NULL;
}

static pthread_t g_reader1, g_reader2;

static void start_reader_threads(void)
{
    reader_arg_t *a1 = malloc(sizeof(*a1));
    a1->ep = g_ep_in;
    pthread_create(&g_reader1, NULL, reader_thread, a1);

    if (g_ep_in2) {
        reader_arg_t *a2 = malloc(sizeof(*a2));
        a2->ep = g_ep_in2;
        pthread_create(&g_reader2, NULL, reader_thread, a2);
    }
}

static void read_loop(void)
{
    // Reader threads already running — main thread just waits
    printf("[usb]  === PAIRING INSTRUCTIONS ===\n");
    printf("[usb]  1. Hold Xbox button on controller until it flashes RAPIDLY\n");
    printf("[usb]  2. Press the SMALL BUTTON on the dongle (next to LED)\n");
    printf("[usb]  3. Wait for connection...\n\n");

    int loop_count = 0;
    while (g_running) {
        sleep(1);

        // Every 2 seconds, check if we need channel scanning for pairing
        if (++loop_count % 2 == 0) {
            // Count active controllers
            int active = active_slot_count();

            // If no controllers connected, run pairing channel scan
            // This is needed for the small dongle (PID 0x02FE)
            if (active == 0) {
                if (!g_pairing_beacon_enabled) {
                    set_pairing_beacon(true, "no active controllers");
                }
                if (g_forced_pairing_channel)
                    continue;
                uint64_t now = now_monotonic_ms();
                if (now >= g_channel_lock_until_ms) {
                    g_pairing_channel = mt76_pairing_channel_scan(g_dev);
                }
            } else if (g_multi_pair_mode) {
                if (active < MAX_SLOTS) {
                    set_pairing_beacon(true, "multi-pair mode active");
                } else {
                    set_pairing_beacon(false, "all slots occupied");
                }
            }
        }
    }
    pthread_join(g_reader1, NULL);
    if (g_ep_in2) pthread_join(g_reader2, NULL);
}

// ══════════════════════════════════════════════
// main
// ══════════════════════════════════════════════

int main(void)
{
    printf("=== Xbox One Wireless Adapter Daemon ===\n");
    printf("libusb + IOHIDUserDevice — no entitlements needed\n\n");

    const char *verbose_air_env = getenv("XBOX_VERBOSE_AIR");
    g_verbose_air = verbose_air_env && verbose_air_env[0] && strcmp(verbose_air_env, "0") != 0;

    const char *wlan_ack_env = getenv("XBOX_WLAN_ACK");
    g_wlan_ack_enabled = wlan_ack_env && wlan_ack_env[0] && strcmp(wlan_ack_env, "0") != 0;
    printf("[init] WLAN GIP ACKs: %s\n", g_wlan_ack_enabled ? "enabled" : "disabled");

    const char *usb_ack_env = getenv("XBOX_USB_GIP_ACK");
    g_usb_gip_ack_enabled = usb_ack_env && usb_ack_env[0] && strcmp(usb_ack_env, "0") != 0;
    printf("[init] USB GIP ACKs: %s\n", g_usb_gip_ack_enabled ? "enabled" : "disabled");

    const char *lost_env = getenv("XBOX_CLIENT_LOST_EVENTS");
    g_handle_client_lost = lost_env && lost_env[0] && strcmp(lost_env, "0") != 0;
    printf("[init] Client-lost events: %s\n", g_handle_client_lost ? "enabled" : "disabled");

    const char *multi_pair_env = getenv("XBOX_MULTI_PAIR");
    g_multi_pair_mode = multi_pair_env && multi_pair_env[0] && strcmp(multi_pair_env, "0") != 0;
    printf("[init] Multi-controller pairing: %s\n", g_multi_pair_mode ? "enabled" : "disabled");


    const char *pair_ch_env = getenv("XBOX_PAIR_CHANNEL");
    if (pair_ch_env && pair_ch_env[0]) {
        int ch = atoi(pair_ch_env);
        if (ch == 36 || ch == 40 || ch == 44 || ch == 48) {
            g_forced_pairing_channel = (uint8_t)ch;
            g_pairing_channel = g_forced_pairing_channel;
            printf("[init] Fixed pairing channel requested: %d\n", ch);
        } else {
            fprintf(stderr, "[init] Ignoring invalid XBOX_PAIR_CHANNEL=%s (allowed: 36,40,44,48)\n", pair_ch_env);
        }
    }

    init_event_udp_from_env();

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    // Init libusb with full debug output
    if (libusb_init(&g_ctx) != LIBUSB_SUCCESS) {
        fprintf(stderr, "[usb]  libusb_init failed\n");
        return 1;
    }

    // Enable debug logging to see what's happening at USB level
    // Set to LIBUSB_LOG_LEVEL_DEBUG for maximum verbosity
    const char *debug_env = getenv("LIBUSB_DEBUG");
    if (debug_env) {
        int level = atoi(debug_env);
        libusb_set_option(g_ctx, LIBUSB_OPTION_LOG_LEVEL, level);
    } else {
        libusb_set_option(g_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
    }

    const char *reset_env = getenv("XBOX_USB_RESET_ON_START");
    g_usb_reset_on_start = !(reset_env && reset_env[0] && strcmp(reset_env, "0") == 0);
    printf("[init] USB reset on start: %s\n", g_usb_reset_on_start ? "enabled" : "disabled");

    // Show all Microsoft USB devices currently connected
    list_usb_devices();
    printf("\n");

    probe_hid_support();

    // Wait for dongle
    printf("[init] Waiting for Xbox Wireless Adapter (VID=0x045E)...\n");
    while (g_running && !(g_dev = open_dongle())) {
        sleep(1);
        list_usb_devices();
    }
    if (!g_running) goto cleanup;

    if (g_usb_reset_on_start) {
        printf("[init] Resetting USB device to clear stale state...\n");
        int rr = libusb_reset_device(g_dev);
        if (rr == LIBUSB_SUCCESS) {
            libusb_close(g_dev);
            g_dev = NULL;
            usleep(500 * 1000);

            for (int i = 0; i < 30 && g_running && !g_dev; i++) {
                g_dev = open_dongle();
                if (!g_dev) usleep(200 * 1000);
            }

            if (!g_dev) {
                fprintf(stderr, "[init] Failed to reopen dongle after reset\n");
                goto cleanup;
            }
            printf("[init] USB reset complete\n");
        } else {
            fprintf(stderr, "[init] USB reset skipped: %s\n", libusb_strerror(rr));
        }
    }

    // Detach kernel driver if needed
    if (libusb_kernel_driver_active(g_dev, 0) == 1) {
        libusb_detach_kernel_driver(g_dev, 0);
    }

    // Set configuration (no reset — would clear firmware state!)
    printf("[init] Setting USB configuration 1...\n");
    int cr = libusb_set_configuration(g_dev, 1);
    if (cr != LIBUSB_SUCCESS && cr != LIBUSB_ERROR_BUSY)
        fprintf(stderr, "[init] set_configuration warning: %s\n", libusb_strerror(cr));

    // Interface 0 claimen
    printf("[init] Claiming interface 0...\n");
    int claim = libusb_claim_interface(g_dev, 0);
    if (claim != LIBUSB_SUCCESS) {
        fprintf(stderr, "[init] claim_interface failed: %s\n", libusb_strerror(claim));
        goto cleanup;
    }
    printf("[init] Interface claimed OK\n");

    // Find bulk endpoints
    if (!find_endpoints(libusb_get_device(g_dev))) {
        fprintf(stderr, "[init] No bulk endpoints found — wrong device?\n");
        goto cleanup;
    }

    // ── Firmware upload for PID 0x02FE ───────────────────────────────────────
    // Uses proper DMA-based upload with control transfers (xow/xone protocol)
    {
        struct libusb_device_descriptor desc;
        libusb_get_device_descriptor(libusb_get_device(g_dev), &desc);

        if (desc.idProduct == XBOX_PID_V3) {
            printf("\n[fw]   PID 0x02FE detected: firmware upload required\n");

            size_t fw_size = 0;
            uint8_t *fw = mt76_load_firmware(FW_PATH, &fw_size);
            if (!fw) {
                fprintf(stderr,
                    "\n[fw]   ERROR: Firmware not found!\n"
                    "[fw]   Please run first:\n"
                    "[fw]     chmod +x extract_firmware.sh\n"
                    "[fw]     ./extract_firmware.sh\n"
                    "[fw]   Then: sudo ./xbox_daemon\n\n");
                goto cleanup;
            }

            // Use proper DMA-based firmware upload with control transfers
            printf("[fw]   Starting DMA-based firmware upload...\n");
            int fw_ret = mt76_upload_fw_dma(g_dev, fw, fw_size);
            free(fw);

            if (fw_ret != 0) {
                fprintf(stderr, "[fw]   Firmware upload failed\n");
                goto cleanup;
            }

            printf("[fw]   Firmware uploaded successfully\n");

            // On macOS the MT76 does NOT re-enumerate after IVB boot —
            // the USB connection stays live and the same handle remains valid.
            // (We verified this: FCE=0x00000000 was read back on the same handle
            //  right after MT_FW_LOAD_IVB.)  Simply wait ~200ms for the firmware
            // to finish its internal init, then proceed on the same handle.
            printf("[fw]   Waiting for firmware to settle (200ms)...\n");
            usleep(200 * 1000);

            printf("[fw]   Firmware active — initializing radio...\n\n");

            // Now perform full radio initialization (registers, MCU commands, beacon)
            if (mt76_radio_init(g_dev, g_our_mac) != 0) {
                fprintf(stderr, "[fw]   Radio initialization failed\n");
                goto cleanup;
            }

            printf("[init] Our Xbox MAC/BSSID = %02x:%02x:%02x:%02x:%02x:%02x (channel %u)\n",
                   g_our_mac[0], g_our_mac[1], g_our_mac[2],
                   g_our_mac[3], g_our_mac[4], g_our_mac[5], g_pairing_channel);

            if (g_forced_pairing_channel) {
                uint8_t hw_ch = (uint8_t)(g_forced_pairing_channel - 12); // 36->0x24, 40->0x28, 44->0x2c, 48->0x30
                if (mt76_configure_channel(g_dev, hw_ch, MCU_CH_BW_20, 1, 0x18) == 0) {
                    mt76_write_beacon(g_dev, g_our_mac, 1, hw_ch);
                    printf("[mt76]  Fixed on pairing channel %u\n", g_forced_pairing_channel);
                } else {
                    fprintf(stderr, "[mt76]  Failed to force pairing channel %u\n", g_forced_pairing_channel);
                }
            }

            // Give radio time to settle
            printf("[fw]   Radio settling (500ms)...\n");
            usleep(500 * 1000);
        }
    }

    // Probe endpoints to see if dongle is sending any data
    printf("[init] Probing endpoints for any incoming data...\n");
    uint8_t probe_buf[USB_BUF_SIZE];
    int probe_n = 0;
    int probe_r = libusb_bulk_transfer(g_dev, g_ep_in, probe_buf, sizeof(probe_buf), &probe_n, 500);
    if (probe_r == LIBUSB_SUCCESS && probe_n > 0) {
        printf("[init] Data on EP 0x%02x: ", g_ep_in);
        hexdump("", probe_buf, probe_n);
    } else {
        printf("[init] EP 0x%02x: %s\n", g_ep_in, libusb_strerror(probe_r));
    }

    // ── Start reader threads FIRST ────────────────────────────────────────
// Important: on Linux the kernel driver starts IN-URBs before the init send.
// Device sends immediately after init receipt — nobody must miss the response.
    printf("[init] Starting IN reader threads on EP 0x%02x and EP 0x%02x...\n", g_ep_in, g_ep_in2);
    start_reader_threads();
    usleep(100 * 1000);  // 100ms: Threads must block in libusb_bulk_transfer

    // PID 0x02FE uses the MT76 802.11 WLAN stack — no raw GIP commands go over USB.
    // Communication is entirely through WLAN frames: the beacon (already written by
    // mt76_radio_init) makes the AP visible; controllers associate via 802.11 and
    // send GIP over WLAN.  DO NOT write raw bytes to any OUT endpoint here.

    printf("\n[init] Ready.\n");
    printf("[init] → Hold Xbox button on controller AND press button on dongle\n\n");

    // Read loop
    read_loop();

cleanup:
    printf("\n[exit] Cleaning up...\n");
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (g_slots[i].hid_device) {
            CFRelease(g_slots[i].hid_device);
            g_slots[i].hid_device = NULL;
        }
    }
    if (g_dev) {
        libusb_release_interface(g_dev, 0);
        libusb_close(g_dev);
    }
    if (g_event_udp_sock >= 0) {
        close(g_event_udp_sock);
        g_event_udp_sock = -1;
    }
    libusb_exit(g_ctx);
    printf("[exit] Done.\n");
    return 0;
}
