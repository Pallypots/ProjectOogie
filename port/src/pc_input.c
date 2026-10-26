/* Project Oogie: controllers for the PC port.
 *
 * Replaces the runtime's Windows xbox_InputGetState (src/input/xinput_device.c,
 * renamed to xbox_InputGetState_xinput by the pad_wrapper build patch) so that
 *
 *   - Xbox 360 / Xbox One / Xbox Series pads, wired or Bluetooth, are read
 *     through XInput as before;
 *   - PlayStation 4 and 5 pads (DualShock 4, DualSense, wired or Bluetooth),
 *     Switch Pro and other pads are read through SDL2's GameController API
 *     (its HIDAPI drivers speak the Sony protocols directly). SDL2.dll is
 *     loaded at run time from beside the exe; without it only XInput pads
 *     and the keyboard work, nothing else changes;
 *   - every pad button can be rebound in settings.ini [ControllerButtons];
 *   - the game's rumble reaches the pad (the OHCI OUT packets, patch pad_rumble).
 *
 * Pads are polled on one input thread (4 ms), so the game's own thread never
 * waits on a slow driver, an empty XInput slot (a known multi-millisecond
 * call) included. Port 0 gets the first pad: XInput pads keep their XInput
 * slot, and the other pads fill the ports no XInput pad is on, in the order
 * they were connected. The keyboard and mouse merge on port 0 as before.
 *
 * OOGIE_INPUT_OLD=1 bypasses all of this and uses the runtime's function. */
#include <windows.h>
#include <math.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "xinput_xbox.h"
#include "keyboard.h"
#include "mkb_input.h"
#include "pc_settings.h"
#include "pc_input.h"
#include "pc_menu.h"

DWORD xbox_InputGetState_xinput(DWORD dwPort, XBOX_INPUT_STATE *pState);

/* ---- physical buttons, in Xbox (positional) names (PB_x in pc_input.h) -- */
static const char *const s_pb_names[PB_COUNT] = {
    "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start",
    "LS", "RS", "DUp", "DDown", "DLeft", "DRight", "Touchpad", "Misc", "Guide"
};

typedef struct {
    int      present;
    uint32_t btn;                 /* 1 << PB_x */
    BYTE     lt, rt;              /* 0..255 */
    SHORT    lx, ly, rx, ry;      /* Xbox convention: +Y is up */
} PadRaw;

/* ---- Xbox targets ------------------------------------------------------- */
enum {
    T_A, T_B, T_X, T_Y, T_BLACK, T_WHITE, T_LT, T_RT,      /* analog 0..7 */
    T_START, T_BACK, T_LS, T_RS, T_DUP, T_DDOWN, T_DLEFT, T_DRIGHT,
    T_COUNT
};
static const char *const s_t_names[T_COUNT] = {
    "A", "B", "X", "Y", "Black", "White", "LeftTrigger", "RightTrigger",
    "Start", "Back", "LeftStickClick", "RightStickClick",
    "DpadUp", "DpadDown", "DpadLeft", "DpadRight"
};
static const WORD s_t_digital[T_COUNT] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    XBOX_GAMEPAD_START, XBOX_GAMEPAD_BACK, XBOX_GAMEPAD_LEFT_THUMB, XBOX_GAMEPAD_RIGHT_THUMB,
    XBOX_GAMEPAD_DPAD_UP, XBOX_GAMEPAD_DPAD_DOWN, XBOX_GAMEPAD_DPAD_LEFT, XBOX_GAMEPAD_DPAD_RIGHT
};
/* The defaults: the original pad's layout. Black/White on LB/RB follows the
 * runtime's XInput mapping; the touchpad click also works as Back. */
static uint32_t s_bind[T_COUNT];

static void binds_default(void)
{
    s_bind[T_A] = 1u << PB_A;         s_bind[T_B] = 1u << PB_B;
    s_bind[T_X] = 1u << PB_X;         s_bind[T_Y] = 1u << PB_Y;
    s_bind[T_BLACK] = 1u << PB_LB;    s_bind[T_WHITE] = 1u << PB_RB;
    s_bind[T_LT] = 1u << PB_LT;       s_bind[T_RT] = 1u << PB_RT;
    s_bind[T_START] = 1u << PB_START; s_bind[T_BACK] = (1u << PB_BACK) | (1u << PB_TOUCH);
    s_bind[T_LS] = 1u << PB_LS;       s_bind[T_RS] = 1u << PB_RS;
    s_bind[T_DUP] = 1u << PB_DUP;     s_bind[T_DDOWN] = 1u << PB_DDOWN;
    s_bind[T_DLEFT] = 1u << PB_DLEFT; s_bind[T_DRIGHT] = 1u << PB_DRIGHT;
}

static void binds_load(void)
{
    int t;
    binds_default();
    for (t = 0; t < T_COUNT; t++) {
        const char *v = pc_settings_get("ControllerButtons", s_t_names[t]);
        char buf[160], *tok, *ctx = NULL;
        uint32_t m = 0;
        int bad = 0;
        if (!v)
            continue;
        snprintf(buf, sizeof buf, "%s", v);
        for (tok = strtok_s(buf, ", \t", &ctx); tok; tok = strtok_s(NULL, ", \t", &ctx)) {
            int b, hit = 0;
            if (!_stricmp(tok, "none")) { hit = 1; }
            for (b = 0; b < PB_COUNT && !hit; b++)
                if (!_stricmp(tok, s_pb_names[b])) { m |= 1u << b; hit = 1; }
            if (!hit) {
                fprintf(stderr, "[INPUT] settings.ini [ControllerButtons] %s: unknown button '%s'\n",
                        s_t_names[t], tok);
                bad = 1;
            }
        }
        if (!bad)
            s_bind[t] = m;
    }
}

/* ---- SDL2, loaded at run time ------------------------------------------- */
typedef struct _SDL_GameController SDL_GameController;
typedef struct _SDL_Joystick SDL_Joystick;
#define SDL_INIT_JOYSTICK        0x00000200u
#define SDL_INIT_GAMECONTROLLER  0x00002000u
#define SDL_INIT_EVENTS          0x00004000u
enum { SDLT_UNKNOWN, SDLT_XBOX360, SDLT_XBOXONE, SDLT_PS3, SDLT_PS4, SDLT_SWITCH_PRO,
       SDLT_VIRTUAL, SDLT_PS5 };
/* SDL_GameControllerButton */
enum { SB_A, SB_B, SB_X, SB_Y, SB_BACK, SB_GUIDE, SB_START, SB_LS, SB_RS, SB_LB, SB_RB,
       SB_DUP, SB_DDOWN, SB_DLEFT, SB_DRIGHT, SB_MISC1, SB_P1, SB_P2, SB_P3, SB_P4, SB_TOUCH };
/* SDL_GameControllerAxis */
enum { SA_LX, SA_LY, SA_RX, SA_RY, SA_LT, SA_RT };

static struct {
    HMODULE dll;
    int  (__cdecl *Init)(uint32_t);
    int  (__cdecl *SetHint)(const char *, const char *);
    const char *(__cdecl *GetError)(void);
    void (__cdecl *PumpEvents)(void);
    void (__cdecl *FlushEvents)(uint32_t, uint32_t);
    int  (__cdecl *NumJoysticks)(void);
    int  (__cdecl *IsGameController)(int);
    int  (__cdecl *TypeForIndex)(int);
    const char *(__cdecl *PathForIndex)(int);
    int32_t (__cdecl *DeviceInstanceID)(int);
    SDL_GameController *(__cdecl *Open)(int);
    void (__cdecl *Close)(SDL_GameController *);
    int  (__cdecl *GetAttached)(SDL_GameController *);
    SDL_Joystick *(__cdecl *GetJoystick)(SDL_GameController *);
    int32_t (__cdecl *InstanceID)(SDL_Joystick *);
    const char *(__cdecl *Name)(SDL_GameController *);
    int  (__cdecl *GetType)(SDL_GameController *);
    uint8_t (__cdecl *GetButton)(SDL_GameController *, int);
    int16_t (__cdecl *GetAxis)(SDL_GameController *, int);
    int  (__cdecl *SetLED)(SDL_GameController *, uint8_t, uint8_t, uint8_t);
    int  (__cdecl *Rumble)(SDL_GameController *, uint16_t, uint16_t, uint32_t);
} S;

static int sdl_load(void)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    char *slash = n ? strrchr(path, '\\') : NULL;
    if (!slash)
        return 0;
    snprintf(slash + 1, sizeof path - (slash + 1 - path), "SDL2.dll");
    /* The exe's own folder only, never the DLL search path. */
    S.dll = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!S.dll) {
        fprintf(stderr, "[INPUT] %s not found: PlayStation and other non-Xbox pads are off\n", path);
        return 0;
    }
#define L(f, name) if (!(*(FARPROC *)&S.f = GetProcAddress(S.dll, name))) { \
        fprintf(stderr, "[INPUT] SDL2.dll lacks %s\n", name); return 0; }
    L(Init, "SDL_Init"); L(SetHint, "SDL_SetHint"); L(GetError, "SDL_GetError");
    L(PumpEvents, "SDL_PumpEvents"); L(FlushEvents, "SDL_FlushEvents");
    L(NumJoysticks, "SDL_NumJoysticks"); L(IsGameController, "SDL_IsGameController");
    L(TypeForIndex, "SDL_GameControllerTypeForIndex");
    L(PathForIndex, "SDL_JoystickPathForIndex");
    L(DeviceInstanceID, "SDL_JoystickGetDeviceInstanceID");
    L(Open, "SDL_GameControllerOpen"); L(Close, "SDL_GameControllerClose");
    L(GetAttached, "SDL_GameControllerGetAttached");
    L(GetJoystick, "SDL_GameControllerGetJoystick"); L(InstanceID, "SDL_JoystickInstanceID");
    L(Name, "SDL_GameControllerName"); L(GetType, "SDL_GameControllerGetType");
    L(GetButton, "SDL_GameControllerGetButton"); L(GetAxis, "SDL_GameControllerGetAxis");
    L(SetLED, "SDL_GameControllerSetLED"); L(Rumble, "SDL_GameControllerRumble");
#undef L
    return 1;
}

/* ---- shared state --------------------------------------------------------- */
#define MAX_SDL 8
static CRITICAL_SECTION s_lock;
static PadRaw s_xi[XUSER_MAX_COUNT];          /* by XInput slot */
static PadRaw s_sdl[MAX_SDL];                 /* by connection order */
static SDL_GameController *s_sdl_pad[MAX_SDL];  /* input thread only */
static int32_t s_sdl_id[MAX_SDL];
static int s_sdl_type[MAX_SDL];
static WORD s_rumble[XBOX_MAX_CONTROLLERS][2];  /* wanted, by game port */
static int  s_rumble_dirty[XBOX_MAX_CONTROLLERS];
static int  s_rumble_on = 1;
static volatile LONG s_ready;                 /* 1 thread running, -1 old path */

static SHORT neg_y(int16_t v) { return (SHORT)(v == -32768 ? 32767 : -v); }

/* Stick dead zone (2026-10-25, owner on a DualSense: Jack turned on his own
 * and the pad seemed locked out): the raw axes went to the game, so a stick
 * resting a little off centre read as a small push. Radial, with Microsoft's
 * XInput values (left 7849, right 8689 of 32767), and rescaled so the full
 * range is still reached just past the dead zone. Both pad paths. */
/* square: the movement stick (2026-10-25, owner: Jack walked instead of
 * running at some angles). A modern stick moves in a circle, so a full push
 * on a diagonal gives each axis only 0.71 of its range, and a worn stick may
 * not reach the edge in some directions. The game was made for the Xbox pad,
 * whose stick reports reach the corners. So: 90% of the way out counts as all
 * the way, and the circle is stretched onto the square (each direction's
 * largest axis gets the push's length): a full push reads full in every
 * direction, a half push still reads half. */
static void stick_deadzone(SHORT *x, SHORT *y, int dz, int square)
{
    const float outer = 32767.0f * 0.90f;
    float fx = (float)*x, fy = (float)*y, m = sqrtf(fx * fx + fy * fy), len, k;
    if (m <= (float)dz) { *x = 0; *y = 0; return; }
    len = (m >= outer ? 1.0f : (m - (float)dz) / (outer - (float)dz)) * 32767.0f;
    k = len / m;
    if (square) {
        float ax = fx < 0 ? -fx : fx, ay = fy < 0 ? -fy : fy, big = ax > ay ? ax : ay;
        k = len / big;                 /* the larger axis gets the full length */
    }
    fx *= k; fy *= k;
    *x = (SHORT)(fx > 32767.0f ? 32767 : fx < -32768.0f ? -32768 : (int)fx);
    *y = (SHORT)(fy > 32767.0f ? 32767 : fy < -32768.0f ? -32768 : (int)fy);
}

static void sdl_read(int i, PadRaw *r)
{
    static const int map[][2] = {
        { SB_A, PB_A }, { SB_B, PB_B }, { SB_X, PB_X }, { SB_Y, PB_Y },
        { SB_LB, PB_LB }, { SB_RB, PB_RB }, { SB_BACK, PB_BACK }, { SB_START, PB_START },
        { SB_LS, PB_LS }, { SB_RS, PB_RS }, { SB_DUP, PB_DUP }, { SB_DDOWN, PB_DDOWN },
        { SB_DLEFT, PB_DLEFT }, { SB_DRIGHT, PB_DRIGHT }, { SB_TOUCH, PB_TOUCH },
        { SB_MISC1, PB_MISC }, { SB_GUIDE, PB_GUIDE },
    };
    SDL_GameController *c = s_sdl_pad[i];
    int k, lt, rt;
    memset(r, 0, sizeof *r);
    r->present = 1;
    for (k = 0; k < (int)(sizeof map / sizeof map[0]); k++)
        if (S.GetButton(c, map[k][0]))
            r->btn |= 1u << map[k][1];
    lt = S.GetAxis(c, SA_LT); rt = S.GetAxis(c, SA_RT);
    r->lt = (BYTE)(lt <= 0 ? 0 : lt * 255 / 32767);
    r->rt = (BYTE)(rt <= 0 ? 0 : rt * 255 / 32767);
    if (r->lt > 30) r->btn |= 1u << PB_LT;
    if (r->rt > 30) r->btn |= 1u << PB_RT;
    r->lx = S.GetAxis(c, SA_LX); r->ly = neg_y(S.GetAxis(c, SA_LY));
    r->rx = S.GetAxis(c, SA_RX); r->ry = neg_y(S.GetAxis(c, SA_RY));
    stick_deadzone(&r->lx, &r->ly, 7849, 1);
    stick_deadzone(&r->rx, &r->ry, 8689, 0);
}

typedef DWORD (WINAPI *XInputGetStateExFn)(DWORD, XINPUT_STATE *);
static XInputGetStateExFn s_xi_get_ex;   /* reports the Guide button (0x400) */

static void xi_read(const XINPUT_STATE *x, PadRaw *r)
{
    static const WORD map[][2] = {
        { XINPUT_GAMEPAD_A, PB_A }, { XINPUT_GAMEPAD_B, PB_B },
        { XINPUT_GAMEPAD_X, PB_X }, { XINPUT_GAMEPAD_Y, PB_Y },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, PB_LB }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, PB_RB },
        { XINPUT_GAMEPAD_BACK, PB_BACK }, { XINPUT_GAMEPAD_START, PB_START },
        { XINPUT_GAMEPAD_LEFT_THUMB, PB_LS }, { XINPUT_GAMEPAD_RIGHT_THUMB, PB_RS },
        { XINPUT_GAMEPAD_DPAD_UP, PB_DUP }, { XINPUT_GAMEPAD_DPAD_DOWN, PB_DDOWN },
        { XINPUT_GAMEPAD_DPAD_LEFT, PB_DLEFT }, { XINPUT_GAMEPAD_DPAD_RIGHT, PB_DRIGHT },
    };
    int k;
    memset(r, 0, sizeof *r);
    r->present = 1;
    for (k = 0; k < (int)(sizeof map / sizeof map[0]); k++)
        if (x->Gamepad.wButtons & map[k][0])
            r->btn |= 1u << map[k][1];
    if (x->Gamepad.wButtons & 0x0400) r->btn |= 1u << PB_GUIDE;
    r->lt = x->Gamepad.bLeftTrigger; r->rt = x->Gamepad.bRightTrigger;
    if (r->lt > 30) r->btn |= 1u << PB_LT;
    if (r->rt > 30) r->btn |= 1u << PB_RT;
    r->lx = x->Gamepad.sThumbLX; r->ly = x->Gamepad.sThumbLY;
    r->rx = x->Gamepad.sThumbRX; r->ry = x->Gamepad.sThumbRY;
    stick_deadzone(&r->lx, &r->ly, 7849, 1);
    stick_deadzone(&r->rx, &r->ry, 8689, 0);
}

/* Which device a game port reads: >= 0 an XInput slot, <= -2 SDL pad
 * (-2 - index), -1 none. Caller holds s_lock. */
static int port_device(DWORD port)
{
    int p, k = 0, i;
    if (s_xi[port].present)
        return (int)port;
    for (p = 0; p < (int)port; p++)
        if (!s_xi[p].present)
            k++;                      /* SDL pads taken by earlier ports */
    for (i = 0; i < MAX_SDL; i++)
        if (s_sdl[i].present && k-- == 0)
            return -2 - i;
    return -1;
}

static void sdl_scan(void)
{
    int n = S.NumJoysticks(), j, i;
    for (i = 0; i < MAX_SDL; i++)
        if (s_sdl_pad[i] && !S.GetAttached(s_sdl_pad[i])) {
            fprintf(stderr, "[INPUT] pad %d disconnected\n", i);
            S.Close(s_sdl_pad[i]);
            s_sdl_pad[i] = NULL;
            EnterCriticalSection(&s_lock);
            s_sdl[i].present = 0;
            LeaveCriticalSection(&s_lock);
        }
    for (j = 0; j < n; j++) {
        int32_t id = S.DeviceInstanceID(j);
        int t, have = 0, slot = -1;
        if (!S.IsGameController(j))
            continue;
        t = S.TypeForIndex(j);
        /* XInput pads are read through XInput already; SDL would see them
         * twice. SDL names those "XInput#n", and an XInput-capable HID
         * device has "IG_" in its path. A pad that only looks like an Xbox
         * pad (a third-party Bluetooth one) has neither and is kept. */
        {
            const char *path = S.PathForIndex(j);
            char up[512];
            size_t q;
            snprintf(up, sizeof up, "%s", path ? path : "");
            for (q = 0; up[q]; q++) up[q] = (char)toupper((unsigned char)up[q]);
            if (t == SDLT_VIRTUAL || !strncmp(up, "XINPUT", 6) || strstr(up, "IG_")
                || (!up[0] && (t == SDLT_XBOX360 || t == SDLT_XBOXONE))) {
                static int32_t seen[16];
                static int nseen;
                int k, old = 0;
                for (k = 0; k < nseen; k++) if (seen[k] == id) old = 1;
                if (!old && nseen < 16) {
                    seen[nseen++] = id;
                    fprintf(stderr, "[INPUT] SDL sees an XInput pad (type %d, %s): read through XInput\n",
                            t, path ? path : "no path");
                }
                continue;
            }
        }
        for (i = 0; i < MAX_SDL; i++) {
            if (s_sdl_pad[i] && s_sdl_id[i] == id) have = 1;
            if (!s_sdl_pad[i] && slot < 0) slot = i;
        }
        if (have || slot < 0)
            continue;
        {
            const char *path = S.PathForIndex(j);
            fprintf(stderr, "[INPUT] opening pad %d (%s)\n", slot, path ? path : "no path");
        }
        s_sdl_pad[slot] = S.Open(j);
        if (!s_sdl_pad[slot])
            continue;
        s_sdl_id[slot] = S.InstanceID(S.GetJoystick(s_sdl_pad[slot]));
        s_sdl_type[slot] = S.GetType(s_sdl_pad[slot]);
        fprintf(stderr, "[INPUT] pad %d connected: %s (type %d)\n", slot,
                S.Name(s_sdl_pad[slot]) ? S.Name(s_sdl_pad[slot]) : "?", s_sdl_type[slot]);
        if (s_sdl_type[slot] == SDLT_PS4 || s_sdl_type[slot] == SDLT_PS5) {
            const char *lb = pc_settings_get("Controller", "lightbar");
            if (lb && *lb) {
                unsigned long c = strtoul(lb, NULL, 16);
                S.SetLED(s_sdl_pad[slot], (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c);
            }
        }
    }
}

static DWORD WINAPI input_thread(LPVOID arg)
{
    int use_sdl = (int)(INT_PTR)arg;
    DWORD next_empty_poll[XUSER_MAX_COUNT] = { 0 };
    DWORD last_scan = 0, rumble_sent[XBOX_MAX_CONTROLLERS] = { 0 };
    int p;

    if (use_sdl) {
        /* Full DualShock 4 / DualSense reports over Bluetooth, which rumble
         * needs too; SDL leaves the pad in that mode until it reconnects. */
        S.SetHint("SDL_JOYSTICK_HIDAPI_PS4_RUMBLE", "1");
        S.SetHint("SDL_JOYSTICK_HIDAPI_PS5_RUMBLE", "1");
        S.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
        if (S.Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
            fprintf(stderr, "[INPUT] SDL_Init failed: %s\n", S.GetError());
            use_sdl = 0;
        }
    }
    InterlockedExchange(&s_ready, 1);
    for (;;) {
        DWORD now = GetTickCount();
        PadRaw xi[XUSER_MAX_COUNT], sd[MAX_SDL];
        WORD rum[XBOX_MAX_CONTROLLERS][2];
        int dirty[XBOX_MAX_CONTROLLERS];

        EnterCriticalSection(&s_lock);
        memcpy(xi, s_xi, sizeof xi);
        LeaveCriticalSection(&s_lock);
        /* An empty XInput slot costs a slow call: look once a second. */
        for (p = 0; p < XUSER_MAX_COUNT; p++) {
            XINPUT_STATE x;
            if (!xi[p].present && (LONG)(now - next_empty_poll[p]) < 0)
                continue;
            if ((s_xi_get_ex ? s_xi_get_ex((DWORD)p, &x) : XInputGetState((DWORD)p, &x)) == ERROR_SUCCESS) {
                if (!xi[p].present)
                    fprintf(stderr, "[INPUT] Xbox pad on XInput slot %d connected\n", p);
                xi_read(&x, &xi[p]);
            } else {
                if (xi[p].present)
                    fprintf(stderr, "[INPUT] Xbox pad on XInput slot %d disconnected\n", p);
                xi[p].present = 0;
                next_empty_poll[p] = now + 1000;
            }
        }
        if (use_sdl) {
            S.PumpEvents();
            S.FlushEvents(0, 0xFFFFu);
            if (now - last_scan >= 500) { sdl_scan(); last_scan = now; }
            for (p = 0; p < MAX_SDL; p++)
                if (s_sdl_pad[p]) sdl_read(p, &sd[p]); else sd[p].present = 0;
        } else {
            memset(sd, 0, sizeof sd);
        }

        EnterCriticalSection(&s_lock);
        memcpy(s_xi, xi, sizeof xi);
        memcpy(s_sdl, sd, sizeof sd);
        memcpy(rum, s_rumble, sizeof rum);
        memcpy(dirty, s_rumble_dirty, sizeof dirty);
        memset(s_rumble_dirty, 0, sizeof s_rumble_dirty);
        LeaveCriticalSection(&s_lock);

        {
            /* Settings menu: Back+Start together, or the Guide / PS button,
             * on any pad opens and closes it (pc_menu.c). */
            static int chord_was;
            uint32_t all = pc_input_raw_buttons();
            int chord = ((all & (1u << PB_BACK)) && (all & (1u << PB_START))) || (all & (1u << PB_GUIDE));
            if (chord && !chord_was)
                pc_menu_request_toggle();
            chord_was = chord;
        }

        /* Rumble, on this thread so a slow Bluetooth write never holds the
         * game. SDL rumble lasts for the duration given, so it is renewed. */
        for (p = 0; p < XBOX_MAX_CONTROLLERS; p++) {
            int dev, renew;
            renew = (rum[p][0] || rum[p][1]) && now - rumble_sent[p] >= 400;
            if (!dirty[p] && !renew)
                continue;
            EnterCriticalSection(&s_lock);
            dev = port_device((DWORD)p);
            LeaveCriticalSection(&s_lock);
            if (dev >= 0) {
                XINPUT_VIBRATION v;
                if (!dirty[p]) continue;          /* XInput keeps its level */
                v.wLeftMotorSpeed = rum[p][0]; v.wRightMotorSpeed = rum[p][1];
                XInputSetState((DWORD)dev, &v);
            } else if (dev <= -2 && use_sdl && s_sdl_pad[-2 - dev]) {
                S.Rumble(s_sdl_pad[-2 - dev], rum[p][0], rum[p][1], 1000);
            }
            rumble_sent[p] = now;
        }
        Sleep(4);
    }
    return 0;
}

static void input_init(void)
{
    static volatile LONG once;
    const char *e;
    int use_sdl;

    if (InterlockedCompareExchange(&once, 1, 0) != 0) {
        while (s_ready == 0) Sleep(1);
        return;
    }
    e = getenv("OOGIE_INPUT_OLD");
    if (e && *e && *e != '0') {
        fprintf(stderr, "[INPUT] OOGIE_INPUT_OLD: runtime XInput path\n");
        InterlockedExchange(&s_ready, -1);
        return;
    }
    InitializeCriticalSection(&s_lock);
    {
        HMODULE xi = LoadLibraryA("xinput1_4.dll");
        if (xi) s_xi_get_ex = (XInputGetStateExFn)(void *)GetProcAddress(xi, (LPCSTR)100);
    }
    binds_load();
    s_rumble_on = g_pc.rumble != 0;
    use_sdl = g_pc.other_pads && sdl_load();
    fprintf(stderr, "[INPUT] Xbox pads via XInput; other pads via SDL2 %s; rumble %s\n",
            use_sdl ? "on" : "off", s_rumble_on ? "on" : "off");
    CloseHandle(CreateThread(NULL, 0, input_thread, (LPVOID)(INT_PTR)use_sdl, 0, NULL));
    while (s_ready == 0) Sleep(1);
    Sleep(20);                       /* first XInput poll and SDL scan */
}

/* Keyboard on top of a pad (the runtime's keyboard_merge, which is static). */
static void keyboard_merge(XBOX_INPUT_STATE *s)
{
    XBOX_INPUT_STATE kb;
    int i;
    if (!xbox_InputKeyboardPoll(&kb))
        return;
    s->Gamepad.wButtons |= kb.Gamepad.wButtons;
    for (i = 0; i < 8; i++)
        if (kb.Gamepad.bAnalogButtons[i] > s->Gamepad.bAnalogButtons[i])
            s->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
    if (kb.Gamepad.sThumbLX) s->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
    if (kb.Gamepad.sThumbLY) s->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
    if (kb.Gamepad.sThumbRX) s->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
    if (kb.Gamepad.sThumbRY) s->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
    s->dwPacketNumber = kb.dwPacketNumber;
}

/* Skip Intro Logos (Display page): while the company logos film (mv_00.bik)
 * plays, Start is pressed for the player on pad 1 (on 250 ms, off 250 ms;
 * the game ignores Start for the film's first seconds). The film's start and
 * end are seen in the files the game opens (the runtime's path hook): the
 * next film or level file ends it, and 60 s at most. */
static volatile LONG s_intro;
static volatile DWORD s_intro_t0;
void pc_input_path_watch(const char *xbox_path)
{
    if (!xbox_path) return;
    if (strstr(xbox_path, "mv_00.bik") || strstr(xbox_path, "MV_00.BIK")) {
        s_intro_t0 = GetTickCount();
        InterlockedExchange(&s_intro, 1);
        if (g_pc.skip_intro) { fprintf(stderr, "[INPUT] Skip Intro Logos: pressing Start through the logos film\n"); fflush(stderr); }
    } else if (s_intro && (strstr(xbox_path, ".bik") || strstr(xbox_path, ".lz"))) {
        InterlockedExchange(&s_intro, 0);
    }
}

static int intro_skip_press(void)
{
    if (!s_intro || !g_pc.skip_intro) return 0;
    if (GetTickCount() - s_intro_t0 > 60000) { InterlockedExchange(&s_intro, 0); return 0; }
    return ((GetTickCount() - s_intro_t0) / 250) & 1;
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    static DWORD packet[XBOX_MAX_CONTROLLERS];
    static XBOX_GAMEPAD last[XBOX_MAX_CONTROLLERS];
    PadRaw r;
    int dev, t, kb = 0;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (s_ready <= 0) {
        input_init();
        if (s_ready < 0)
            return xbox_InputGetState_xinput(dwPort, pState);
    }

    EnterCriticalSection(&s_lock);
    dev = port_device(dwPort);
    if (dev >= 0) r = s_xi[dev];
    else if (dev <= -2) r = s_sdl[-2 - dev];
    else memset(&r, 0, sizeof r);
    LeaveCriticalSection(&s_lock);

    {
        /* While the settings menu is open the game sees a pad at rest, and
         * after it closes, until every button is let go (the B or Start
         * that closed it must not reach the game). */
        static int held_off[XBOX_MAX_CONTROLLERS];
        static DWORD held_since[XBOX_MAX_CONTROLLERS];
        if (pc_menu_is_open()) { held_off[dwPort] = 1; held_since[dwPort] = GetTickCount(); }
        else if (held_off[dwPort] && ((!r.btn && !pc_menu_keys_held())
                                      || GetTickCount() - held_since[dwPort] > 2000))
            held_off[dwPort] = 0;       /* released, or 2 s after the menu closed whatever is held */
        if (held_off[dwPort]) {
            memset(pState, 0, sizeof *pState);
            pState->dwPacketNumber = packet[dwPort];
            return (r.present || dwPort == 0) ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
        }
    }

    memset(pState, 0, sizeof *pState);
    if (r.present) {
        XBOX_GAMEPAD *g = &pState->Gamepad;
        for (t = 0; t < T_COUNT; t++) {
            uint32_t m = s_bind[t];
            if (!(r.btn & m))
                continue;
            if (t <= T_RT) {
                /* Analog on the console: a trigger keeps its pressure. */
                BYTE v = 255;
                if (m == (1u << PB_LT)) v = r.lt;
                else if (m == (1u << PB_RT)) v = r.rt;
                g->bAnalogButtons[t] = v;
            } else {
                g->wButtons |= s_t_digital[t];
            }
        }
        /* An analog trigger bound to its own target reads its full range
         * even below the digital threshold. */
        if (s_bind[T_LT] == (1u << PB_LT)) g->bAnalogButtons[T_LT] = r.lt;
        if (s_bind[T_RT] == (1u << PB_RT)) g->bAnalogButtons[T_RT] = r.rt;
        g->sThumbLX = r.lx; g->sThumbLY = r.ly;
        g->sThumbRX = r.rx; g->sThumbRY = r.ry;
        if (memcmp(g, &last[dwPort], sizeof *g)) {
            last[dwPort] = *g;
            packet[dwPort]++;
        }
        pState->dwPacketNumber = packet[dwPort];
    }

    if (dwPort == 0) {
        if (r.present) {
            keyboard_merge(pState);
        } else {
            kb = xbox_InputKeyboardPoll(pState);
        }
        /* Mouse+keyboard on top: the mouse owns the aim stick while moving. */
        if (mkb_merge(dwPort, pState))
            kb = 1;
        {
            /* Skip Intro Logos: Start, on and off */
            static int was;
            int on = intro_skip_press();
            if (on) { pState->Gamepad.wButtons |= 0x0010; kb = 1; }   /* XINPUT_GAMEPAD_START */
            if (on != was) { was = on; packet[dwPort]++; }
            if (on || was) pState->dwPacketNumber = packet[dwPort];
        }
        {
            /* Auto-Repeat Held Button (Controls page, accessibility): while
             * the chosen face button (1 A, 2 B, 3 X, 4 Y: analog slots 0..3)
             * is held, it reads pressed 50 ms and released 50 ms, about ten
             * presses a second, so button-mashing prompts need only a hold. */
            static int phase_was;
            int b = g_pc.auto_repeat - 1;
            if (b >= 0 && b <= 3 && pState->Gamepad.bAnalogButtons[b] > 30) {
                int phase = (GetTickCount() / 50) & 1;
                if (phase) pState->Gamepad.bAnalogButtons[b] = 0;
                if (phase != phase_was) { phase_was = phase; packet[dwPort]++; }
                pState->dwPacketNumber = packet[dwPort];
            }
        }
        if (!r.present && (kb || mkb_enabled()))
            return ERROR_SUCCESS;
    }
    return r.present ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

/* The game's rumble packet (USB OUT on the pad's endpoint 2), from the
 * pad_rumble patch to src/usb/ohci.c. */
void oogie_pad_rumble(int port, const uint8_t *p, int len)
{
    WORD lo, hi;
    if (port < 0 || port >= XBOX_MAX_CONTROLLERS || len < 6 || !p || s_ready <= 0)
        return;
    /* XInput's rumble packet: 00 06 then the two motor speeds. */
    if (p[0] != 0 || p[1] < 6)
        return;
    lo = (WORD)(p[2] | (p[3] << 8));
    hi = (WORD)(p[4] | (p[5] << 8));
    if (!s_rumble_on || g_pc.rumble <= 0) {
        lo = hi = 0;
    } else if (g_pc.rumble >= 2) {              /* strong: half as much again */
        lo = (WORD)(lo + lo / 2 < lo ? 0xFFFF : (lo + lo / 2 > 0xFFFF ? 0xFFFF : lo + lo / 2));
        hi = (WORD)(hi + hi / 2 > 0xFFFF ? 0xFFFF : hi + hi / 2);
    }
    EnterCriticalSection(&s_lock);
    if (s_rumble[port][0] != lo || s_rumble[port][1] != hi) {
        s_rumble[port][0] = lo; s_rumble[port][1] = hi;
        s_rumble_dirty[port] = 1;
    }
    LeaveCriticalSection(&s_lock);
}

/* ---- for the settings menu (pc_input.h) ---------------------------------- */
/* The left stick as the menu's d-pad (2026-10-25, owner on a DualSense: the
 * menu skipped rows). A stick held near the old single threshold (20000)
 * flickered across it, and every flicker was a new press; a diagonal pressed
 * two directions. Now: only the dominant axis, pressed past 24000 and let go
 * below 12000. State per device, updated under s_lock. */
static uint32_t s_xi_dir[XUSER_MAX_COUNT], s_sdl_dir[MAX_SDL];
static uint32_t stick_dirs(const PadRaw *r, uint32_t *held)
{
    int ax = r->lx < 0 ? -r->lx : r->lx, ay = r->ly < 0 ? -r->ly : r->ly;
    uint32_t want = 0;
    if (ax >= ay) { if (ax > 24000 || (ax > 12000 && (*held & ((1u << PB_DLEFT) | (1u << PB_DRIGHT)))))
                        want = r->lx < 0 ? 1u << PB_DLEFT : 1u << PB_DRIGHT; }
    else          { if (ay > 24000 || (ay > 12000 && (*held & ((1u << PB_DUP) | (1u << PB_DDOWN)))))
                        want = r->ly > 0 ? 1u << PB_DUP : 1u << PB_DDOWN; }
    *held = want;
    return want;
}
uint32_t pc_input_raw_buttons(void)
{
    uint32_t m = 0;
    int i;
    if (s_ready <= 0)
        return 0;
    EnterCriticalSection(&s_lock);
    for (i = 0; i < XUSER_MAX_COUNT; i++) {
        const PadRaw *r = &s_xi[i];
        if (!r->present) continue;
        m |= r->btn;
        m |= stick_dirs(r, &s_xi_dir[i]);
    }
    for (i = 0; i < MAX_SDL; i++) {
        const PadRaw *r = &s_sdl[i];
        if (!r->present) continue;
        m |= r->btn;
        m |= stick_dirs(r, &s_sdl_dir[i]);
    }
    LeaveCriticalSection(&s_lock);
    return m;
}

const char *pc_input_button_name(int pb)
{
    return (pb >= 0 && pb < PB_COUNT) ? s_pb_names[pb] : "?";
}

void pc_input_binds_reload(void)
{
    binds_load();
}

int pc_input_pad_list(char lines[][96], int max)
{
    int n = 0, i;
    if (s_ready <= 0)
        return 0;
    for (i = 0; i < XUSER_MAX_COUNT && n < max; i++)
        if (s_xi[i].present)
            snprintf(lines[n++], 96, "Xbox controller (XInput %d)", i + 1);
    for (i = 0; i < MAX_SDL && n < max; i++)
        if (s_sdl[i].present && s_sdl_pad[i]) {
            const char *nm = S.Name ? S.Name(s_sdl_pad[i]) : NULL;
            snprintf(lines[n++], 96, "%s", nm ? nm : "Controller");
        }
    return n;
}
