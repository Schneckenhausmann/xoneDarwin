#include "xbox_protocol.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_SLOTS 8

// SDL constants (kept local to avoid SDL headers dependency)
#define SDL_INIT_JOYSTICK       0x00000200u
#define SDL_INIT_GAMECONTROLLER 0x00002000u

#define SDL_JOYSTICK_TYPE_GAMECONTROLLER 1

#define SDL_HAT_CENTERED  0x00
#define SDL_HAT_UP        0x01
#define SDL_HAT_RIGHT     0x02
#define SDL_HAT_DOWN      0x04
#define SDL_HAT_LEFT      0x08
#define SDL_HAT_RIGHTUP   (SDL_HAT_RIGHT | SDL_HAT_UP)
#define SDL_HAT_RIGHTDOWN (SDL_HAT_RIGHT | SDL_HAT_DOWN)
#define SDL_HAT_LEFTUP    (SDL_HAT_LEFT  | SDL_HAT_UP)
#define SDL_HAT_LEFTDOWN  (SDL_HAT_LEFT  | SDL_HAT_DOWN)

typedef uint32_t Uint32;
typedef uint8_t Uint8;
typedef int SDL_bool;
typedef struct _SDL_Joystick SDL_Joystick;
typedef int32_t SDL_JoystickID;

typedef struct SDL_VirtualJoystickDesc {
    Uint32 version;
    uint16_t type;
    uint16_t padding;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t naxes;
    uint16_t nbuttons;
    uint16_t nballs;
    uint16_t nhats;
    uint16_t ntouchpads;
    uint16_t nsensors;
    uint16_t padding2[2];
    Uint32 button_mask;
    Uint32 axis_mask;
    const char *name;
    const void *touchpads;
    const void *sensors;
    void *userdata;
    void (*Update)(void *userdata);
    void (*SetPlayerIndex)(void *userdata, int player_index);
    bool (*Rumble)(void *userdata, uint16_t low_frequency_rumble, uint16_t high_frequency_rumble);
    bool (*RumbleTriggers)(void *userdata, uint16_t left_rumble, uint16_t right_rumble);
    bool (*SetLED)(void *userdata, Uint8 red, Uint8 green, Uint8 blue);
    bool (*SendEffect)(void *userdata, const void *data, int size);
    bool (*SetSensorsEnabled)(void *userdata, bool enabled);
    void (*Cleanup)(void *userdata);
} SDL_VirtualJoystickDesc;

typedef Uint32 (*SDL_WasInit_Fn)(Uint32);
typedef int (*SDL_Init_Fn)(Uint32);
typedef int (*SDL_InitSubSystem_Fn)(Uint32);
typedef void (*SDL_UpdateJoysticks_Fn)(void);

// SDL2 virtual joystick API
typedef int (*SDL2_JoystickAttachVirtual_Fn)(int, int, int, int);
typedef int (*SDL2_JoystickDetachVirtual_Fn)(int);
typedef SDL_Joystick *(*SDL2_JoystickOpen_Fn)(int);

// SDL3 virtual joystick API
typedef SDL_JoystickID (*SDL3_AttachVirtualJoystick_Fn)(const SDL_VirtualJoystickDesc *desc);
typedef SDL_bool (*SDL3_DetachVirtualJoystick_Fn)(SDL_JoystickID instance_id);
typedef SDL_Joystick *(*SDL3_OpenJoystick_Fn)(SDL_JoystickID instance_id);

typedef void (*SDL_JoystickClose_Fn)(SDL_Joystick *);
typedef int (*SDL2_JoystickSetVirtualAxis_Fn)(SDL_Joystick *, int, int16_t);
typedef int (*SDL2_JoystickSetVirtualButton_Fn)(SDL_Joystick *, int, Uint8);
typedef int (*SDL2_JoystickSetVirtualHat_Fn)(SDL_Joystick *, int, Uint8);
typedef SDL_bool (*SDL3_SetJoystickVirtualAxis_Fn)(SDL_Joystick *, int, int16_t);
typedef SDL_bool (*SDL3_SetJoystickVirtualButton_Fn)(SDL_Joystick *, int, SDL_bool);
typedef SDL_bool (*SDL3_SetJoystickVirtualHat_Fn)(SDL_Joystick *, int, Uint8);

typedef struct {
    SDL_WasInit_Fn SDL_WasInit;
    SDL_Init_Fn SDL_Init;
    SDL_InitSubSystem_Fn SDL_InitSubSystem;
    SDL_UpdateJoysticks_Fn SDL_UpdateJoysticks;

    SDL2_JoystickAttachVirtual_Fn SDL2_JoystickAttachVirtual;
    SDL2_JoystickDetachVirtual_Fn SDL2_JoystickDetachVirtual;
    SDL2_JoystickOpen_Fn SDL2_JoystickOpen;

    SDL3_AttachVirtualJoystick_Fn SDL3_AttachVirtualJoystick;
    SDL3_DetachVirtualJoystick_Fn SDL3_DetachVirtualJoystick;
    SDL3_OpenJoystick_Fn SDL3_OpenJoystick;

    SDL_JoystickClose_Fn SDL_JoystickClose;
    SDL2_JoystickSetVirtualAxis_Fn SDL2_JoystickSetVirtualAxis;
    SDL2_JoystickSetVirtualButton_Fn SDL2_JoystickSetVirtualButton;
    SDL2_JoystickSetVirtualHat_Fn SDL2_JoystickSetVirtualHat;
    SDL3_SetJoystickVirtualAxis_Fn SDL3_SetJoystickVirtualAxis;
    SDL3_SetJoystickVirtualButton_Fn SDL3_SetJoystickVirtualButton;
    SDL3_SetJoystickVirtualHat_Fn SDL3_SetJoystickVirtualHat;

    int api_version;
    int ready;
} sdl_api_t;

typedef struct {
    int active;
    SDL_JoystickID instance_id;
    SDL_Joystick *joy;
    int calibrated;
    int base_lt, base_rt;
    int base_lx, base_ly, base_rx, base_ry;
    int buttons;
    int lt, rt, lx, ly, rx, ry;
} slot_state_t;

static sdl_api_t g_sdl = {0};
static slot_state_t g_slots[MAX_SLOTS] = {0};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_stick_gain = 1;
static int g_stick_deadzone = 500;
static int g_swap_lr = 0;
static int g_swap_sticks = 0;
static int g_xone_axis_layout = 0;
static int g_logged_no_sdl = 0;
static int g_logged_first_udp = 0;
static int g_logged_unknown_line = 0;
static void *g_sdl_handle2 = NULL;
static void *g_sdl_handle3 = NULL;
static int g_sdl_handles_init = 0;

static void init_sdl_handles(void)
{
    if (g_sdl_handles_init) return;
    g_sdl_handles_init = 1;

    char exe_path[PATH_MAX];
    uint32_t exe_size = (uint32_t)sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &exe_size) != 0) {
        return;
    }

    const char *marker = "/Contents/MacOS/";
    char *p = strstr(exe_path, marker);
    if (!p) {
        return;
    }

    *p = '\0';

    char sdl3_path[PATH_MAX];
    char sdl2_path[PATH_MAX];
    snprintf(sdl3_path, sizeof(sdl3_path), "%s/Contents/Frameworks/libSDL3.dylib", exe_path);
    snprintf(sdl2_path, sizeof(sdl2_path), "%s/Contents/Frameworks/libSDL2.dylib", exe_path);

    g_sdl_handle3 = dlopen(sdl3_path, RTLD_NOW | RTLD_GLOBAL);
    g_sdl_handle2 = dlopen(sdl2_path, RTLD_NOW | RTLD_GLOBAL);

    if (!g_sdl_handle3 && !g_sdl_handle2) {
        const char *err = dlerror();
        if (err) {
            fprintf(stderr, "[inject] dlopen SDL frameworks failed: %s\n", err);
        }
    }
}

static void *sdl_sym(const char *name)
{
    void *sym = dlsym(RTLD_DEFAULT, name);
    if (sym) return sym;

    init_sdl_handles();

    if (g_sdl_handle3) {
        sym = dlsym(g_sdl_handle3, name);
        if (sym) return sym;
    }
    if (g_sdl_handle2) {
        sym = dlsym(g_sdl_handle2, name);
        if (sym) return sym;
    }

    return NULL;
}

static int json_has_type(const char *json, const char *type)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"type\":\"%s\"", type);
    return strstr(json, needle) != NULL;
}

static int json_get_int(const char *json, const char *key, int *out)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;

    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = (int)v;
    return 1;
}

static int dpad_to_hat(int buttons)
{
    int up = (buttons & BTN_DPAD_UP) != 0;
    int down = (buttons & BTN_DPAD_DOWN) != 0;
    int left = (buttons & BTN_DPAD_LEFT) != 0;
    int right = (buttons & BTN_DPAD_RIGHT) != 0;

    if (up && !down && !left && !right) return SDL_HAT_UP;
    if (up && !down && !left && right) return SDL_HAT_RIGHTUP;
    if (!up && !down && !left && right) return SDL_HAT_RIGHT;
    if (!up && down && !left && right) return SDL_HAT_RIGHTDOWN;
    if (!up && down && !left && !right) return SDL_HAT_DOWN;
    if (!up && down && left && !right) return SDL_HAT_LEFTDOWN;
    if (!up && !down && left && !right) return SDL_HAT_LEFT;
    if (up && !down && left && !right) return SDL_HAT_LEFTUP;
    return SDL_HAT_CENTERED;
}

static int16_t trigger_to_sdl_axis(int v)
{
    if (v < 0) v = 0;
    if (v > 4095) v = 4095;
    // xone/xow-style triggers are unipolar (0..max).
    return (int16_t)((v * 32767) / 4095);
}

static int16_t scale_stick_axis(int v)
{
    if (v > -g_stick_deadzone && v < g_stick_deadzone) {
        return 0;
    }
    long x = (long)v * (long)g_stick_gain;
    if (x > 32767) x = 32767;
    if (x < -32768) x = -32768;
    return (int16_t)x;
}

static SDL_JoystickID sdl_attach_virtual(void)
{
    if (g_sdl.api_version == 2 && g_sdl.SDL2_JoystickAttachVirtual) {
        return (SDL_JoystickID)g_sdl.SDL2_JoystickAttachVirtual(
            SDL_JOYSTICK_TYPE_GAMECONTROLLER,
            6,
            21,
            1
        );
    }

    if (g_sdl.api_version == 3 && g_sdl.SDL3_AttachVirtualJoystick) {
        SDL_VirtualJoystickDesc desc;
        memset(&desc, 0, sizeof(desc));
        desc.version = (Uint32)sizeof(desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        desc.vendor_id = 0x045e;
        desc.product_id = 0x02ea;
        desc.naxes = 6;
        desc.nbuttons = 21;
        desc.nhats = 1;
        desc.button_mask = (1u << 21) - 1u;
        desc.axis_mask = (1u << 6) - 1u;
        desc.name = "Xbox Wireless Adapter Virtual Pad";
        return g_sdl.SDL3_AttachVirtualJoystick(&desc);
    }

    return -1;
}

static void sdl_detach_virtual(SDL_JoystickID instance_id)
{
    if (g_sdl.api_version == 2 && g_sdl.SDL2_JoystickDetachVirtual) {
        g_sdl.SDL2_JoystickDetachVirtual((int)instance_id);
    } else if (g_sdl.api_version == 3 && g_sdl.SDL3_DetachVirtualJoystick) {
        g_sdl.SDL3_DetachVirtualJoystick(instance_id);
    }
}

static SDL_Joystick *sdl_open_joystick(SDL_JoystickID instance_id)
{
    if (g_sdl.api_version == 2 && g_sdl.SDL2_JoystickOpen) {
        return g_sdl.SDL2_JoystickOpen((int)instance_id);
    }
    if (g_sdl.api_version == 3 && g_sdl.SDL3_OpenJoystick) {
        return g_sdl.SDL3_OpenJoystick(instance_id);
    }
    return NULL;
}

static void sdl_set_axis(SDL_Joystick *joy, int axis, int16_t value)
{
    if (g_sdl.api_version == 2 && g_sdl.SDL2_JoystickSetVirtualAxis) {
        g_sdl.SDL2_JoystickSetVirtualAxis(joy, axis, value);
    } else if (g_sdl.api_version == 3 && g_sdl.SDL3_SetJoystickVirtualAxis) {
        g_sdl.SDL3_SetJoystickVirtualAxis(joy, axis, value);
    }
}

static void sdl_set_button(SDL_Joystick *joy, int button, Uint8 down)
{
    if (g_sdl.api_version == 2 && g_sdl.SDL2_JoystickSetVirtualButton) {
        g_sdl.SDL2_JoystickSetVirtualButton(joy, button, down);
    } else if (g_sdl.api_version == 3 && g_sdl.SDL3_SetJoystickVirtualButton) {
        g_sdl.SDL3_SetJoystickVirtualButton(joy, button, down ? 1 : 0);
    }
}

static void sdl_set_hat(SDL_Joystick *joy, int hat, Uint8 value)
{
    if (g_sdl.api_version == 2 && g_sdl.SDL2_JoystickSetVirtualHat) {
        g_sdl.SDL2_JoystickSetVirtualHat(joy, hat, value);
    } else if (g_sdl.api_version == 3 && g_sdl.SDL3_SetJoystickVirtualHat) {
        g_sdl.SDL3_SetJoystickVirtualHat(joy, hat, value);
    }
}

static void sdl_flush_updates(void)
{
    if (g_sdl.SDL_UpdateJoysticks) {
        g_sdl.SDL_UpdateJoysticks();
    }
}

static void apply_slot_state_locked(int slot)
{
    slot_state_t *s = &g_slots[slot];
    if (!s->active || s->instance_id < 0 || !s->joy) return;

    int lx = s->lx;
    int ly = s->ly;
    int rx = s->rx;
    int ry = s->ry;
    if (g_swap_sticks) {
        int tlx = lx, tly = ly;
        lx = rx; ly = ry;
        rx = tlx; ry = tly;
    }

    // Match xone/xow behavior for vertical axes.
    int16_t ax_lx = scale_stick_axis(lx);
    int16_t ax_ly = scale_stick_axis(-ly);
    int16_t ax_rx = scale_stick_axis(rx);
    int16_t ax_ry = scale_stick_axis(-ry);

    // Two supported layouts:
    // - xone/xow: LX, LY, LT, RX, RY, RT
    // - sdl-gamepad: LX, LY, RX, RY, LT, RT
    if (g_xone_axis_layout) {
        sdl_set_axis(s->joy, 0, ax_lx);
        sdl_set_axis(s->joy, 1, ax_ly);
    } else {
        sdl_set_axis(s->joy, 0, ax_lx);
        sdl_set_axis(s->joy, 1, ax_ly);
        sdl_set_axis(s->joy, 2, ax_rx);
        sdl_set_axis(s->joy, 3, ax_ry);
    }
    if (g_swap_lr) {
        if (g_xone_axis_layout) {
            sdl_set_axis(s->joy, 2, trigger_to_sdl_axis(s->rt));
            sdl_set_axis(s->joy, 3, ax_rx);
            sdl_set_axis(s->joy, 4, ax_ry);
            sdl_set_axis(s->joy, 5, trigger_to_sdl_axis(s->lt));
        } else {
            sdl_set_axis(s->joy, 4, trigger_to_sdl_axis(s->rt));
            sdl_set_axis(s->joy, 5, trigger_to_sdl_axis(s->lt));
        }
    } else {
        if (g_xone_axis_layout) {
            sdl_set_axis(s->joy, 2, trigger_to_sdl_axis(s->lt));
            sdl_set_axis(s->joy, 3, ax_rx);
            sdl_set_axis(s->joy, 4, ax_ry);
            sdl_set_axis(s->joy, 5, trigger_to_sdl_axis(s->rt));
        } else {
            sdl_set_axis(s->joy, 4, trigger_to_sdl_axis(s->lt));
            sdl_set_axis(s->joy, 5, trigger_to_sdl_axis(s->rt));
        }
    }

    // Buttons (SDL gamecontroller order)
    sdl_set_button(s->joy, 0, (s->buttons & BTN_A) ? 1 : 0);
    sdl_set_button(s->joy, 1, (s->buttons & BTN_B) ? 1 : 0);
    sdl_set_button(s->joy, 2, (s->buttons & BTN_X) ? 1 : 0);
    sdl_set_button(s->joy, 3, (s->buttons & BTN_Y) ? 1 : 0);
    sdl_set_button(s->joy, 4, (s->buttons & BTN_VIEW) ? 1 : 0);
    sdl_set_button(s->joy, 5, (s->buttons & BTN_SYNC) ? 1 : 0);
    sdl_set_button(s->joy, 6, (s->buttons & BTN_MENU) ? 1 : 0);
    sdl_set_button(s->joy, 7, (s->buttons & BTN_LS) ? 1 : 0);
    sdl_set_button(s->joy, 8, (s->buttons & BTN_RS) ? 1 : 0);
    if (g_swap_lr) {
        sdl_set_button(s->joy, 9, (s->buttons & BTN_RB) ? 1 : 0);
        sdl_set_button(s->joy, 10, (s->buttons & BTN_LB) ? 1 : 0);
    } else {
        sdl_set_button(s->joy, 9, (s->buttons & BTN_LB) ? 1 : 0);
        sdl_set_button(s->joy, 10, (s->buttons & BTN_RB) ? 1 : 0);
    }
    sdl_set_button(s->joy, 11, (s->buttons & BTN_DPAD_UP) ? 1 : 0);
    sdl_set_button(s->joy, 12, (s->buttons & BTN_DPAD_DOWN) ? 1 : 0);
    sdl_set_button(s->joy, 13, (s->buttons & BTN_DPAD_LEFT) ? 1 : 0);
    sdl_set_button(s->joy, 14, (s->buttons & BTN_DPAD_RIGHT) ? 1 : 0);
    // Leave higher buttons unbound for now to avoid trigger/shoulder confusion.
    sdl_set_button(s->joy, 15, 0);
    sdl_set_button(s->joy, 16, 0);

    sdl_set_hat(s->joy, 0, (Uint8)dpad_to_hat(s->buttons));
    sdl_flush_updates();

}

static int ensure_sdl_ready(void)
{
    if (g_sdl.ready) return 1;

    g_sdl.SDL_Init = (SDL_Init_Fn)sdl_sym("SDL_Init");
    g_sdl.SDL_WasInit = (SDL_WasInit_Fn)sdl_sym("SDL_WasInit");
    g_sdl.SDL_InitSubSystem = (SDL_InitSubSystem_Fn)sdl_sym("SDL_InitSubSystem");
    g_sdl.SDL_UpdateJoysticks = (SDL_UpdateJoysticks_Fn)sdl_sym("SDL_UpdateJoysticks");

    g_sdl.SDL_JoystickClose = (SDL_JoystickClose_Fn)sdl_sym("SDL_JoystickClose");
    if (!g_sdl.SDL_WasInit && !g_sdl.SDL_InitSubSystem && !g_sdl.SDL_Init) {
        if (!g_logged_no_sdl) {
            fprintf(stderr, "[inject] SDL core symbols not ready yet (waiting for Ryujinx SDL init)\n");
            g_logged_no_sdl = 1;
        }
    }

    // SDL2 symbols
    g_sdl.SDL2_JoystickAttachVirtual = (SDL2_JoystickAttachVirtual_Fn)sdl_sym("SDL_JoystickAttachVirtual");
    g_sdl.SDL2_JoystickDetachVirtual = (SDL2_JoystickDetachVirtual_Fn)sdl_sym("SDL_JoystickDetachVirtual");
    g_sdl.SDL2_JoystickOpen = (SDL2_JoystickOpen_Fn)sdl_sym("SDL_JoystickOpen");
    g_sdl.SDL2_JoystickSetVirtualAxis = (SDL2_JoystickSetVirtualAxis_Fn)sdl_sym("SDL_JoystickSetVirtualAxis");
    g_sdl.SDL2_JoystickSetVirtualButton = (SDL2_JoystickSetVirtualButton_Fn)sdl_sym("SDL_JoystickSetVirtualButton");
    g_sdl.SDL2_JoystickSetVirtualHat = (SDL2_JoystickSetVirtualHat_Fn)sdl_sym("SDL_JoystickSetVirtualHat");

    // SDL3 symbols
    g_sdl.SDL3_AttachVirtualJoystick = (SDL3_AttachVirtualJoystick_Fn)sdl_sym("SDL_AttachVirtualJoystick");
    g_sdl.SDL3_DetachVirtualJoystick = (SDL3_DetachVirtualJoystick_Fn)sdl_sym("SDL_DetachVirtualJoystick");
    g_sdl.SDL3_OpenJoystick = (SDL3_OpenJoystick_Fn)sdl_sym("SDL_OpenJoystick");
    g_sdl.SDL3_SetJoystickVirtualAxis = (SDL3_SetJoystickVirtualAxis_Fn)sdl_sym("SDL_SetJoystickVirtualAxis");
    g_sdl.SDL3_SetJoystickVirtualButton = (SDL3_SetJoystickVirtualButton_Fn)sdl_sym("SDL_SetJoystickVirtualButton");
    g_sdl.SDL3_SetJoystickVirtualHat = (SDL3_SetJoystickVirtualHat_Fn)sdl_sym("SDL_SetJoystickVirtualHat");

    if (g_sdl.SDL2_JoystickAttachVirtual &&
        g_sdl.SDL2_JoystickDetachVirtual &&
        g_sdl.SDL2_JoystickOpen &&
        g_sdl.SDL2_JoystickSetVirtualAxis &&
        g_sdl.SDL2_JoystickSetVirtualButton &&
        g_sdl.SDL2_JoystickSetVirtualHat) {
        g_sdl.api_version = 2;
    } else if (g_sdl.SDL3_AttachVirtualJoystick &&
               g_sdl.SDL3_DetachVirtualJoystick &&
               g_sdl.SDL3_OpenJoystick &&
               g_sdl.SDL3_SetJoystickVirtualAxis &&
               g_sdl.SDL3_SetJoystickVirtualButton &&
               g_sdl.SDL3_SetJoystickVirtualHat) {
        g_sdl.api_version = 3;
    } else {
        if (!g_logged_no_sdl) {
            fprintf(stderr, "[inject] neither SDL2 nor SDL3 virtual joystick symbols available yet\n");
            g_logged_no_sdl = 1;
        }
        return 0;
    }

    Uint32 need = SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER;
    if (g_sdl.SDL_WasInit && g_sdl.SDL_InitSubSystem &&
        ((g_sdl.SDL_WasInit(need) & need) != need)) {
        g_sdl.SDL_InitSubSystem(need);
    } else if (g_sdl.SDL_Init) {
        g_sdl.SDL_Init(need);
    }

    g_sdl.ready = 1;
    g_logged_no_sdl = 0;
    fprintf(stderr, "[inject] SDL virtual joystick API ready (SDL%d)\n", g_sdl.api_version);
    return 1;
}

static void on_connected(int slot)
{
    if (slot < 0 || slot >= MAX_SLOTS) return;
    if (!ensure_sdl_ready()) return;

    fprintf(stderr, "[inject] connected event slot=%d\n", slot);

    pthread_mutex_lock(&g_lock);
    slot_state_t *s = &g_slots[slot];
    if (!s->active) {
        SDL_JoystickID jid = sdl_attach_virtual();
        int attach_ok = (g_sdl.api_version == 3) ? (jid > 0) : (jid >= 0);
        if (attach_ok) {
            SDL_Joystick *joy = sdl_open_joystick(jid);
            if (!joy) {
                sdl_detach_virtual(jid);
                fprintf(stderr, "[inject] attached id %d but failed opening joystick\n", (int)jid);
                pthread_mutex_unlock(&g_lock);
                return;
            }
            s->active = 1;
            s->instance_id = jid;
            s->joy = joy;
            s->calibrated = 0;
            s->base_lt = s->base_rt = 0;
            s->base_lx = s->base_ly = s->base_rx = s->base_ry = 0;
            s->buttons = 0;
            s->lt = s->rt = s->lx = s->ly = s->rx = s->ry = 0;
            apply_slot_state_locked(slot);
            fprintf(stderr, "[inject] slot %d attached as virtual SDL joystick id=%d\n", slot, (int)jid);
        } else {
            fprintf(stderr, "[inject] failed attaching virtual joystick for slot %d\n", slot);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

static void on_disconnected(int slot)
{
    if (slot < 0 || slot >= MAX_SLOTS) return;
    pthread_mutex_lock(&g_lock);
    slot_state_t *s = &g_slots[slot];
    if (s->active) {
        if (s->joy && g_sdl.SDL_JoystickClose)
            g_sdl.SDL_JoystickClose(s->joy);
        sdl_detach_virtual(s->instance_id);
        memset(s, 0, sizeof(*s));
        s->instance_id = -1;
        fprintf(stderr, "[inject] slot %d detached\n", slot);
    }
    pthread_mutex_unlock(&g_lock);
}

static void on_input(const char *line)
{
    int slot = -1;
    int buttons = 0;
    int lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;

    if (!json_get_int(line, "slot", &slot)) return;
    if (slot < 0 || slot >= MAX_SLOTS) return;
    if (!json_get_int(line, "buttons", &buttons)) return;

    if (!ensure_sdl_ready()) return;

    // If we missed the explicit "connected" event (e.g. injector started late),
    // lazily create the virtual device on first input packet for this slot.
    if (!g_slots[slot].active) {
        on_connected(slot);
    }

    json_get_int(line, "lt", &lt);
    json_get_int(line, "rt", &rt);
    json_get_int(line, "lx", &lx);
    json_get_int(line, "ly", &ly);
    json_get_int(line, "rx", &rx);
    json_get_int(line, "ry", &ry);

    pthread_mutex_lock(&g_lock);
    slot_state_t *s = &g_slots[slot];
    if (s->active) {
        if (!s->calibrated) {
            s->base_lt = lt;
            s->base_rt = rt;
            s->base_lx = lx;
            s->base_ly = ly;
            s->base_rx = rx;
            s->base_ry = ry;
            s->calibrated = 1;
            fprintf(stderr, "[inject] slot %d calibrated: LT=%d RT=%d LX=%d LY=%d RX=%d RY=%d\n",
                    slot, s->base_lt, s->base_rt, s->base_lx, s->base_ly, s->base_rx, s->base_ry);
        }

        // remove neutral offsets seen in daemon UDP stream
        int c_lt = lt - s->base_lt;
        int c_rt = rt - s->base_rt;
        int c_lx = lx - s->base_lx;
        int c_ly = ly - s->base_ly;
        int c_rx = rx - s->base_rx;
        int c_ry = ry - s->base_ry;

        if (c_lt < 0) c_lt = -c_lt;
        if (c_rt < 0) c_rt = -c_rt;
        if (c_lt > 4095) c_lt = 4095;
        if (c_rt > 4095) c_rt = 4095;

        s->buttons = buttons;
        s->lt = c_lt;
        s->rt = c_rt;
        s->lx = c_lx;
        s->ly = c_ly;
        s->rx = c_rx;
        s->ry = c_ry;
        apply_slot_state_locked(slot);
    }
    pthread_mutex_unlock(&g_lock);
}

static void handle_line(const char *line)
{
    if (json_has_type(line, "connected")) {
        int slot = -1;
        if (json_get_int(line, "slot", &slot)) on_connected(slot);
    } else if (json_has_type(line, "disconnected")) {
        int slot = -1;
        if (json_get_int(line, "slot", &slot)) on_disconnected(slot);
    } else if (json_has_type(line, "input")) {
        on_input(line);
    } else if (!g_logged_unknown_line) {
        fprintf(stderr, "[inject] ignoring unknown UDP event: %s\n", line);
        g_logged_unknown_line = 1;
    }
}

static void *udp_thread(void *arg)
{
    (void)arg;

    const char *addr = getenv("XBOX_INJECT_UDP_ADDR");
    const char *port_env = getenv("XBOX_INJECT_UDP_PORT");
    const char *gain_env = getenv("XBOX_INJECT_STICK_GAIN");
    const char *deadzone_env = getenv("XBOX_INJECT_STICK_DEADZONE");
    const char *swap_env = getenv("XBOX_INJECT_SWAP_LR");
    const char *swap_sticks_env = getenv("XBOX_INJECT_SWAP_STICKS");
    const char *axis_layout_env = getenv("XBOX_INJECT_AXIS_LAYOUT");
    const char *bind_addr = (addr && addr[0]) ? addr : "127.0.0.1";
    int port = (port_env && port_env[0]) ? atoi(port_env) : 7947;
    if (gain_env && gain_env[0]) {
        int g = atoi(gain_env);
        if (g > 0 && g <= 64) g_stick_gain = g;
    }
    if (deadzone_env && deadzone_env[0]) {
        int dz = atoi(deadzone_env);
        if (dz >= 0 && dz <= 12000) g_stick_deadzone = dz;
    }
    if (swap_env && swap_env[0]) {
        g_swap_lr = strcmp(swap_env, "0") != 0;
    }
    if (swap_sticks_env && swap_sticks_env[0]) {
        g_swap_sticks = strcmp(swap_sticks_env, "0") != 0;
    }
    if (axis_layout_env && axis_layout_env[0]) {
        if (strcmp(axis_layout_env, "sdl") == 0) {
            g_xone_axis_layout = 0;
        } else {
            g_xone_axis_layout = 1;
        }
    }
    if (port <= 0 || port > 65535) port = 7947;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "[inject] socket failed: %s\n", strerror(errno));
        return NULL;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1) {
        fprintf(stderr, "[inject] invalid bind address: %s\n", bind_addr);
        close(sock);
        return NULL;
    }

    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "[inject] bind %s:%d failed: %s\n", bind_addr, port, strerror(errno));
        close(sock);
        return NULL;
    }

    fprintf(stderr, "[inject] listening on %s:%d (stick_gain=%d deadzone=%d swap_lr=%d swap_sticks=%d axis_layout=%s)\n",
            bind_addr, port, g_stick_gain, g_stick_deadzone, g_swap_lr, g_swap_sticks,
            g_xone_axis_layout ? "xone" : "sdl");

    uint8_t buf[4096];
    while (1) {
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[inject] recv failed: %s\n", strerror(errno));
            break;
        }

        if (!g_logged_first_udp) {
            fprintf(stderr, "[inject] first UDP packet received (%zd bytes)\n", n);
            g_logged_first_udp = 1;
        }

        buf[n] = '\0';
        char *save = NULL;
        char *line = strtok_r((char *)buf, "\r\n", &save);
        while (line) {
            if (*line) handle_line(line);
            line = strtok_r(NULL, "\r\n", &save);
        }
    }

    close(sock);
    return NULL;
}

__attribute__((constructor))
static void inject_init(void)
{
    for (int i = 0; i < MAX_SLOTS; i++) g_slots[i].instance_id = -1;

    pthread_t t;
    if (pthread_create(&t, NULL, udp_thread, NULL) == 0) {
        pthread_detach(t);
        fprintf(stderr, "[inject] Ryujinx SDL injector loaded\n");
    } else {
        fprintf(stderr, "[inject] failed to start UDP thread\n");
    }
}
