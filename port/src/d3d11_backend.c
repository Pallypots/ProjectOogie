/*
 * Project Oogie: Direct3D 11 render back end for the NV2A pushbuffer executor.
 *
 * The runtime's executor (third_party/xboxrecomp/src/kernel/nv2a_pb_exec.c)
 * decodes what the game asks the GPU to do and, by default, rasterises it on
 * the CPU into guest memory. That runs Oogie's Revenge at 1-2 fps in a level.
 * With OOGIE_D3D11=1 this file is registered as the executor's back end
 * (nv2a_backend.h): the executor still transforms vertices (fixed function and
 * vertex programs) and hands over screen-space triangles, clears and flips,
 * and the GPU does the rasterising, texturing, blending and depth testing.
 *
 * Design:
 *  - Every guest colour surface the game draws into becomes a D3D11 render
 *    target, keyed by its physical address. When a batch samples a texture at
 *    the address of one of those surfaces (the game's glow targets, its
 *    previous-frame feedback, its swizzled 128x128 targets), the render
 *    target is sampled directly. Swizzling is a memory layout and does not
 *    exist on the GPU side, so swizzled targets need no special handling.
 *  - Other textures are decoded with nv2a_backend_decode_texture (the
 *    executor's own decoder) into B8G8R8A8 and cached; a cheap hash of the
 *    guest bytes, checked once per frame per texture, decides re-upload.
 *  - Depth/stencil: one D24S8 buffer per (zeta address, size).
 *  - Present: the largest surface drawn since the last flip is the frame
 *    (same rule as the executor's note_drawn), stretched to the window.
 *  - Screenshots for the test scripts: while the file named by
 *    OOGIE_SHOT_FLAG exists, each presented frame is written as
 *    <OOGIE_SHOT_PREFIX>NNNNN.bmp (the CPU path's RECOMP_FB_DUMP* would
 *    dump guest memory, which this back end no longer writes).
 *  - Keyboard: the window forwards keys to the runtime's keyboard pad
 *    (RECOMP_KEYBOARD=1), as the runtime's own framebuffer window does.
 *
 *  - Register combiners: the pixel shader evaluates the NV2A combiner program
 *    (up to 8 stages plus the final combiner) with the rules of the runtime's
 *    nv2a_combiner.c, from the state and per-vertex data the build-time
 *    patches backend_extra_stages / backend_vertex_extra_* expose: specular,
 *    fog factor, texture stages 1-3 and their coordinates. Oogie stores
 *    colours at half intensity (0x7F = 1.0, a PS2 habit) and doubles them in
 *    the combiners; "texture * diffuse" drew everything at half brightness
 *    (KNOWN_ISSUES #36). Before the title programs the combiners, colour =
 *    texture * diffuse. Bink's YUV draw keeps its dedicated path.
 *  - Vertex programs on the GPU (oogie_gpu_vsh.h, patch backend_gpu_vsh):
 *    vertex-program batches arrive untransformed and each NV2A program runs
 *    as a generated HLSL vertex shader (be_draw_vsh). The CPU interpreter was
 *    the level's bottleneck, and the game steps once per frame, so a slow
 *    frame was slow motion (KNOWN_ISSUES #37). OOGIE_GPU_VSH=0 turns it off.
 *
 * Limits: bump-env, dot-product and cube texture stage modes are
 * approximated (as in the runtime's CPU path). Everything here runs on the
 * executor's one thread, so there is no locking.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <dxgi1_6.h>   /* IDXGIFactory6: pick the high-performance GPU */
#include <d3dcompiler.h>
#include <mmsystem.h>
#include <psapi.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pc_settings.h"
#include "nv2a_backend.h"
#include "nv2a_combiner.h"
#include "oogie_gpu_vsh.h"

void xbox_InputKeySet(int vk, int down);
void xbox_InputKeysClear(void);
ptrdiff_t xbox_GetMemoryOffset(void);   /* xbox_memory_layout.h */
/* port/patches/backend_extra_stages */
int oogie_backend_stage_texture(int stage, Nv2aTexture *out);
int oogie_backend_combiner_stages(void);
int oogie_backend_combiner(Nv2aCombiner *out, float fog_rgb[3]);
const uint32_t (*oogie_vsh_program(uint32_t *start))[4];   /* patch vsh_access */
/* port/patches/backend_vertex_extra_*: one per Nv2aVertex of the batch.
 * Same layout as the patch's struct. */
typedef struct {
    float spec[4];
    float fog, q0;
    float tc[3][4];
} OogieVertexExtra;
const OogieVertexExtra *oogie_backend_vertex_extra(void);

/* The pixel shader's constants (cbuffer C), 16-byte rows as HLSL packs them. */
typedef struct {
    float rt[4], tx[4], at[4];
    uint32_t rcs[4];            /* use combiners, stages, control >> 8, stage program */
    uint32_t rcw[8][4];         /* colour ICW, alpha ICW, colour OCW, alpha OCW */
    float k0[8][4], k1[8][4];   /* per-stage constants c0, c1 */
    uint32_t fin[4];            /* final combiner words 0, 1 */
    float fk0[4], fk1[4], fogc[4];
} ShaderConsts;
static ShaderConsts s_consts;
/* GPU vertex programs: first VIN element of the current draw (rt.w). */
static float s_vin_base;

/* ── device and window ─────────────────────────────────────────── */

static HWND s_hwnd;
static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static D3D_FEATURE_LEVEL s_feature_level;   /* what the device got (COMPATIBILITY.md) */
static int s_is_warp, s_debug_layer;
static IDXGISwapChain *s_swap;
static ID3D11RenderTargetView *s_back_rtv;
static UINT s_gpu_vendor;   /* the adapter's PCI vendor (render resolution Auto) */
static int s_failed, s_ready;
static uint32_t s_frame;               /* flips so far */
static volatile LONG s_closed;
static volatile LONG s_shot_req;            /* F12 pressed: save the next shown picture */

static ID3D11VertexShader *s_vs, *s_blit_vs;
static ID3D11PixelShader *s_ps, *s_blit_ps, *s_fill_ps, *s_aa_ps, *s_sharp_ps, *s_cb_ps;
static ID3D11InputLayout *s_layout;
static ID3D11Buffer *s_cb;
static ID3D11SamplerState *s_samp[4];  /* [linear][clamp] */
static ID3D11SamplerState *s_samp_blit; /* presenting: plain linear, clamp */

/* Texture samplers [linear][clamp]. With anisotropic filtering on (settings
 * menu, Graphics) the linear ones filter anisotropically. */
static void apply_samplers(int aniso)
{
    int i;
    for (i = 0; i < 4; i++) {
        D3D11_SAMPLER_DESC ss;
        ID3D11SamplerState *n = NULL;
        memset(&ss, 0, sizeof ss);
        ss.Filter = (i & 1) ? (aniso > 1 ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR)
                            : D3D11_FILTER_MIN_MAG_MIP_POINT;
        ss.MaxAnisotropy = aniso > 1 ? (UINT)aniso : 1;
        ss.AddressU = ss.AddressV = ss.AddressW =
            (i & 2) ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
        ss.MaxLOD = D3D11_FLOAT32_MAX;
        if (SUCCEEDED(ID3D11Device_CreateSamplerState(s_dev, &ss, &n)) && n) {
            if (s_samp[i]) ID3D11SamplerState_Release(s_samp[i]);
            s_samp[i] = n;
        }
    }
}

/* 16:9 widescreen (recomp_manual.c): the game set up a widened camera in
 * the last half second, so this is a 3D scene drawn for 16:9. */
int oogie_widescreen_active(void);
/* Whether a frame is a widescreen one: it drew 3D (a vertex program longer
 * than the 3-instruction 2D pass-through) into a frame buffer. Menus, film
 * and loading screens are pure 2D and stay 4:3. */
static int s_has3d_cur, s_has3d_prev, s_shown_wide;
static int wide_frame(void) { return g_pc.aspect == 1 && s_has3d_cur; }
/* The shape the window shows a widescreen frame at: the window's own, so the
 * picture fills it edge to edge (owner 2026-10-18: no black bars). */
static volatile float s_wide_ar = 16.0f / 9.0f;
float oogie_wide_aspect(void) { return s_wide_ar; }
static int wide_2d(void) { return g_pc.aspect == 1 && (s_has3d_cur || s_has3d_prev); }
static ID3D11RasterizerState *s_rs;
static ID3D11Texture2D *s_white;
static ID3D11ShaderResourceView *s_white_srv;

int xbox_FramebufferKeyDown(int vk);   /* src/input/keyboard.c */
#include "pc_settings.h"
#include "pc_menu.h"
#include "pc_shot.h"
#include <ctype.h>

/* Once per frame: a key the runtime thinks is held must be physically down,
 * and nothing is held while another window is in front. Key messages alone
 * can miss a release -- an overlay (the Xbox Game Bar while recording) can
 * take the keyboard without the game window seeing WM_KILLFOCUS, and the
 * WM_KEYUP then goes to the overlay. The game then saw the attack button held
 * for ever: Jack stuck charging the Soul Robber, and menus that wait for every
 * button to be released ignored all input (owner's session 2026-10-17). */
/* Owner's testing layout (2026-10-17), until the options menu has rebinding:
 * the arrow keys are the face buttons in the pad's diamond -- Down A, Right B,
 * Left X, Up Y -- and the d-pad moves to T F G H (up left down right). Z X C V
 * still work. OOGIE_ARROW_FACE=0 restores the runtime's map (arrows = d-pad).
 * Physical keys are tracked here and the runtime sees the key each stands
 * for, held while any key mapped to it is held. */
static unsigned char s_phys[256];       /* physical keys held (from messages) */
static int s_arrow_face = -1;
/* settings.ini [Keyboard] (pc_settings.c), 2026-10-18: physical key -> the
 * key the runtime's map (src/input/keyboard.c) reads for that Xbox control.
 * 0 = unbound. The defaults written to settings.ini are the layout above. */
static unsigned char s_keymap[256];

static const struct { const char *name; int vk; } s_key_targets[] = {
    { "A", 'Z' }, { "B", 'X' }, { "X", 'C' }, { "Y", 'V' },
    { "Black", 'E' }, { "White", 'Q' }, { "LeftTrigger", '1' }, { "RightTrigger", '3' },
    { "Start", VK_RETURN }, { "Back", VK_BACK },
    { "LeftStickClick", VK_SHIFT }, { "RightStickClick", VK_CONTROL },
    { "DpadUp", VK_UP }, { "DpadDown", VK_DOWN }, { "DpadLeft", VK_LEFT }, { "DpadRight", VK_RIGHT },
    { "LeftStickUp", 'W' }, { "LeftStickDown", 'S' }, { "LeftStickLeft", 'A' }, { "LeftStickRight", 'D' },
    { "RightStickUp", 'I' }, { "RightStickDown", 'K' }, { "RightStickLeft", 'J' }, { "RightStickRight", 'L' },
};

static int key_by_name(const char *n)
{
    static const struct { const char *name; int vk; } named[] = {
        { "Up", VK_UP }, { "Down", VK_DOWN }, { "Left", VK_LEFT }, { "Right", VK_RIGHT },
        { "Enter", VK_RETURN }, { "Return", VK_RETURN }, { "Backspace", VK_BACK },
        { "Space", VK_SPACE }, { "Tab", VK_TAB }, { "Shift", VK_SHIFT }, { "Ctrl", VK_CONTROL },
        { "Control", VK_CONTROL }, { "Alt", VK_MENU }, { "Escape", VK_ESCAPE }, { "Esc", VK_ESCAPE },
        { "Insert", VK_INSERT }, { "Delete", VK_DELETE }, { "Home", VK_HOME }, { "End", VK_END },
        { "PageUp", VK_PRIOR }, { "PageDown", VK_NEXT },
        { "Comma", VK_OEM_COMMA }, { "Period", VK_OEM_PERIOD }, { "Semicolon", VK_OEM_1 },
        { "Slash", VK_OEM_2 }, { "Quote", VK_OEM_7 }, { "LeftBracket", VK_OEM_4 },
        { "RightBracket", VK_OEM_6 }, { "Minus", VK_OEM_MINUS }, { "Equals", VK_OEM_PLUS },
    };
    int i;
    if (n[0] && !n[1]) {
        int c = toupper((unsigned char)n[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    if ((n[0] == 'F' || n[0] == 'f') && atoi(n + 1) >= 1 && atoi(n + 1) <= 12)
        return VK_F1 + atoi(n + 1) - 1;
    if (!_strnicmp(n, "Numpad", 6) && n[6] >= '0' && n[6] <= '9' && !n[7])
        return VK_NUMPAD0 + (n[6] - '0');
    for (i = 0; i < (int)(sizeof named / sizeof named[0]); i++)
        if (!_stricmp(n, named[i].name)) return named[i].vk;
    return 0;
}

static void keymap_load(void)
{
    size_t t;
    if (s_arrow_face < 0) {
        const char *e = getenv("OOGIE_ARROW_FACE");
        s_arrow_face = !(e && *e == '0');
    }
    memset(s_keymap, 0, sizeof s_keymap);
    for (t = 0; t < sizeof s_key_targets / sizeof s_key_targets[0]; t++) {
        const char *v = s_arrow_face ? pc_settings_get("Keyboard", s_key_targets[t].name) : NULL;
        char buf[160], *tok, *ctx = NULL;
        if (!v) {
            /* Not in the file, or OOGIE_ARROW_FACE=0: the runtime's own map. */
            s_keymap[s_key_targets[t].vk] = (unsigned char)s_key_targets[t].vk;
            if (s_key_targets[t].vk == 'W') s_keymap[VK_NUMPAD8] = 'W';
            if (s_key_targets[t].vk == 'S') s_keymap[VK_NUMPAD2] = 'S';
            if (s_key_targets[t].vk == 'A') s_keymap[VK_NUMPAD4] = 'A';
            if (s_key_targets[t].vk == 'D') s_keymap[VK_NUMPAD6] = 'D';
            continue;
        }
        snprintf(buf, sizeof buf, "%s", v);
        for (tok = strtok_s(buf, ", \t", &ctx); tok; tok = strtok_s(NULL, ", \t", &ctx)) {
            int vk = key_by_name(tok);
            if (vk) s_keymap[vk] = (unsigned char)s_key_targets[t].vk;
            else if (_stricmp(tok, "none"))
                fprintf(stderr, "[INPUT] settings.ini [Keyboard] %s: unknown key '%s'\n",
                        s_key_targets[t].name, tok);
        }
    }
}

static void keys_release_all(void);

/* The settings menu changed [Keyboard]: rebuild the map at the next key and
 * let go of everything held under the old one. */
void oogie_keymap_reload(void)
{
    s_arrow_face = -1;
    keys_release_all();
}

static int key_target(int vk)
{
    if (s_arrow_face < 0)
        keymap_load();
    return (unsigned)vk < 256 ? s_keymap[vk] : 0;
}

static void key_event(int vk, int down)
{
    int t, v, held = 0;
    if ((unsigned)vk >= 256)
        return;
    s_phys[vk] = down ? 1 : 0;
    t = key_target(vk);
    for (v = 1; v < 256 && !held; v++)
        if (s_phys[v] && key_target(v) == t)
            held = 1;
    xbox_InputKeySet(t, held);
}

static void keys_release_all(void)
{
    memset(s_phys, 0, sizeof s_phys);
    xbox_InputKeysClear();
}

static void keys_sync(void)
{
    int vk;
    if (!s_hwnd)
        return;
    if (GetForegroundWindow() != s_hwnd) {
        keys_release_all();
        return;
    }
    for (vk = 1; vk < 256; vk++)
        if (s_phys[vk] && !(GetAsyncKeyState(vk) & 0x8000))
            key_event(vk, 0);
}

/* Window size and fullscreen (owner, 2026-10-18; settings.ini [Display]).
 * The swap chain follows the window's client size, so a maximised or
 * fullscreen window is drawn at its real resolution, and the 4:3 picture is
 * letterboxed rather than stretched (keep_aspect = 0 stretches). F11 or
 * Alt+Enter toggles borderless fullscreen on the window's monitor. */
static int s_keep_aspect = 1;
static int s_fullscreen;
static void window_place_save(void);
static WINDOWPLACEMENT s_wp = { sizeof(WINDOWPLACEMENT) };

/* g_pc.monitor: 0 the window's own monitor, n the n-th one Windows lists. */
static HMONITOR s_mon_pick; static int s_mon_i;
static BOOL CALLBACK mon_pick_cb(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    (void)dc; (void)r;
    if (++s_mon_i == (int)lp) { s_mon_pick = m; return FALSE; }
    return TRUE;
}
static HMONITOR pick_monitor(void)
{
    s_mon_pick = NULL; s_mon_i = 0;
    if (g_pc.monitor > 0) EnumDisplayMonitors(NULL, NULL, mon_pick_cb, (LPARAM)g_pc.monitor);
    return s_mon_pick ? s_mon_pick : MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTOPRIMARY);
}

static void toggle_fullscreen(void)
{
    LONG st = GetWindowLongA(s_hwnd, GWL_STYLE);
    if (!s_fullscreen) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        GetWindowPlacement(s_hwnd, &s_wp);
        GetMonitorInfoA(pick_monitor(), &mi);
        s_fullscreen = 1;               /* before the resize: window_place_save skips it */
        SetWindowLongA(s_hwnd, GWL_STYLE, (st & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        /* Topmost while active, so the taskbar does not cover it (it stays
         * on top of a borderless window Windows does not treat as fullscreen). */
        SetWindowPos(s_hwnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        s_fullscreen = 1;
    } else {
        SetWindowLongA(s_hwnd, GWL_STYLE, (st & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPos(s_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPlacement(s_hwnd, &s_wp);
        SetWindowPos(s_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                     | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        s_fullscreen = 0;
        window_place_save();            /* the restored window */
    }
}

/* Called before each present: the back buffer takes the client size. */
static void back_resize(void)
{
    static UINT cur_w, cur_h;
    RECT rc;
    UINT w, h;
    ID3D11Texture2D *bb = NULL;
    if (!GetClientRect(s_hwnd, &rc)) return;
    w = (UINT)(rc.right - rc.left); h = (UINT)(rc.bottom - rc.top);
    if (w < 16 || h < 16) return;                 /* minimised */
    if (!cur_w) {
        D3D11_TEXTURE2D_DESC bd;
        IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb);
        ID3D11Texture2D_GetDesc(bb, &bd);
        ID3D11Texture2D_Release(bb);
        cur_w = bd.Width; cur_h = bd.Height;
    }
    if (w == cur_w && h == cur_h) return;
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    ID3D11RenderTargetView_Release(s_back_rtv);
    s_back_rtv = NULL;
    if (FAILED(IDXGISwapChain_ResizeBuffers(s_swap, 0, w, h, DXGI_FORMAT_UNKNOWN, 0)))
        fprintf(stderr, "[D3D11] ResizeBuffers %ux%u failed\n", w, h);
    else
        cur_w = w, cur_h = h;
    IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb, NULL, &s_back_rtv);
    ID3D11Texture2D_Release(bb);
}

/* The window's place for the next start ([Display] window_place: x y w h
 * maximised), not while fullscreen, and only when it changed. */
static void window_place_save(void)
{
    static char last[64];
    WINDOWPLACEMENT wp;
    char b[64];
    if (!s_hwnd || s_fullscreen) return;
    wp.length = sizeof wp;
    if (!GetWindowPlacement(s_hwnd, &wp) || wp.showCmd == SW_SHOWMINIMIZED) return;
    snprintf(b, sizeof b, "%ld %ld %ld %ld %d", wp.rcNormalPosition.left, wp.rcNormalPosition.top,
             wp.rcNormalPosition.right - wp.rcNormalPosition.left,
             wp.rcNormalPosition.bottom - wp.rcNormalPosition.top, wp.showCmd == SW_SHOWMAXIMIZED);
    if (!strcmp(b, last)) return;
    snprintf(last, sizeof last, "%s", b);
    pc_settings_set("Display", "window_place", b);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SETCURSOR:
        if (s_fullscreen && LOWORD(l) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break;
    case WM_EXITSIZEMOVE:
        window_place_save();
        break;
    case WM_SIZE:
        if (w == SIZE_MAXIMIZED || w == SIZE_RESTORED) window_place_save();
        break;
    case WM_CLOSE:
        window_place_save();
        InterlockedExchange(&s_closed, 1);
        ExitProcess(0);
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if ((w == VK_F11 && m == WM_KEYDOWN && !(l & 0x40000000))
                || (w == VK_RETURN && m == WM_SYSKEYDOWN && (l & 0x20000000) && !(l & 0x40000000))) {
            g_pc.fullscreen = !g_pc.fullscreen;   /* F11 or Alt+Enter, not START */
            pc_config_save_int("Display", "fullscreen", g_pc.fullscreen);
            return 0;
        }
        if (w == VK_F12 && m == WM_KEYDOWN) {     /* screenshot (pc_shot.c) */
            if (!(l & 0x40000000)) InterlockedExchange(&s_shot_req, 1);
            return 0;
        }
        if (pc_menu_key((int)w, 1)) {
            keys_release_all();                   /* the game sees nothing held */
            return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;
        }
        key_event((int)w, 1);
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (pc_menu_key((int)w, 0))
            return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;
        key_event((int)w, 0);
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;
    case WM_ACTIVATE:
        /* Fullscreen: on top while active, behind other windows after Alt+Tab. */
        if (s_fullscreen)
            SetWindowPos(h, LOWORD(w) == WA_INACTIVE ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        /* Pause in Background (Audio page): switching to another window
         * opens the settings menu, which pauses the game. */
        if (LOWORD(w) == WA_INACTIVE && g_pc.pause_bg && !pc_menu_is_open())
            pc_menu_request_toggle();
        break;
    case WM_KILLFOCUS:
        keys_release_all();
        pc_menu_keys_reset();           /* key-ups now go to another window */
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* Declarations every pixel shader shares, the generated combiner shaders
 * included (they must match the vertex shader's output). */
#define HLSL_COMMON \
"cbuffer C : register(b0) { float4 rt; float4 tx; float4 at;\n" \
"  uint4 rcs; uint4 rcw[8]; float4 k0[8]; float4 k1[8]; uint4 fin; float4 fk0; float4 fk1; float4 fogc; };\n" \
"struct VI { float4 p : POSITION; float4 c : COLOR0; float2 t : TEXCOORD0;\n" \
"  float4 s : COLOR1; float2 fq : TEXCOORD1; float4 t1 : TEXCOORD2; float4 t2 : TEXCOORD3; float4 t3 : TEXCOORD4; };\n" \
"struct VO { float4 p : SV_Position; float4 c : COLOR0; float2 t : TEXCOORD0;\n" \
"  float4 s : COLOR1; float2 fq : TEXCOORD1; float4 t1 : TEXCOORD2; float4 t2 : TEXCOORD3; float4 t3 : TEXCOORD4; };\n" \
"Texture2D T : register(t0); Texture2D T1 : register(t1); Texture2D T2 : register(t2); Texture2D T3 : register(t3);\n" \
"SamplerState S : register(s0); SamplerState S1 : register(s1); SamplerState S2 : register(s2); SamplerState S3 : register(s3);\n" \
"float4 atest(float4 c) {\n" \
"  if (at.x > 0.5) { float a = floor(c.a * 255.0 + 0.5); float r = at.z; int f = (int)at.y; bool ok = true;\n" \
"    if (f == 0) ok = false; else if (f == 1) ok = a < r; else if (f == 2) ok = a == r;\n" \
"    else if (f == 3) ok = a <= r; else if (f == 4) ok = a > r; else if (f == 5) ok = a != r;\n" \
"    else if (f == 6) ok = a >= r; if (!ok) discard; }\n" \
"  return c; }\n" \
"float4 ptex(Texture2D tex, SamplerState ss, float4 c) {\n" \
"  float q = (abs(c.w) > 1e-20) ? c.w : 1.0; return tex.Sample(ss, c.xy / q); }\n"

static const char s_hlsl[] =
HLSL_COMMON
"VO vs(VI i) {\n"
"  VO o; float w = (i.p.w > 1e-30) ? 1.0 / i.p.w : 1.0;\n"
"  float x = i.p.x * rt.x * 2.0 - 1.0; float y = 1.0 - i.p.y * rt.y * 2.0;\n"
"  float z = saturate(i.p.z * rt.z);\n"
"  o.p = float4(x * w, y * w, z * w, w); o.c = i.c; o.t = i.t * tx.xy;\n"
"  o.s = i.s; o.fq = i.fq; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3; return o; }\n"
"float4 ps(VO i) : SV_Target {\n"
"  float4 c = i.c;\n"
"  if (tx.w > 0.5) {  /* Bink YUV: Y on t0, chroma planes on t1 and t2 (BT.601) */\n"
"    float y = 1.164 * (T.Sample(S, i.t).r - 0.0627);\n"
"    float cr = T1.Sample(S, i.t).r - 0.502, cb = T2.Sample(S, i.t).r - 0.502;\n"
"    if (tx.w > 1.5) { float sw = cr; cr = cb; cb = sw; }\n"
"    return float4(saturate(y + 1.596 * cr), saturate(y - 0.813 * cr - 0.391 * cb),\n"
"                  saturate(y + 2.018 * cb), 1.0); }\n"
"  if (tx.z > 0.5) c *= T.Sample(S, i.t);\n"
"  return atest(saturate(c)); }\n"
"struct BO { float4 p : SV_Position; float2 t : TEXCOORD; };\n"
"BO bvs(uint id : SV_VertexID) { BO o; float2 t = float2((id << 1) & 2, id & 2);\n"
"  o.t = t * rt.zw; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
"float4 bps(BO i) : SV_Target { return float4(T.Sample(S, i.t).rgb, 1); }\n"
/* sides of a 4:3 screen in a wide window: the picture, zoomed to the width
 * (tx.x: the part of its height shown), blurred (tx.y) and darkened */
"float4 fbps(BO i) : SV_Target { float2 c = i.t; c.y = rt.w * 0.5 + (c.y - rt.w * 0.5) * tx.x;\n"
"  float3 s = 0; for (int a = -3; a <= 3; a++) for (int b = -3; b <= 3; b++)\n"
"    s += T.SampleLevel(S, c + float2(a, b) * tx.y * rt.zw, 0).rgb;\n"
"  return float4(s / 49.0 * 0.45, 1); }\n"
/* Anti-aliasing (settings: Graphics > Anti-Aliasing), on the finished frame
 * at render resolution: where the luminance contrast around a pixel is high,
 * blend along the edge direction found from the four diagonals (the FXAA
 * idea; written here). tx.xy: one texel; tx.z: longest blend in texels. */
"float lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
"float3 tap(float2 u) { return T.SampleLevel(S, u, 0).rgb; }\n"
"float4 aaps(BO i) : SV_Target { float2 p = tx.xy, u = i.t; float3 cm = tap(u); float lm = lum(cm);\n"
"  float ln = lum(tap(u + float2(0, -p.y))), ls = lum(tap(u + float2(0, p.y)));\n"
"  float lw = lum(tap(u + float2(-p.x, 0))), le = lum(tap(u + float2(p.x, 0)));\n"
"  float lmin = min(lm, min(min(ln, ls), min(lw, le))), lmax = max(lm, max(max(ln, ls), max(lw, le)));\n"
"  if (lmax - lmin < max(0.0312, lmax * 0.125)) return float4(cm, 1);\n"
"  float a = lum(tap(u - p)), b = lum(tap(u + float2(p.x, -p.y)));\n"
"  float c = lum(tap(u + float2(-p.x, p.y))), d = lum(tap(u + p));\n"
"  float2 dir = float2(-((a + b) - (c + d)), (a + c) - (b + d));\n"
"  float red = max((a + b + c + d) * 0.03125, 1.0 / 128.0);\n"
"  dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + red), -tx.z, tx.z) * p;\n"
"  float3 x = 0.5 * (tap(u - dir / 6.0) + tap(u + dir / 6.0));\n"
"  float3 y = x * 0.5 + 0.25 * (tap(u - dir * 0.5) + tap(u + dir * 0.5));\n"
"  float ly = lum(y); return float4((ly < lmin || ly > lmax) ? x : y, 1); }\n"
/* Sharpening (Graphics > Sharpening): contrast-adaptive, after the idea of
 * AMD FidelityFX CAS: less where the neighbourhood already has contrast, so
 * edges do not ring. tx.z: strength 0..1. */
"float4 shps(BO i) : SV_Target { float2 p = tx.xy, u = i.t; float3 c = tap(u);\n"
"  float3 n = tap(u + float2(0, -p.y)), s = tap(u + float2(0, p.y)), w = tap(u + float2(-p.x, 0)), e = tap(u + float2(p.x, 0));\n"
"  float3 mn = min(c, min(min(n, s), min(w, e))), mx = max(c, max(max(n, s), max(w, e)));\n"
"  float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));\n"
"  float3 k = amp * (-1.0 / lerp(8.0, 5.0, tx.z));\n"
"  return float4(saturate((c + (n + s + w + e) * k) / (1.0 + 4.0 * k)), 1); }\n"
/* Colour-Blind Filter (Graphics, accessibility): one 3x3 colour matrix,
 * rows in tx.xyz, at.xyz, k0[0].xyz (cb_matrix). */
"float4 cbps(BO i) : SV_Target { float3 c = tap(i.t);\n"
"  return float4(saturate(float3(dot(tx.xyz, c), dot(at.xyz, c), dot(k0[0].xyz, c))), 1); }\n";

/* ── register combiners: one generated pixel shader per program ──
 *
 * The NV2A combiner program (up to 8 stages and the final combiner) becomes
 * straight-line HLSL with every register a named variable, compiled on first
 * use and cached by the words that define it. Rules from the runtime's
 * nv2a_combiner.c (itself after xemu's psh.c). A general interpreter in one
 * shader (register file as an indexed array) ran the level at 3 fps on this
 * GPU; the generated ones cost about as much as the fixed path.
 * Registers: 0 zero 1 c0 2 c1 3 fog 4 v0 5 v1 8-11 t0-t3 12 r0 13 r1
 * 14 v1+r0 (final) 15 E*F (final). An input byte is reg | alpha<<4 | map<<5. */
static const char *const s_rc_reg[16] = {
    "rz", "c0", "c1", "fg", "v0", "v1", "rz", "rz",
    "t0", "t1", "t2", "t3", "r0", "r1", "vs", "ef"
};

/* Compiled shaders are kept on disk (%LOCALAPPDATA%\ProjectOogie\shaders,
 * keyed by a hash of source, entry and target), so a generated shader is
 * compiled once ever rather than once per session: each compile was a
 * visible hitch the first time an area or effect appeared. */
static char s_shader_dir[MAX_PATH];
static int s_shader_dir_state;              /* 0 unknown, 1 ok, -1 none */
static uint32_t s_sh_compiled, s_sh_loaded;
static double s_sh_compile_ms;

static uint64_t shader_key(const char *src, size_t len, const char *entry, const char *target)
{
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < len; i++) h = (h ^ (uint8_t)src[i]) * 1099511628211ull;
    for (i = 0; entry[i]; i++) h = (h ^ (uint8_t)entry[i]) * 1099511628211ull;
    for (i = 0; target[i]; i++) h = (h ^ (uint8_t)target[i]) * 1099511628211ull;
    return h;
}

static HRESULT compile_cached(const char *src, size_t len, const char *name, const char *entry,
                              const char *target, ID3DBlob **code, ID3DBlob **err)
{
    char path[MAX_PATH + 32];
    uint64_t key = shader_key(src, len, entry, target);
    LARGE_INTEGER f, t0, t1;
    HRESULT hr;
    FILE *fp;
    *code = NULL;
    *err = NULL;
    if (!s_shader_dir_state) {
        const char *la = getenv("LOCALAPPDATA");
        s_shader_dir_state = -1;
        if (la && !(getenv("OOGIE_SHADER_CACHE") && *getenv("OOGIE_SHADER_CACHE") == '0')) {
            snprintf(s_shader_dir, sizeof s_shader_dir, "%s\\ProjectOogie", la);
            CreateDirectoryA(s_shader_dir, NULL);
            strncat(s_shader_dir, "\\shaders", sizeof s_shader_dir - strlen(s_shader_dir) - 1);
            CreateDirectoryA(s_shader_dir, NULL);
            if (GetFileAttributesA(s_shader_dir) != INVALID_FILE_ATTRIBUTES)
                s_shader_dir_state = 1;
        }
    }
    if (s_shader_dir_state > 0) {
        snprintf(path, sizeof path, "%s\\%016llx.cso", s_shader_dir, (unsigned long long)key);
        fp = fopen(path, "rb");
        if (fp) {
            long n;
            fseek(fp, 0, SEEK_END);
            n = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (n > 0 && SUCCEEDED(D3DCreateBlob((SIZE_T)n, code))
                    && fread(ID3D10Blob_GetBufferPointer(*code), 1, (size_t)n, fp) == (size_t)n) {
                fclose(fp);
                s_sh_loaded++;
                return S_OK;
            }
            if (*code) { ID3D10Blob_Release(*code); *code = NULL; }
            fclose(fp);
        }
    }
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    hr = D3DCompile(src, len, name, NULL, NULL, entry, target,
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code, err);
    QueryPerformanceCounter(&t1);
    s_sh_compiled++;
    s_sh_compile_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    fprintf(stderr, "[D3D11] shader compiled %s %016llx (%.0f ms)\n", name, (unsigned long long)key,
            (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart);
    if (SUCCEEDED(hr) && s_shader_dir_state > 0 && (fp = fopen(path, "wb")) != NULL) {
        fwrite(ID3D10Blob_GetBufferPointer(*code), 1, ID3D10Blob_GetBufferSize(*code), fp);
        fclose(fp);
    }
    return hr;
}

typedef struct { char *p; size_t n, cap; } Str;

static void sadd(Str *s, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (s->cap - s->n < 512) {
        size_t cap = s->cap ? s->cap * 2 : 8192;
        char *np = (char *)realloc(s->p, cap);
        if (!np) return;
        s->p = np; s->cap = cap;
    }
    va_start(ap, fmt);
    k = vsnprintf(s->p + s->n, s->cap - s->n, fmt, ap);
    va_end(ap);
    if (k > 0 && (size_t)k < s->cap - s->n) s->n += (size_t)k;
}

/* An input as HLSL: `alpha` selects the alpha portion (blue unless the
 * alpha bit is set); otherwise rgb (.aaa with the alpha bit). */
static void rc_in(char *out, size_t cap, uint32_t b, int alpha)
{
    const char *r = s_rc_reg[b & 15];
    char src[32];
    if (alpha) snprintf(src, sizeof src, "%s.%s", r, (b & 16) ? "a" : "b");
    else snprintf(src, sizeof src, "%s.%s", r, (b & 16) ? "aaa" : "rgb");
    switch ((b >> 5) & 7) {
    case 0: snprintf(out, cap, "max(%s, 0.0)", src); break;
    case 1: snprintf(out, cap, "(1.0 - saturate(%s))", src); break;
    case 2: snprintf(out, cap, "(2.0 * max(%s, 0.0) - 1.0)", src); break;
    case 3: snprintf(out, cap, "(1.0 - 2.0 * max(%s, 0.0))", src); break;
    case 4: snprintf(out, cap, "(max(%s, 0.0) - 0.5)", src); break;
    case 5: snprintf(out, cap, "(0.5 - max(%s, 0.0))", src); break;
    case 6: snprintf(out, cap, "%s", src); break;
    default: snprintf(out, cap, "(-%s)", src); break;
    }
}

static const char *rc_out_scale(uint32_t m)
{
    switch (m) {
    case 0x08: return "(%s - 0.5)";
    case 0x10: return "(%s * 2.0)";
    case 0x18: return "((%s - 0.5) * 2.0)";
    case 0x20: return "(%s * 4.0)";
    case 0x30: return "(%s * 0.5)";
    default:   return "(%s)";
    }
}

static void rc_stage_tex(Str *s, int st, uint32_t mode)
{
    static const char *const tex[4] = { "T", "T1", "T2", "T3" };
    static const char *const smp[4] = { "S", "S1", "S2", "S3" };
    static const char *const crd[4] = { "float4(i.t, 0, i.fq.y)", "i.t1", "i.t2", "i.t3" };
    if (mode == 0)
        sadd(s, "  float4 t%d = float4(0, 0, 0, 1);\n", st);
    else if (mode == 4)
        sadd(s, "  float4 t%d = saturate(%s);\n", st, crd[st]);
    else if (mode >= 1 && mode <= 3)    /* cube approximated as 2D */
        sadd(s, "  float4 t%d = ptex(%s, %s, %s);\n", st, tex[st], smp[st], crd[st]);
    else                                 /* bump-env, dot product: zero */
        sadd(s, "  float4 t%d = float4(0, 0, 0, 0);\n", st);
}

/* HLSL for one combiner program; caller frees. */
static char *rc_generate(const Nv2aCombiner *rc)
{
    Str s = { 0 };
    uint32_t n = rc->control & 0xFF, fl = rc->control >> 8, st;
    char A[96], B[96], C[96], D[96], E[96], F[96];
    if (n > 8) n = 8;
    sadd(&s, "%s", HLSL_COMMON);
    sadd(&s, "float4 rcps(VO i) : SV_Target {\n");
    sadd(&s, "  float4 rz = 0, c0 = k0[0], c1 = k1[0], fg = float4(fogc.rgb, i.fq.x);\n");
    sadd(&s, "  float4 v0 = i.c, v1 = i.s, r1 = 0, vs = 0, ef = 0;\n");
    for (st = 0; st < 4; st++)
        rc_stage_tex(&s, (int)st, (rc->stage_program >> (st * 5)) & 0x1F);
    /* r0.a starts as texture 0's alpha, or 1 with stage 0 off */
    sadd(&s, "  float4 r0 = float4(0, 0, 0, %s);\n", (rc->stage_program & 0x1F) ? "t0.a" : "1.0");
    for (st = 0; st < n; st++) {
        uint32_t icw = rc->color_icw[st], ocw = rc->color_ocw[st];
        uint32_t aicw = rc->alpha_icw[st], aocw = rc->alpha_ocw[st];
        uint32_t f = ocw >> 12, af = aocw >> 12, d;
        const char *sc = rc_out_scale(f & 0x38), *asc = rc_out_scale(af & 0x38);
        char e[160];
        sadd(&s, "  {\n    c0 = k0[%u]; c1 = k1[%u];\n", (fl & 0x010) ? st : 0, (fl & 0x100) ? st : 0);
        if ((f & 4) || (af & 4))
            sadd(&s, "    bool mux = %s;\n", (fl & 1) ? "r0.a >= 0.5" : "(((uint)(r0.a * 255.0)) & 1) != 0");
        rc_in(A, sizeof A, icw >> 24, 0); rc_in(B, sizeof B, (icw >> 16) & 255, 0);
        rc_in(C, sizeof C, (icw >> 8) & 255, 0); rc_in(D, sizeof D, icw & 255, 0);
        sadd(&s, "    float3 pab, pcd;\n");
        if (f & 2) sadd(&s, "    pab = dot(%s, %s).xxx;\n", A, B);
        else       sadd(&s, "    pab = %s * %s;\n", A, B);
        if (f & 1) sadd(&s, "    pcd = dot(%s, %s).xxx;\n", C, D);
        else       sadd(&s, "    pcd = %s * %s;\n", C, D);
        sadd(&s, "    float3 sm = %s;\n", (f & 4) ? "mux ? pcd : pab" : "pab + pcd");
        rc_in(A, sizeof A, aicw >> 24, 1); rc_in(B, sizeof B, (aicw >> 16) & 255, 1);
        rc_in(C, sizeof C, (aicw >> 8) & 255, 1); rc_in(D, sizeof D, aicw & 255, 1);
        sadd(&s, "    float apab = %s * %s, apcd = %s * %s;\n", A, B, C, D);
        sadd(&s, "    float asm1 = %s;\n", (af & 4) ? "mux ? apcd : apab" : "apab + apcd");
        snprintf(e, sizeof e, sc, "pab"); sadd(&s, "    float3 ab = clamp(%s, -1.0, 1.0);\n", e);
        snprintf(e, sizeof e, sc, "pcd"); sadd(&s, "    float3 cd = clamp(%s, -1.0, 1.0);\n", e);
        snprintf(e, sizeof e, sc, "sm");  sadd(&s, "    float3 ms = clamp(%s, -1.0, 1.0);\n", e);
        snprintf(e, sizeof e, asc, "apab"); sadd(&s, "    float aab = clamp(%s, -1.0, 1.0);\n", e);
        snprintf(e, sizeof e, asc, "apcd"); sadd(&s, "    float acd = clamp(%s, -1.0, 1.0);\n", e);
        snprintf(e, sizeof e, asc, "asm1"); sadd(&s, "    float ams = clamp(%s, -1.0, 1.0);\n", e);
        /* all reads happened above; now the writes, in the C evaluator's order */
        if ((d = (ocw >> 4) & 15) != 0) {
            sadd(&s, "    %s.rgb = ab;\n", s_rc_reg[d]);
            if (f & 0x80) sadd(&s, "    %s.a = ab.z;\n", s_rc_reg[d]);
        }
        if ((d = ocw & 15) != 0) {
            sadd(&s, "    %s.rgb = cd;\n", s_rc_reg[d]);
            if (f & 0x40) sadd(&s, "    %s.a = cd.z;\n", s_rc_reg[d]);
        }
        if ((d = (ocw >> 8) & 15) != 0) sadd(&s, "    %s.rgb = ms;\n", s_rc_reg[d]);
        if ((d = (aocw >> 4) & 15) != 0) sadd(&s, "    %s.a = aab;\n", s_rc_reg[d]);
        if ((d = aocw & 15) != 0) sadd(&s, "    %s.a = acd;\n", s_rc_reg[d]);
        if ((d = (aocw >> 8) & 15) != 0) sadd(&s, "    %s.a = ams;\n", s_rc_reg[d]);
        sadd(&s, "    rz = 0;\n  }\n");
    }
    if (rc->final0 || rc->final1) {
        uint32_t f0 = rc->final0, f1 = rc->final1, ff = f1 & 0xFF;
        sadd(&s, "  c0 = fk0; c1 = fk1;\n");
        sadd(&s, "  { float3 a = %s, b = %s; vs = float4(%s, 0); }\n",
             (ff & 0x40) ? "1.0 - v1.rgb" : "v1.rgb", (ff & 0x20) ? "1.0 - r0.rgb" : "r0.rgb",
             (ff & 0x80) ? "saturate(a + b)" : "a + b");
        rc_in(E, sizeof E, f1 >> 24, 0); rc_in(F, sizeof F, (f1 >> 16) & 255, 0);
        sadd(&s, "  ef = float4(%s * %s, 0);\n", E, F);
        rc_in(A, sizeof A, f0 >> 24, 0); rc_in(B, sizeof B, (f0 >> 16) & 255, 0);
        rc_in(C, sizeof C, (f0 >> 8) & 255, 0); rc_in(D, sizeof D, f0 & 255, 0);
        rc_in(E, sizeof E, (f1 >> 8) & 255, 1);
        sadd(&s, "  { float3 fa = %s; float4 o = float4(%s + %s * (1.0 - fa) + %s * fa, %s);\n",
             A, D, C, B, E);
        sadd(&s, "    return atest(saturate(o)); }\n}\n");
    } else {
        sadd(&s, "  return atest(saturate(r0));\n}\n");
    }
    return s.p;
}

/* Cache of compiled combiner shaders, keyed by the program's words. */
typedef struct {
    uint32_t key[38];
    ID3D11PixelShader *ps;
} RcShader;
#define MAX_RC_SHADERS 1024
static RcShader s_rcs[MAX_RC_SHADERS];
static int s_nrcs;
static int s_rc_failed;

static void rc_key(const Nv2aCombiner *rc, uint32_t k[38])
{
    uint32_t n = rc->control & 0xFF, st;
    memset(k, 0, 38 * sizeof *k);
    if (n > 8) n = 8;
    k[0] = rc->control; k[1] = rc->stage_program; k[2] = rc->final0; k[3] = rc->final1;
    for (st = 0; st < n; st++) {
        k[4 + st * 4 + 0] = rc->color_icw[st];
        k[4 + st * 4 + 1] = rc->alpha_icw[st];
        k[4 + st * 4 + 2] = rc->color_ocw[st];
        k[4 + st * 4 + 3] = rc->alpha_ocw[st];
    }
}

static ID3D11PixelShader *rc_shader(const Nv2aCombiner *rc)
{
    static int last = -1;
    uint32_t k[38];
    ID3DBlob *code = NULL, *err = NULL;
    char *src;
    RcShader *e;
    int i;
    rc_key(rc, k);
    if (last >= 0 && !memcmp(s_rcs[last].key, k, sizeof k))
        return s_rcs[last].ps;
    for (i = 0; i < s_nrcs; i++)
        if (!memcmp(s_rcs[i].key, k, sizeof k)) {
            last = i;
            return s_rcs[i].ps;
        }
    if (s_nrcs == MAX_RC_SHADERS)
        return NULL;
    src = rc_generate(rc);
    if (!src)
        return NULL;
    e = &s_rcs[s_nrcs];
    memcpy(e->key, k, sizeof k);
    e->ps = NULL;
    if (FAILED(compile_cached(src, strlen(src), "oogie_rc", "rcps", "ps_4_0", &code, &err))) {
        if (s_rc_failed++ < 5)
            fprintf(stderr, "[D3D11] combiner shader failed: %s\n%s\n",
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?", src);
    } else {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                       ID3D10Blob_GetBufferSize(code), NULL, &e->ps);
    }
    if (code) ID3D10Blob_Release(code);
    if (err) ID3D10Blob_Release(err);
    free(src);
    last = s_nrcs++;
    if (s_nrcs <= 8 || (s_nrcs & (s_nrcs - 1)) == 0)
        fprintf(stderr, "[D3D11] combiner programs: %d (shaders: %u compiled in %.0f ms, %u from the disk cache)\n",
                s_nrcs, s_sh_compiled, s_sh_compile_ms, s_sh_loaded);
    return e->ps;
}

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    if (FAILED(D3DCompile(s_hlsl, sizeof s_hlsl - 1, "oogie_d3d11", NULL, NULL,
                          entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        fprintf(stderr, "[D3D11] shader %s failed: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static int init_device(void)
{
    WNDCLASSA wc;
    DXGI_SWAP_CHAIN_DESC sd;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
    ID3D11Texture2D *bb = NULL;
    ID3DBlob *b;
    RECT r;
    int scale = 2;
    const char *e;
    unsigned i;

    /* Called on every draw: these two tests must stay first (a getenv here
     * was ~9% of the GPU thread, tools/sampler). */
    if (s_ready) return 1;
    if (s_failed) return 0;
    s_failed = 1;
    {
        /* Real pixels, not Windows' 125-200% bitmap stretch: sharper, and a
         * borderless fullscreen window then covers the taskbar. */
        typedef BOOL (WINAPI *SetCtx)(HANDLE);
        SetCtx f = (SetCtx)(void *)GetProcAddress(GetModuleHandleA("user32.dll"),
                                                  "SetProcessDpiAwarenessContext");
        if (!f || !f((HANDLE)(INT_PTR)-4))       /* PER_MONITOR_AWARE_V2 */
            SetProcessDPIAware();
    }
    e = getenv("OOGIE_WINDOW_SCALE");
    if (e && atoi(e) >= 1 && atoi(e) <= 8) {
        scale = atoi(e);
    } else {
        /* "auto" (or 0): the largest whole multiple of 640x480 that fits
         * the primary monitor's work area with room for the title bar. */
        RECT wa;
        if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
            int fit_w = (wa.right - wa.left) * 9 / 10 / 640;
            int fit_h = ((wa.bottom - wa.top) * 9 / 10 - 40) / 480;
            scale = fit_w < fit_h ? fit_w : fit_h;
            if (scale < 1) scale = 1;
            if (scale > 8) scale = 8;
        }
    }

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "OogieD3D11";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);
    r.left = 0; r.top = 0; r.right = 640 * scale; r.bottom = 480 * scale;
    if (g_pc.aspect == 1) r.right = (480 * scale * 16 + 8) / 9;   /* a 16:9 window */
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s_hwnd = CreateWindowA("OogieD3D11",
                           "The Nightmare Before Christmas: Oogie's Revenge (Project Oogie, D3D11)",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!s_hwnd) { fprintf(stderr, "[D3D11] CreateWindow failed\n"); return 0; }
    pc_menu_set_window(s_hwnd);
    {
        /* Where the window was last time ([Display] window_place, saved by
         * window_place_save), if that is still on a connected monitor. */
        const char *wp = pc_settings_get("Display", "window_place");
        int x, y, w, h, mx = 0;
        if (wp && sscanf(wp, "%d %d %d %d %d", &x, &y, &w, &h, &mx) >= 4 && w >= 320 && h >= 240) {
            RECT rr;
            rr.left = x; rr.top = y; rr.right = x + w; rr.bottom = y + h;
            if (MonitorFromRect(&rr, MONITOR_DEFAULTTONULL)) {
                SetWindowPos(s_hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
                if (mx) ShowWindow(s_hwnd, SW_MAXIMIZE);
            }
        }
    }

    memset(&sd, 0, sizeof sd);
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 640 * scale;
    sd.BufferDesc.Height = 480 * scale;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = s_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    {
        /* Other PCs (docs/COMPATIBILITY.md): any GPU and driver. Feature
         * levels 11.0, 10.1 and 10.0 are accepted (shaders are SM 4.0; the
         * GPU vertex-program path needs 11.0 and is skipped below it). If no
         * hardware device can be made, WARP (Windows' software D3D11) is
         * used, slow but working. OOGIE_D3D_WARP=1 forces WARP, for testing
         * without a second GPU vendor; OOGIE_D3D_DEBUG=1 asks for the D3D
         * debug layer (Graphics Tools must be installed) and copies its
         * warnings to the log. */
        static const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        const char *ew = getenv("OOGIE_D3D_WARP"), *ed = getenv("OOGIE_D3D_DEBUG");
        int force_warp = ew && *ew == '1';
        UINT flags = (ed && *ed == '1') ? D3D11_CREATE_DEVICE_DEBUG : 0;
        HRESULT hr = E_FAIL;
        int attempt;
        (void)fl;
        /* Laptops with two GPUs (Intel + Nvidia/AMD): ask Windows for the
         * high-performance one (IDXGIFactory6, Windows 10 1803+); the default
         * adapter is often the integrated GPU. OOGIE_GPU=powersave asks for
         * the low-power one; anything without IDXGIFactory6 uses the default. */
        IDXGIAdapter *pick = NULL;
        {
            IDXGIFactory1 *f1 = NULL;
            IDXGIFactory6 *f6 = NULL;
            const char *eg = getenv("OOGIE_GPU");
            DXGI_GPU_PREFERENCE pref = (eg && !_stricmp(eg, "powersave")) ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                                                                           : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
            if (!force_warp && SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f1)) && f1) {
                if (SUCCEEDED(IDXGIFactory1_QueryInterface(f1, &IID_IDXGIFactory6, (void **)&f6)) && f6) {
                    if (FAILED(IDXGIFactory6_EnumAdapterByGpuPreference(f6, 0, pref, &IID_IDXGIAdapter, (void **)&pick)))
                        pick = NULL;
                    IDXGIFactory6_Release(f6);
                }
                IDXGIFactory1_Release(f1);
            }
        }
        for (attempt = 0; attempt < 4 && FAILED(hr); attempt++) {
            D3D_DRIVER_TYPE dt = (force_warp || attempt >= 2) ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
            UINT fflags = (attempt & 1) ? 0 : flags;           /* retry without the debug layer */
            IDXGIAdapter *ad = dt == D3D_DRIVER_TYPE_HARDWARE ? pick : NULL;
            if ((attempt & 1) && !flags) continue;
            hr = D3D11CreateDeviceAndSwapChain(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : dt, NULL, fflags, fls, 3, D3D11_SDK_VERSION,
                                               &sd, &s_swap, &s_dev, &got, &s_ctx);
            if (FAILED(hr) && ad)                              /* that adapter failed: the default one */
                hr = D3D11CreateDeviceAndSwapChain(NULL, dt, NULL, fflags, fls, 3, D3D11_SDK_VERSION,
                                                   &sd, &s_swap, &s_dev, &got, &s_ctx);
            if (SUCCEEDED(hr)) {
                s_feature_level = got;
                s_is_warp = dt == D3D_DRIVER_TYPE_WARP;
                s_debug_layer = (fflags & D3D11_CREATE_DEVICE_DEBUG) != 0;
            } else {
                fprintf(stderr, "[D3D11] %s device%s: failed (0x%08lX)\n",
                        dt == D3D_DRIVER_TYPE_WARP ? "WARP" : "hardware",
                        (fflags & D3D11_CREATE_DEVICE_DEBUG) ? " with the debug layer" : "", (unsigned long)hr);
            }
        }
        if (pick) IDXGIAdapter_Release(pick);
        if (FAILED(hr)) {
            fprintf(stderr, "[D3D11] device creation failed\n");
            MessageBoxA(s_hwnd, "No Direct3D 11 device could be created (feature level 10.0 or newer is needed).\n"
                        "Update the graphics driver and try again.", "Project Oogie", MB_ICONERROR);
            return 0;
        }
        {
            /* Which GPU and driver: the first line to read in a report from another PC. */
            IDXGIDevice *xd = NULL;
            IDXGIAdapter *ad = NULL;
            DXGI_ADAPTER_DESC desc;
            char name[128];
            memset(&desc, 0, sizeof desc);
            if (SUCCEEDED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&xd)) && xd) {
                if (SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)) && ad) {
                    LARGE_INTEGER umd;
                    IDXGIAdapter_GetDesc(ad, &desc);
                    s_gpu_vendor = desc.VendorId;
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, NULL, NULL);
                    umd.QuadPart = 0;
                    IDXGIAdapter_CheckInterfaceSupport(ad, &IID_IDXGIDevice, &umd);
                    fprintf(stderr, "[D3D11] GPU: %s (vendor %04X device %04X, %u MB), driver %u.%u.%u.%u, feature level %X.%X%s%s\n",
                            name, desc.VendorId, desc.DeviceId, (unsigned)(desc.DedicatedVideoMemory >> 20),
                            (unsigned)HIWORD(umd.HighPart), (unsigned)LOWORD(umd.HighPart),
                            (unsigned)HIWORD(umd.LowPart), (unsigned)LOWORD(umd.LowPart),
                            (unsigned)(got >> 12), (unsigned)((got >> 8) & 0xF),
                            s_is_warp ? ", WARP (software)" : "", s_debug_layer ? ", debug layer on" : "");
                    IDXGIAdapter_Release(ad);
                }
                IDXGIDevice_Release(xd);
            }
        }
    }
    IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb, NULL, &s_back_rtv);
    ID3D11Texture2D_Release(bb);
    {
        /* Alt+Enter is ours (borderless), not DXGI's exclusive mode. */
        IDXGIFactory *fac = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetParent(s_swap, &IID_IDXGIFactory, (void **)&fac)) && fac) {
            IDXGIFactory_MakeWindowAssociation(fac, s_hwnd, DXGI_MWA_NO_ALT_ENTER);
            IDXGIFactory_Release(fac);
        }
        s_keep_aspect = g_pc.aspect != 2;
        if (g_pc.fullscreen)
            toggle_fullscreen();
    }

    if (!(b = compile("vs", "vs_4_0"))) return 0;
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_vs);
    {
        /* slot 0: Nv2aVertex; slot 1: OogieVertexExtra */
        D3D11_INPUT_ELEMENT_DESC il[8] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 1, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 40, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 56, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        ID3D11Device_CreateInputLayout(s_dev, il, 8, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), &s_layout);
    }
    ID3D10Blob_Release(b);
    if (!(b = compile("ps", "ps_4_0"))) return 0;
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_ps);
    ID3D10Blob_Release(b);
    if (!(b = compile("bvs", "vs_4_0"))) return 0;
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_blit_vs);
    ID3D10Blob_Release(b);
    if (!(b = compile("bps", "ps_4_0"))) return 0;
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_blit_ps);
    ID3D10Blob_Release(b);
    if ((b = compile("fbps", "ps_4_0")) != NULL) {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_fill_ps);
        ID3D10Blob_Release(b);
    }
    if ((b = compile("aaps", "ps_4_0")) != NULL) {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_aa_ps);
        ID3D10Blob_Release(b);
    }
    if ((b = compile("shps", "ps_4_0")) != NULL) {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_sharp_ps);
        ID3D10Blob_Release(b);
    }
    if ((b = compile("cbps", "ps_4_0")) != NULL) {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_cb_ps);
        ID3D10Blob_Release(b);
    }

    {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof(ShaderConsts);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb);
    }
    apply_samplers(g_pc.aniso);
    {
        D3D11_SAMPLER_DESC ss;
        memset(&ss, 0, sizeof ss);
        ss.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        ss.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(s_dev, &ss, &s_samp_blit);
    }
    {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;   /* ponytail: culling left to the executor's transform */
        rd.DepthClipEnable = FALSE;
        rd.ScissorEnable = FALSE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs);
    }
    {
        uint32_t px = 0xFFFFFFFFu;
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA init;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        init.pSysMem = &px; init.SysMemPitch = 4; init.SysMemSlicePitch = 4;
        ID3D11Device_CreateTexture2D(s_dev, &td, &init, &s_white);
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_white, NULL, &s_white_srv);
    }
    s_failed = 0;
    s_ready = 1;
    fprintf(stderr, "[D3D11] back end up: window %ux%u\n", 640 * scale, 480 * scale);
    fflush(stderr);
    return 1;
}

/* ── render targets ────────────────────────────────────────────── */

typedef struct {
    uint32_t key, w, h;
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    uint32_t last_frame;
    uint32_t shown_frame;       /* last flip that presented it, + 1 (0: never) */
    /* KNOWN_ISSUES #49: guest memory under the target, so a texture the CPU
     * later loads at the same address is not answered with the old picture. */
    uint32_t drawn_frame;       /* last frame drawn into, + 1 */
    uint32_t guest_hash;        /* sampled hash of guest memory at that flip */
    uint32_t baseline;          /* 1: take guest_hash at the next flip; 2: valid */
    uint32_t checked_frame;     /* replacement check done this frame, + 1 */
    float sc;                   /* render scale: the texture is SCL(w, sc) x SCL(h, sc) */
} RenderTarget;

typedef struct {
    uint32_t key, w, h;
    float sc;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
} DepthTarget;

/* A size in the game's pixels at render scale sc, in host pixels. */
#define SCL(n, sc) ((uint32_t)((float)(n) * (sc) + 0.5f))

#define MAX_RT 64
static RenderTarget s_rt[MAX_RT];
static int s_nrt;
static DepthTarget s_dt[32];
static int s_ndt;

static RenderTarget *s_frame_rt;       /* largest surface drawn since the last flip */
static RenderTarget *s_shown_rt;       /* what was presented last */
/* The largest surface drawn since the last flip that is also a display
 * buffer. Presented in preference to s_frame_rt: after finishing its frame
 * the game sometimes draws into an off-screen 640x480 surface (0x00EA4000,
 * trace pt3 frame 5480), and "largest drawn last" then showed that surface
 * for one frame -- the black or white flickers in the owner's video. */
static RenderTarget *s_frame_chain;

#define PHYS(va) ((va) & 0x0FFFFFFFu)

/* KNOWN_ISSUES #51: the game's own frame buffers. On the console only these
 * can be on screen; "the biggest surface drawn" is a guess that can pick an
 * off-screen layer. The results screen after a chapter draws its background
 * layers into other 640x480 surfaces and showed them (and black) at random.
 * Read from the D3D device (pointer at 0x001E25E8): +0x1A10 holds the buffer
 * count (2 or 3) and +0x1A14 the surfaces, whose +4 is the data address --
 * the layout D3D's Swap (sub_001D69E0) and its finish path (sub_001D6BD0)
 * use. Refreshed at every flip. OOGIE_PRESENT_GUESS=1: the old rule only. */
static uint32_t s_disp[3];
static int s_ndisp;
static RenderTarget *s_frame_disp;     /* last frame buffer drawn this frame */
static unsigned s_fl_draws, s_fl_disp;  /* OOGIE_FRAME_LOG counts */
/* OOGIE_FRAME_LOG: the frame's clears and draws by target, run-length coded
 * ("C35C D35C*135" ...), to see where the flip falls in the stream. */
static char s_fl_seq[512];
static uint32_t s_fl_last_key;
static char s_fl_last_kind;
static unsigned s_fl_run;
static void fl_seq(char kind, uint32_t key)
{
    size_t n = strlen(s_fl_seq);
    if (kind == s_fl_last_kind && key == s_fl_last_key) { s_fl_run++; return; }
    if (s_fl_run > 1 && n + 8 < sizeof s_fl_seq) n += (size_t)snprintf(s_fl_seq + n, sizeof s_fl_seq - n, "*%u", s_fl_run);
    if (n + 12 < sizeof s_fl_seq) snprintf(s_fl_seq + n, sizeof s_fl_seq - n, " %c%03X", kind, (key >> 12) & 0xFFF);
    s_fl_last_kind = kind; s_fl_last_key = key; s_fl_run = 1;
}
/* KNOWN_ISSUES #51: the frame buffers drawn since the last flip, in order.
 * D3D's Swap here ends a frame through its finish path, and when a target
 * other than the back buffer is bound it defers the FLIP_STALL to the next
 * SetRenderTarget -- into the next frame's draws (ik3: 131-145 draws per
 * flip instead of 135). Then two buffers were drawn: the rest of the
 * finished frame, then the start of the next. The earlier one is complete. */
static RenderTarget *s_disp_first, *s_disp_last;
static unsigned s_disp_switches;
/* KNOWN_ISSUES #51: some sessions run the game's buffers out of step with
 * D3D's flip: a menu frame's draws land in one buffer and the next frame's
 * clear lands in the same buffer before the flip (ss3: "D104*134 C104 D104"
 * per flip, where a good session has "C104 D104*135"), so the buffer shown
 * held a clear and one quad: black, or one layer. When a frame buffer that
 * was drawn in this flip interval gets a full colour clear, what it held is a
 * finished frame: it is copied aside and shown at the flip instead.
 * OOGIE_NO_CLEAR_SNAPSHOT=1 turns this off. */
static RenderTarget s_snap;            /* the copy, shown in place of a target */
static RenderTarget *s_snap_of;        /* the target it was copied from */
static int s_snap_valid;
static unsigned s_snap_count;

static void snap_before_clear(RenderTarget *r)
{
    static int off = -1;
    D3D11_TEXTURE2D_DESC td;
    /* Off by default since 2026-10-19 (KNOWN_ISSUES #57): with the Swap
     * interval fix the flip falls at the frame's end, and frames that clear
     * mid-way (inventory: 152 world draws, a full clear, 153 menu draws;
     * the trophy shelf) had the pre-clear half shown instead of the finished
     * frame: the owner's see-through menus and figure-less shelf, over 4000
     * times in one session. OOGIE_CLEAR_SNAPSHOT=1 turns it back on. */
    if (off < 0) { const char *e = getenv("OOGIE_CLEAR_SNAPSHOT"); off = !(e && *e == '1'); }
    if (off || !r->tex) return;
    ID3D11Texture2D_GetDesc(r->tex, &td);
    if (s_snap.tex) {
        D3D11_TEXTURE2D_DESC sd;
        ID3D11Texture2D_GetDesc(s_snap.tex, &sd);
        if (sd.Width != td.Width || sd.Height != td.Height) {
            ID3D11ShaderResourceView_Release(s_snap.srv); ID3D11Texture2D_Release(s_snap.tex);
            s_snap.srv = NULL; s_snap.tex = NULL;
        }
    }
    if (!s_snap.tex) {
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_snap.tex))) return;
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_snap.tex, NULL, &s_snap.srv);
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_snap.tex, (ID3D11Resource *)r->tex);
    s_snap.key = r->key; s_snap.w = r->w; s_snap.h = r->h; s_snap.sc = r->sc;
    s_snap_of = r;
    s_snap_valid = 1;
    if (s_snap_count++ < 10 || (s_snap_count & 4095) == 0)
        fprintf(stderr, "[PRESENT] f=%u %08X cleared after this flip's drawing: showing the finished frame (%u so far)\n",
                s_frame, r->key, s_snap_count);
}

/* Frames shown, for the frame-locked vblank (patch vblank_frame_lock). */
volatile long g_oogie_presents;

static void disp_refresh(void)
{
    static int off = -1;
    const uint8_t *m = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t dev, n, i, keys[3];
    if (off < 0) { const char *e = getenv("OOGIE_PRESENT_GUESS"); off = e && *e && *e != '0'; }
    s_ndisp = 0;
    if (off) return;
    dev = *(const volatile uint32_t *)(m + 0x001E25E8u);
    if (!dev || dev >= 0x04000000u - 0x1A20u) return;
    n = *(const volatile uint32_t *)(m + dev + 0x1A10u);
    if (n < 1 || n > 3) return;
    for (i = 0; i < n; i++) {
        uint32_t surf = *(const volatile uint32_t *)(m + dev + 0x1A14u + 4 * i);
        if (!surf || surf >= 0x04000000u - 8) return;
        keys[i] = PHYS(*(const volatile uint32_t *)(m + surf + 4));
        if (!keys[i]) return;
    }
    for (i = 0; i < n; i++) s_disp[i] = keys[i];
    s_ndisp = (int)n;
}

static int s_crtc_used;   /* this flip shows the finished / scanned-out buffer (be_flip) */
volatile uint32_t g_oogie_flip_done;   /* set by the executor (patch flip_done) */
static int is_disp(uint32_t key)
{
    int i;
    for (i = 0; i < s_ndisp; i++)
        if (s_disp[i] == key) return 1;
    return 0;
}

/* The size a surface really has. The clip rectangle is what the executor
 * reports; vertices are in absolute surface pixels, so the target is sized
 * from the pitch and, for full-screen buffers, a 4:3 height. */
static void surface_dims(const Nv2aSurface *s, uint32_t *w, uint32_t *h)
{
    uint32_t bpp = s->bytes_per_pixel ? s->bytes_per_pixel : 4;
    uint32_t pw = s->pitch ? s->pitch / bpp : s->width;
    /* KNOWN_ISSUES #51: the row length is the pitch. The clip rectangle is
     * sometimes set wider than the buffer (645x483, even 1542x1156 on the
     * 640-wide display buffers, owner's logs 2026-10-18); taking it as the
     * width made a "new" surface at the same address, which threw the
     * picture away -- black main menu, half-drawn inventory. */
    *w = (s->pitch && pw >= 16) ? pw : (pw > s->width ? pw : s->width);
    *h = s->height;
    if (*w >= 320 && *h < (*w * 3) / 4)
        *h = (*w * 3) / 4;
    /* The game zooms whole screens (menus opening, results, the title) by
     * scaling the clip: 642x481 ... 1886x1414 on 640x480 buffers, always
     * 4:3. A screen-sized buffer keeps its own rows. */
    if (s->pitch && *w >= 320 && *h > (*w * 3) / 4)
        *h = (*w * 3) / 4;
}

/* Render Resolution "Auto" (render_scale = 0): the largest multiple of
 * 640x480 that the window's height holds, at most 2 on integrated graphics
 * (Intel, vendor 0x8086) and at most 4; then one step down at a time, twice
 * at most, when the median frame interval over 3 s is above 18 ms (the game
 * runs in slow motion below 60 fps, #37). auto_scale_tick() runs at every
 * flip. */
static float s_auto_sc;
/* Auto's ceiling from the power source (dev cycle 8): on battery a laptop GPU
 * runs slower (the owner's Arc 140V slowdowns, #75), so Auto starts at 1.5x
 * at most, 1x with battery saver on; 0 = no ceiling (mains). */
static float power_cap(void)
{
    SYSTEM_POWER_STATUS ps;
    if (!GetSystemPowerStatus(&ps)) return 0.0f;
    if (ps.SystemStatusFlag) return 1.0f;
    if (ps.ACLineStatus == 0) return 1.5f;
    return 0.0f;
}
static float auto_scale(void)
{
    if (s_auto_sc <= 0.0f) {
        RECT rc;
        uint32_t h = 480, sc;
        if (s_hwnd && GetClientRect(s_hwnd, &rc) && rc.bottom - rc.top > 0) h = (uint32_t)(rc.bottom - rc.top);
        sc = h / 480;
        if (sc < 1) sc = 1;
        if (sc > 4) sc = 4;
        if (s_gpu_vendor == 0x8086 && sc > 2) sc = 2;
        s_auto_sc = (float)sc;
        fprintf(stderr, "[D3D11] render resolution auto: %ux (window height %u, vendor %04X)\n", sc, h, s_gpu_vendor);
        {
            float cap = power_cap();
            if (cap > 0.0f && s_auto_sc > cap) {
                s_auto_sc = cap;
                fprintf(stderr, "[D3D11] render resolution auto: %.1fx while on battery\n", cap);
            }
        }
    }
    return s_auto_sc;
}
/* 2026-10-24 (owner: slowdowns, the opening cutscene at ~40 fps with every
 * other frame slow): the median stayed under 18 ms, so Auto never stepped
 * down. It now also steps down when one 3D frame in five takes over 20 ms,
 * in half steps (2x, 1.5x, 1x), at most three times. */

/* Power source in the log (2026-10-24): on battery a laptop GPU may run at a
 * fraction of its speed, the first thing to know about a slow session. Checked
 * every 5 s from the flip; logged at start and on every change. */
static void power_tick(void)
{
    static DWORD t;
    static int last_ac = -1, last_saver = -1;
    SYSTEM_POWER_STATUS ps;
    DWORD now = GetTickCount();
    if (t && now - t < 5000) return;
    t = now;
    if (!GetSystemPowerStatus(&ps)) return;
    if (ps.ACLineStatus != last_ac || (int)ps.SystemStatusFlag != last_saver) {
        last_ac = ps.ACLineStatus;
        last_saver = ps.SystemStatusFlag;
        fprintf(stderr, "[POWER] %s, battery %d%%%s\n",
                ps.ACLineStatus == 1 ? "on mains power" : ps.ACLineStatus == 0 ? "ON BATTERY" : "power source unknown",
                ps.BatteryLifePercent == 255 ? 0 : ps.BatteryLifePercent,
                ps.SystemStatusFlag ? ", battery saver on" : "");
        {
            /* Unplugged during play: Auto comes down to the battery
             * ceiling at once (it is not raised again on mains; the next
             * start picks the full size). */
            float cap = power_cap();
            if (g_pc.render_scale == 0 && cap > 0.0f && s_auto_sc > cap) {
                s_auto_sc = cap;
                fprintf(stderr, "[D3D11] render resolution auto: down to %.1fx on battery\n", cap);
            }
        }
    }
}

static void auto_scale_tick(void)
{
    static LARGE_INTEGER freq, last;
    static float iv[180];
    static int n, drops;
    LARGE_INTEGER now;
    if (g_pc.render_scale != 0 || drops >= 3 || s_auto_sc <= 1.0f) return;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (last.QuadPart && !pc_menu_is_open() && s_fl_disp >= 40) {   /* 3D frames only: films and menus may run at 30 */
        iv[n++] = (float)((now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart);
        if (n == 180) {
            float s[180], t;
            int i, j, slow = 0;
            memcpy(s, iv, sizeof s);
            for (i = 1; i < 180; i++) { t = s[i]; for (j = i; j > 0 && s[j - 1] > t; j--) s[j] = s[j - 1]; s[j] = t; }
            for (i = 0; i < 180; i++) if (s[i] > 20.0f) slow++;
            if (s[90] > 18.0f || slow >= 36) {
                s_auto_sc -= 0.5f; drops++;
                if (s_auto_sc < 1.0f) s_auto_sc = 1.0f;
                fprintf(stderr, "[D3D11] render resolution auto: median frame %.1f ms, %d of 180 over 20 ms, down to %.1fx\n",
                        s[90], slow, s_auto_sc);
            }
            n = 0;
        }
    }
    last = now;
}

/* Render Resolution (settings menu, Graphics): every target and depth
 * buffer is allocated at a multiple of the game's size and drawn with a
 * viewport to match. Positions reach the shaders normalised by the game's
 * size (rt.x = 1/w), so geometry, clears and texture coordinates follow
 * without change; only the pixel count grows. Kept under 8192 a side. */
/* render_scale: 0 = Auto; 1..4 = that multiple; 240.. = an output height
 * (720 = 720p: the game's 480 lines drawn as 720, scale 1.5); -1 = Native,
 * the window's own height (2026-10-22, owner: "add more resolutions"). The
 * window height is read at each flip (s_native_h), so Native follows a resize:
 * every target is then made again at the new scale. */
static volatile uint32_t s_native_h = 480;
static float rt_scale(uint32_t w, uint32_t h)
{
    int v = g_pc.render_scale;
    float sc;
    if (v == 0) sc = auto_scale();
    else if (v < 0) sc = (float)s_native_h / 480.0f;
    else if (v <= 4) sc = (float)v;
    else sc = (float)v / 480.0f;
    sc = (float)(int)(sc * 64.0f + 0.5f) / 64.0f;   /* a few distinct values, not one per window pixel */
    if (sc < 1.0f) sc = 1.0f;
    while (sc > 1.0f && (SCL(w, sc) > 8192 || SCL(h, sc) > 8192)) sc -= 0.25f;
    if (sc < 1.0f) sc = 1.0f;
    return sc;
}

static RenderTarget *get_rt(const Nv2aSurface *s)
{
    uint32_t key = PHYS(s->color_va), w, h;
    float sc;
    int i;
    D3D11_TEXTURE2D_DESC td;
    RenderTarget *r;

    surface_dims(s, &w, &h);
    sc = rt_scale(w, h);
    for (i = 0; i < s_nrt; i++)
        if (s_rt[i].key == key && s_rt[i].w == w && s_rt[i].sc == sc
                && (s_rt[i].h >= h || (w >= 320 && s_rt[i].h >= (w * 3) / 4)))
            return &s_rt[i];   /* a taller target serves a shorter clip; a screen-sized
                                * buffer keeps its 4:3 rows and clips a taller clip */
    for (i = 0; i < s_nrt; i++)
        if (s_rt[i].key == key && s_rt[i].w == w && s_rt[i].sc == sc && s_rt[i].h < h) {
            /* Same buffer, taller clip: grow it and keep the picture. */
            RenderTarget *g = &s_rt[i];
            ID3D11Texture2D *nt = NULL;
            ID3D11RenderTargetView *nr = NULL;
            ID3D11ShaderResourceView *ns = NULL;
            D3D11_BOX box;
            float black[4] = { 0, 0, 0, 0 };
            memset(&td, 0, sizeof td);
            td.Width = SCL(w, sc); td.Height = SCL(h, sc); td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &nt)))
                return g;
            ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)nt, NULL, &nr);
            ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)nt, NULL, &ns);
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, nr, black);
            box.left = 0; box.top = 0; box.front = 0; box.back = 1;
            box.right = SCL(g->w, sc); box.bottom = SCL(g->h, sc);
            ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)nt, 0, 0, 0, 0,
                                                      (ID3D11Resource *)g->tex, 0, &box);
            ID3D11ShaderResourceView_Release(g->srv);
            ID3D11RenderTargetView_Release(g->rtv);
            ID3D11Texture2D_Release(g->tex);
            g->tex = nt; g->rtv = nr; g->srv = ns; g->h = h;
            fprintf(stderr, "[D3D11] render target %08X grown to %ux%u (x%.2f), picture kept\n",
                    s->color_va, w, h, sc);
            return g;
        }
    for (i = 0; i < s_nrt; i++)              /* same address, new size: replace */
        if (s_rt[i].key == key)
            break;
    if (i == s_nrt) {
        if (s_nrt == MAX_RT) i = 0; else s_nrt++;
    }
    r = &s_rt[i];
    if (r->srv) ID3D11ShaderResourceView_Release(r->srv);
    if (r->rtv) ID3D11RenderTargetView_Release(r->rtv);
    if (r->tex) ID3D11Texture2D_Release(r->tex);
    memset(r, 0, sizeof *r);
    memset(&td, 0, sizeof td);
    td.Width = SCL(w, sc); td.Height = SCL(h, sc); td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->tex)))
        return NULL;
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)r->tex, NULL, &r->rtv);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)r->tex, NULL, &r->srv);
    {
        float black[4] = { 0, 0, 0, 0 };
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, r->rtv, black);
    }
    r->key = key; r->w = w; r->h = h; r->sc = sc;
    fprintf(stderr, "[D3D11] render target %08X %ux%u (x%.2f)\n", s->color_va, w, h, sc);
    fflush(stderr);
    return r;
}

static RenderTarget *find_rt_by_va(uint32_t va)
{
    int i;
    for (i = 0; i < s_nrt; i++)
        if (s_rt[i].key == PHYS(va))
            return &s_rt[i];
    return NULL;
}

static DepthTarget *get_dt(uint32_t zva, uint32_t w, uint32_t h)
{
    uint32_t key = PHYS(zva);
    int i;
    D3D11_TEXTURE2D_DESC td;
    DepthTarget *d;

    float sc = rt_scale(w, h);
    for (i = 0; i < s_ndt; i++)
        if (s_dt[i].key == key && s_dt[i].w == w && s_dt[i].h == h && s_dt[i].sc == sc)
            return &s_dt[i];
    if (s_ndt == 32) i = 0; else i = s_ndt++;
    d = &s_dt[i];
    if (d->dsv) ID3D11DepthStencilView_Release(d->dsv);
    if (d->tex) ID3D11Texture2D_Release(d->tex);
    memset(d, 0, sizeof *d);
    memset(&td, 0, sizeof td);
    td.Width = SCL(w, sc); td.Height = SCL(h, sc); td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &d->tex)))
        return NULL;
    ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)d->tex, NULL, &d->dsv);
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, d->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    d->key = key; d->w = w; d->h = h; d->sc = sc;
    return d;
}

/* ── textures ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t key, w, h, color, pitch;
    uint32_t hash, checked_frame;
    uint32_t lv, dxgi;              /* mip levels and format of tex */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} TexEntry;

#define MAX_TEX 1024
static TexEntry s_tex[MAX_TEX];
static int s_ntex, s_tex_next;
static uint32_t s_tex_created, s_tex_uploads;
static uint64_t s_tex_bytes_created;
static uint32_t *s_decode;
static size_t s_decode_cap;

static int is_linear(uint32_t fmt)
{
    return (fmt >= 0x10 && fmt <= 0x13) || (fmt >= 0x1B && fmt <= 0x1F)
        || fmt == 0x16 || fmt == 0x17 || fmt == 0x30 || fmt == 0x31;
}

/* Bytes of level 0 in guest memory, for the change hash. */
static size_t tex_bytes(const Nv2aTexture *t)
{
    size_t px = (size_t)t->width * t->height;
    if (is_linear(t->color))
        return (size_t)(t->pitch ? t->pitch : t->width * 4) * t->height;
    switch (t->color) {
    case 0x0C: return px / 2;                         /* DXT1 */
    case 0x0E: case 0x0F: return px;                  /* DXT3, DXT5 */
    case 0x00: case 0x01: case 0x0B: case 0x19: return px;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x1A: return px * 2;
    default: return px * 4;
    }
}

/* `full`: every word. Linear (row-order) textures are the dynamic ones --
 * Bink writes only the blocks that changed into its Y/U/V planes, and a
 * sampled hash missed those updates (speckled movies). Swizzled and DXT
 * textures are the static assets, and there are hundreds per frame, so they
 * keep the sampled hash. */
static uint32_t hash_guest(uint32_t va, size_t n, int full)
{
    const uint8_t *p = (const uint8_t *)xbox_GetMemoryOffset() + va;
    uint32_t h = 2166136261u;
    /* 52, not 64 (2026-10-25, owner: a chapter card showed the save screen's
     * forest instead of its picture): DXT5 keeps 16-byte blocks whose first
     * 4 bytes are alpha, and a step of 64 read only those. Every fully opaque
     * DXT5 picture (alpha FF FF, indices 0) hashed the same, so a new card
     * loaded where an older picture of the same size had been kept the old
     * one. 52 is 4 mod 16: the samples walk through all four words of the
     * blocks (alpha, colour endpoints, colour indices). */
    size_t i, step = (!full && n > 65536) ? 52 : 4;
    if (!n) return 0;
    for (i = 0; i + 4 <= n; i += step) {
        h ^= *(const uint32_t *)(p + i);
        h *= 16777619u;
    }
    h ^= (uint32_t)n;
    return h;
}

/* KNOWN_ISSUES #46 diagnostics: OOGIE_FOG_TRACE=1 prints one [FOG] line per
 * frame (draws by path, mean per-vertex fog factor of CPU-path draws, the
 * fog settings and a hash of the constants of the largest vertex-program
 * batch), and [SHOT] lines map saved BMPs to frame numbers. */
static int s_fog_trace = -1;
static struct {
    uint32_t n_cpu, v_cpu, n_vsh, n_vsh_fog, big_n, big_hash, modes[3], nmodes, n_norc, shown;
    uint32_t dkey[6], dcnt[6], ndraw;       /* surfaces drawn into, draws each */
    uint32_t skey[6], scnt[6], nsamp;       /* surfaces sampled as textures */
    uint32_t npost, zeta; char post[24 * 56];   /* post-processing steps, scene depth buffer */
    uint32_t fkey[6], fcnt[6], nfog;        /* fog colours (RGB) used, draws each */
    uint32_t measure, pre_done;             /* OOGIE_FLASH_MEASURE=1 */
    uint32_t n_white, white_key, white_fmt, n_upload;   /* stand-in textures, re-decodes */
    float pre_mean;                         /* scene brightness before post-processing */
    double fog_sum;
    float fp[3][2];
} s_ft;

static float rt_mean(const RenderTarget *r);

/* #46: per-draw state of every draw (and clear) for the last six frames, and
 * the brightness of each presented picture, read back asynchronously three
 * frames later (no pipeline stall: a stall made the flashes rarer). When a
 * picture's brightness differs from both neighbours (a flash),
 * logs/drawdiff_<frame>.txt lists the draws whose state differs from the frame
 * before. OOGIE_FLASH_MEASURE=1 (with OOGIE_FOG_TRACE=1). */
typedef struct {
    uint32_t tgt, tex, texfmt, texhash, rc, fogc, blend, vs, vc, n, misc, fog;
} DrawRec;
#define DREC_MAX 8192
#define DREC_SLOTS 6
static DrawRec *s_drec[DREC_SLOTS];
static uint32_t s_drec_n[DREC_SLOTS], s_drec_frame[DREC_SLOTS];
static float s_drec_fin[DREC_SLOTS];
static uint32_t s_last_tex_hash, s_dr_vs, s_dr_vc, s_dr_n, s_dr_fog;
static int s_flash_rec = -1, s_flash_vc, s_flash_meas_log;

/* OOGIE_FLASH_MEASURE=1: the draw records and the asynchronous brightness,
 * nothing logged per frame (logging and hashing slowed the renderer enough
 * to make the flashes rare); OOGIE_FLASH_VC=1 also hashes every draw's
 * vertex constants; with OOGIE_FOG_TRACE=1 each frame's brightness is logged. */
static int flash_rec_on(void)
{
    if (s_flash_rec < 0) {
        const char *e = getenv("OOGIE_FLASH_MEASURE");
        s_flash_rec = (e && *e && *e != '0') ? 1 : 0;
        e = getenv("OOGIE_FLASH_VC");
        s_flash_vc = e && *e && *e != '0';
        e = getenv("OOGIE_FOG_TRACE");
        s_flash_meas_log = e && *e && *e != '0';
    }
    return s_flash_rec;
}

static void drawrec_add(const DrawRec *d)
{
    int k = (int)(s_frame % DREC_SLOTS);
    if (!s_drec[0]) {
        int j;
        for (j = 0; j < DREC_SLOTS; j++) s_drec[j] = (DrawRec *)calloc(DREC_MAX, sizeof(DrawRec));
        if (!s_drec[DREC_SLOTS - 1]) return;
    }
    if (s_drec_frame[k] != s_frame) { s_drec_frame[k] = s_frame; s_drec_n[k] = 0; s_drec_fin[k] = -1; }
    if (s_drec_n[k] < DREC_MAX) s_drec[k][s_drec_n[k]++] = *d;
}

static void drawrec_dump(uint32_t g)
{
    int p = (int)(g % DREC_SLOTS), q = (int)((g - 1) % DREC_SLOTS), r = (int)((g + 1) % DREC_SLOTS);
    static int dumps;
    char path[64];
    FILE *f;
    uint32_t i, nb = s_drec_n[p], na = s_drec_n[q], diffs = 0;
    if (s_drec_frame[p] != g || s_drec_frame[q] != g - 1)
        return;
    if (dumps >= 200) {               /* still counted, no more files */
        fprintf(stderr, "[FLASH] frame %u brightness %.1f vs %.1f/%.1f (not dumped)\n", g,
                s_drec_fin[p], s_drec_fin[q], s_drec_fin[r]);
        return;
    }
    snprintf(path, sizeof path, "logs/drawdiff_%u.txt", g);
    if (!(f = fopen(path, "w")))
        return;
    dumps++;
    {
        const char *nm[12] = { "tgt", "tex", "texfmt", "texhash", "rc", "fogc", "blend",
                               "vs", "vc", "n", "misc", "fog" };
        fprintf(f, "flash frame %u: brightness %.1f (before %.1f, after %.1f); draws %u (before %u)\n",
                g, s_drec_fin[p], s_drec_fin[q], s_drec_fin[r], nb, na);
        for (i = 0; i < nb && i < na; i++) {
            const uint32_t *x = (const uint32_t *)&s_drec[p][i];
            const uint32_t *y = (const uint32_t *)&s_drec[q][i];
            int k, any = 0;
            for (k = 0; k < 12; k++)
                if (k != 8 && x[k] != y[k]) any = 1;          /* vc: changes every frame */
            if (!any) continue;
            if (++diffs > 400) continue;
            fprintf(f, "draw %u:", i);
            for (k = 0; k < 12; k++)
                if (x[k] != y[k]) fprintf(f, " %s %X->%X", nm[k], y[k], x[k]);
            fprintf(f, "\n");
        }
        fprintf(f, "%u draws differ (constants hash ignored)\n", diffs);
        {   /* all three frames, for scripts/drawdiff.py (before, flash, after) */
            int fr[3], j;
            fr[0] = q; fr[1] = p; fr[2] = r;
            for (j = 0; j < 3; j++) {
                fprintf(f, "-- frame %u (tgt tex texfmt texhash rc fogc blend vs vc n misc fog):\n",
                        s_drec_frame[fr[j]]);
                for (i = 0; i < s_drec_n[fr[j]]; i++) {
                    const uint32_t *x = (const uint32_t *)&s_drec[fr[j]][i];
                    int k;
                    fprintf(f, "%u:", i);
                    for (k = 0; k < 12; k++) fprintf(f, " %X", x[k]);
                    fprintf(f, "\n");
                }
            }
        }
    }
    fclose(f);
    fprintf(stderr, "[FLASH] frame %u brightness %.1f vs %.1f/%.1f: %s\n", g, s_drec_fin[p],
            s_drec_fin[q], s_drec_fin[r], path);
    fflush(stderr);
}

/* At each flip, after Present: start the readback of this picture and finish
 * the one from three frames ago. */
static void drawrec_flip(ID3D11Texture2D *shown, uint32_t w, uint32_t h)
{
    static ID3D11Texture2D *stg[3];
    static uint32_t stg_frame[3], stg_w, stg_h;
    int i = (int)(s_frame % 3);
    if (!shown) return;
    if (stg_w != w || stg_h != h) {
        int k;
        for (k = 0; k < 3; k++) { if (stg[k]) ID3D11Texture2D_Release(stg[k]); stg[k] = NULL; stg_frame[k] = 0; }
        stg_w = w; stg_h = h;
    }
    if (stg[i] && stg_frame[i]) {                         /* frame s_frame - 3 */
        D3D11_MAPPED_SUBRESOURCE m;
        uint32_t f0 = stg_frame[i] - 1;
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stg[i], 0, D3D11_MAP_READ, 0, &m))) {
            double sum = 0; uint32_t x, y, n = 0;
            for (y = 0; y < h; y += 8)
                for (x = 0; x < w; x += 8) {
                    const uint8_t *px = (const uint8_t *)m.pData + (size_t)y * m.RowPitch + x * 4;
                    sum += (px[0] + px[1] + px[2]) / 3.0; n++;
                }
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stg[i], 0);
            if (s_drec[0] && s_drec_frame[f0 % DREC_SLOTS] == f0)
                s_drec_fin[f0 % DREC_SLOTS] = n ? (float)(sum / n) : -1;
            if (s_flash_meas_log)
                fprintf(stderr, "[MEAS] f=%u final=%.1f\n", f0, n ? sum / n : -1.0);
            /* f0 - 1 is a flash if it differs from both f0 - 2 and f0 */
            if (s_drec[0] && f0 >= 2) {
                uint32_t g = f0 - 1;
                float a = s_drec_fin[(g - 1) % DREC_SLOTS], b = s_drec_fin[g % DREC_SLOTS],
                      c = s_drec_fin[f0 % DREC_SLOTS];
                /* the frame after a flash is off too (it blends the previous
                 * picture in), so the neighbours only need to be closer to
                 * each other than to the flash */
                if (a >= 0 && b >= 0 && c >= 0 && s_drec_frame[g % DREC_SLOTS] == g
                        && fabsf(b - a) > 12 && fabsf(b - c) > 8
                        && fabsf(a - c) < 0.5f * fminf(fabsf(b - a), fabsf(b - c)))
                    drawrec_dump(g);
            }
        }
    }
    if (!stg[i]) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(shown, &td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &stg[i]))) return;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stg[i], (ID3D11Resource *)shown);
    stg_frame[i] = s_frame + 1;
}

static int fog_trace_on(void)
{
    if (s_fog_trace < 0) { const char *e = getenv("OOGIE_FOG_TRACE"); s_fog_trace = e && *e && *e != '0'; }
    return s_fog_trace;
}

static float s_ft_big_c[192][4];
static uint32_t s_ft_big_prog, s_ft_big_ninsn;

static void fog_trace_flip(void)
{
    uint32_t i;
    if (!fog_trace_on()) return;
    {
        static FILE *df; static int tried;
        if (!tried) { const char *p = getenv("OOGIE_VC_DUMP"); tried = 1; if (p) df = fopen(p, "wb"); }
        if (df) {
            uint32_t hdr[4];
            hdr[0] = s_frame; hdr[1] = s_ft.big_n; hdr[2] = s_ft_big_prog; hdr[3] = s_ft_big_ninsn;
            fwrite(hdr, sizeof hdr, 1, df);
            fwrite(s_ft_big_c, sizeof s_ft_big_c, 1, df);
            fflush(df);
        }
    }
    fprintf(stderr, "[FOG] f=%u cpu=%u/%uv avg=%.3f vsh=%u fogon=%u norc=%u shown=%08X big=%u/%08X modes",
            s_frame, s_ft.n_cpu, s_ft.v_cpu, s_ft.v_cpu ? s_ft.fog_sum / s_ft.v_cpu : 0.0,
            s_ft.n_vsh, s_ft.n_vsh_fog, s_ft.n_norc, s_ft.shown, s_ft.big_n, s_ft.big_hash);
    for (i = 0; i < s_ft.nmodes; i++)
        fprintf(stderr, " %X:%.4g,%.4g", s_ft.modes[i], s_ft.fp[i][0], s_ft.fp[i][1]);
    fprintf(stderr, " draw");
    for (i = 0; i < s_ft.ndraw; i++) fprintf(stderr, " %X:%u", s_ft.dkey[i], s_ft.dcnt[i]);
    fprintf(stderr, " samp");
    for (i = 0; i < s_ft.nsamp; i++) fprintf(stderr, " %X:%u", s_ft.skey[i], s_ft.scnt[i]);
    fprintf(stderr, " fogc");
    for (i = 0; i < s_ft.nfog; i++) fprintf(stderr, " %06X:%u", s_ft.fkey[i], s_ft.fcnt[i]);
    fprintf(stderr, " zeta=%X white=%u/%X/%X upl=%u", s_ft.zeta, s_ft.n_white, s_ft.white_key,
            s_ft.white_fmt, s_ft.n_upload);
    if (s_ft.pre_done)
        fprintf(stderr, " pre=%.1f", s_ft.pre_mean);
    fprintf(stderr, " post%s\n", s_ft.post);
    memset(&s_ft, 0, sizeof s_ft);
    {
        static int m = -1;
        if (m < 0) { const char *e = getenv("OOGIE_FLASH_MEASURE"); m = (e && *e) ? atoi(e) : 0; }
        s_ft.measure = (uint32_t)m;
    }
}

/* Mean brightness (0-255) of a render target, read back through a staging
 * copy. Stalls the pipeline: diagnostics only. */
static float rt_mean(const RenderTarget *r)
{
    static ID3D11Texture2D *st;
    static uint32_t sw, sh;
    D3D11_MAPPED_SUBRESOURCE m;
    double sum = 0;
    uint32_t x, y, n = 0;
    if (!r || !r->tex) return -1;
    D3D11_TEXTURE2D_DESC rd;
    ID3D11Texture2D_GetDesc(r->tex, &rd);
    if (!st || sw != rd.Width || sh != rd.Height) {
        D3D11_TEXTURE2D_DESC td;
        if (st) { ID3D11Texture2D_Release(st); st = NULL; }
        ID3D11Texture2D_GetDesc(r->tex, &td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st))) return -1;
        sw = rd.Width; sh = rd.Height;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)r->tex);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m)))
        return -1;
    for (y = 0; y < sh; y += 8)
        for (x = 0; x < sw; x += 8) {
            const uint8_t *p = (const uint8_t *)m.pData + (size_t)y * m.RowPitch + x * 4;
            sum += (p[0] + p[1] + p[2]) / 3.0; n++;
        }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    return n ? (float)(sum / n) : -1;
}

static void fog_trace_key(uint32_t *keys, uint32_t *cnt, uint32_t *n, uint32_t key)
{
    uint32_t i;
    for (i = 0; i < *n; i++)
        if (keys[i] == key) { cnt[i]++; return; }
    if (*n < 6) { keys[*n] = key; cnt[*n] = 1; (*n)++; }
}

static void fog_trace_mode(uint32_t en, uint32_t mode, float p0, float p1)
{
    uint32_t i, m = en ? mode : 0;
    for (i = 0; i < s_ft.nmodes; i++)
        if (s_ft.modes[i] == m && s_ft.fp[i][0] == p0 && s_ft.fp[i][1] == p1) return;
    if (s_ft.nmodes < 3) {
        s_ft.modes[s_ft.nmodes] = m; s_ft.fp[s_ft.nmodes][0] = p0; s_ft.fp[s_ft.nmodes][1] = p1;
        s_ft.nmodes++;
    }
}

/* Guest bytes under a render target, sampled like a static texture. */
static uint32_t rt_guest_hash(const RenderTarget *r)
{
    size_t n = (size_t)r->w * r->h * 4;
    if ((size_t)r->key + n > 0x04000000u)
        n = r->key < 0x04000000u ? 0x04000000u - r->key : 0;
    return hash_guest(r->key, n, 0);
}

/* KNOWN_ISSUES #49: a render target lives on after the game stops drawing
 * into it, and get_texture answers any texture at its address with it. The
 * game loads each chapter's title-card picture at an address it drew into
 * before, so a continuous session showed the old picture. A target never
 * presented and not drawn in the last eight frames whose guest memory changed
 * since its last draw (something else, normally a texture load, wrote there)
 * is dropped and the texture decoded from guest memory instead. Hypothesis
 * for #49 until a continuous Chapter 1 -> 2 session confirms it.
 * OOGIE_KEEP_STALE_RT=1: the old behaviour. */
static int rt_replaced(RenderTarget *r)
{
    static int keep = -1;
    uint32_t h;
    if (keep < 0) { const char *e = getenv("OOGIE_KEEP_STALE_RT"); keep = e && *e && *e != '0'; }
    if (keep || r->baseline != 2 || s_frame + 1 - r->drawn_frame <= 8)
        return 0;
    /* Never a surface that was ever presented: the display buffers (rt1:
     * 0x35C000, sampled three frames after its draw, had different guest
     * bytes -- the game reads back its previous frames). */
    if (r->shown_frame || r == s_shown_rt || r == s_frame_rt || r == s_frame_chain)
        return 0;
    if (r->checked_frame == s_frame + 1)
        return 0;
    r->checked_frame = s_frame + 1;
    h = rt_guest_hash(r);
    if (h == r->guest_hash)
        return 0;
    fprintf(stderr, "[D3D11] render target %08X %ux%u (last drawn %u frames ago) holds"
                    " new guest data: sampled as a texture from now on\n",
            r->key, r->w, r->h, s_frame + 1 - r->drawn_frame);
    fflush(stderr);
    if (r->srv) ID3D11ShaderResourceView_Release(r->srv);
    if (r->rtv) ID3D11RenderTargetView_Release(r->rtv);
    if (r->tex) ID3D11Texture2D_Release(r->tex);
    memset(r, 0, sizeof *r);
    return 1;
}

/* ── texture packs (dev cycle 4) ──────────────────────────────────
 * A texture is named by its size and a 64-bit FNV-1a hash of its decoded
 * pixels ("256x256_0123456789abcdef.png"), so the same picture gets the same
 * name however the game stores it. Mods > Save Game Textures writes each
 * texture once to texture_dump\ (PNG with alpha, on a background thread). A
 * mod's textures\ folder (mods\<mod>\textures\<name>.png, any size) replaces
 * the texture of that name. Linear textures (films, render copies) and
 * render targets are left alone; films are replaced whole as .bik files. */
typedef struct {
    uint64_t hash;
    uint32_t w, h;
    char path[MAX_PATH];
    ID3D11ShaderResourceView *srv;
    int tried;
} TexRepl;
static TexRepl *s_repl;
static int s_nrepl;
static volatile LONG s_tex_rescan = 1;      /* read the packs before the first texture */
static uint32_t s_repl_used;

/* The Mods page switched a mod: read the texture packs again. */
void oogie_textures_rescan(void) { InterlockedExchange(&s_tex_rescan, 1); }

static uint64_t tex_content_hash(const uint32_t *px, uint32_t w, uint32_t h)
{
    const uint8_t *p = (const uint8_t *)px;
    size_t n = (size_t)w * h * 4, i;
    uint64_t x = 1469598103934665603ull;
    for (i = 0; i < n; i++) x = (x ^ p[i]) * 1099511628211ull;
    x = (x ^ w) * 1099511628211ull;
    return (x ^ h) * 1099511628211ull;
}

static int repl_cmp(const void *a, const void *b)
{
    uint64_t x = ((const TexRepl *)a)->hash, y = ((const TexRepl *)b)->hash;
    return x < y ? -1 : x > y;
}

static void tex_cache_flush(void)
{
    int i;
    for (i = 0; i < s_ntex; i++) {
        if (s_tex[i].srv) ID3D11ShaderResourceView_Release(s_tex[i].srv);
        if (s_tex[i].tex) ID3D11Texture2D_Release(s_tex[i].tex);
    }
    memset(s_tex, 0, sizeof(TexEntry) * (size_t)s_ntex);
    s_ntex = 0; s_tex_next = 0;
}

static void repl_index(void)
{
    WIN32_FIND_DATAA md, fd;
    HANDLE mh, fh;
    int i, cap = 0, mods = 0;
    for (i = 0; i < s_nrepl; i++) if (s_repl[i].srv) ID3D11ShaderResourceView_Release(s_repl[i].srv);
    free(s_repl); s_repl = NULL; s_nrepl = 0;
    if (getenv("OOGIE_NO_MODS") && *getenv("OOGIE_NO_MODS") == '1') return;
    mh = FindFirstFileA("mods\\*", &md);
    if (mh == INVALID_HANDLE_VALUE) return;
    do {
        char pat[MAX_PATH], chk[MAX_PATH];
        int before = s_nrepl;
        if (!(md.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || md.cFileName[0] == '.') continue;
        snprintf(chk, sizeof chk, "mods\\%s\\disabled", md.cFileName);
        if (GetFileAttributesA(chk) != INVALID_FILE_ATTRIBUTES) continue;
        snprintf(pat, sizeof pat, "mods\\%s\\textures\\*", md.cFileName);
        fh = FindFirstFileA(pat, &fd);
        if (fh == INVALID_HANDLE_VALUE) continue;
        do {
            unsigned w, h;
            unsigned long long hs;
            char ext[16];
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (sscanf(fd.cFileName, "%ux%u_%16llx.%15s", &w, &h, &hs, ext) != 4) continue;
            if (_stricmp(ext, "png") && _stricmp(ext, "dds")) continue;
            for (i = 0; i < s_nrepl; i++) if (s_repl[i].hash == hs) break;
            if (i < s_nrepl) continue;              /* a mod sorted earlier has it */
            if (s_nrepl == cap) {
                TexRepl *n2;
                cap = cap ? cap * 2 : 256;
                n2 = (TexRepl *)realloc(s_repl, sizeof(TexRepl) * (size_t)cap);
                if (!n2) break;
                s_repl = n2;
            }
            memset(&s_repl[s_nrepl], 0, sizeof(TexRepl));
            s_repl[s_nrepl].hash = hs; s_repl[s_nrepl].w = w; s_repl[s_nrepl].h = h;
            snprintf(s_repl[s_nrepl].path, MAX_PATH, "mods\\%s\\textures\\%s", md.cFileName, fd.cFileName);
            s_nrepl++;
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
        if (s_nrepl > before) mods++;
    } while (FindNextFileA(mh, &md));
    FindClose(mh);
    if (s_nrepl > 1) qsort(s_repl, (size_t)s_nrepl, sizeof(TexRepl), repl_cmp);
    if (s_nrepl)
        fprintf(stderr, "[TEXPACK] %d replacement texture%s in %d mod%s\n", s_nrepl, s_nrepl == 1 ? "" : "s",
                mods, mods == 1 ? "" : "s");
}

static ID3D11ShaderResourceView *repl_find(uint64_t ch, uint32_t w, uint32_t h)
{
    int lo = 0, hi = s_nrepl - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        TexRepl *r = &s_repl[mid];
        if (r->hash < ch) lo = mid + 1;
        else if (r->hash > ch) hi = mid - 1;
        else {
            if (r->w != w || r->h != h) return NULL;
            if (!r->srv && !r->tried) {
                unsigned iw = 0, ih = 0;
                unsigned char *px = pc_image_load_bgra(r->path, &iw, &ih);
                r->tried = 1;
                if (px) {
                    D3D11_TEXTURE2D_DESC td;
                    D3D11_SUBRESOURCE_DATA sd;
                    ID3D11Texture2D *tx = NULL;
                    memset(&td, 0, sizeof td);
                    td.Width = iw; td.Height = ih; td.MipLevels = td.ArraySize = 1;
                    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
                    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    sd.pSysMem = px; sd.SysMemPitch = iw * 4; sd.SysMemSlicePitch = 0;
                    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &tx))) {
                        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)tx, NULL, &r->srv);
                        ID3D11Texture2D_Release(tx);
                    }
                    free(px);
                }
                if (s_repl_used++ < 20 || !r->srv)
                    fprintf(stderr, "[TEXPACK] %s %s (%ux%u for %ux%u)\n", r->srv ? "using" : "could not read",
                            r->path, iw, ih, w, h);
            }
            return r->srv;
        }
    }
    return NULL;
}

/* Mods > Save Game Textures: each texture once to texture_dump\. */
static void tex_dump(uint64_t ch, const uint32_t *px, uint32_t w, uint32_t h)
{
    static uint64_t *seen;
    static uint32_t seen_n;
    char path[MAX_PATH];
    uint32_t k;
    if (!seen) {
        seen = (uint64_t *)calloc(65536, sizeof *seen);
        if (!seen) return;
        CreateDirectoryA("texture_dump", NULL);
    }
    for (k = (uint32_t)(ch & 65535u); seen[k]; k = (k + 1) & 65535u)
        if (seen[k] == ch) return;
    if (seen_n >= 60000) return;
    seen[k] = ch; seen_n++;
    snprintf(path, sizeof path, "texture_dump\\%ux%u_%016llx.png", w, h, (unsigned long long)ch);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    pc_png_queue_bgra(path, px, w, h, w * 4);
}

/* ── fast texture upload (dev cycle 6) ─────────────────────────────
 * The reference decoder (nv2a_backend_decode_texture) builds a texture one
 * texel at a time through the general sampler: ~260,000 calls for a 512x512,
 * and a level load brings in dozens at once (cpB8: 48 new textures in one
 * 239 ms frame). Here, in one pass each, with the reference decoder's own
 * arithmetic (d3d8_swizzle.h, nv2a_pb_exec.c sample_texture):
 *  - DXT1/3/5 decoded a block at a time (not a texel at a time);
 *  - swizzled A8R8G8B8 / X8R8G8B8 unswizzled with the runtime's masks;
 *  - LIN_L8 (the films' Y/U/V planes, every film frame) expanded;
 *  - other formats still through the reference decoder.
 * Mipmaps: the game asks for mip filtering on almost every texture (MIN
 * filter 6, LINEAR_MIPMAP_LINEAR: tf2, 147,255 of 152,379 checks) but its
 * textures have one level (levels 1 in all of them), so the Xbox sampled
 * level 0 and distant surfaces shimmered. With Graphics > Mipmaps on, those
 * textures get a full mip chain made on the GPU (GenerateMips). With it off,
 * DXT textures go to the GPU as they are (BC1/BC2/BC3: the NV2A's DXT is
 * plain S3TC, same DXT1 alpha rule), the least memory.
 * OOGIE_TEX_FAST=0: everything through the reference decoder;
 * OOGIE_TEX_VERIFY=1: check every fast decode against the reference decoder
 * and log [TEX] counts every 10 s (OOGIE_TEX_STATS=1: counts only). */
#include "../d3d/d3d8_swizzle.h"

static int tex_opt(const char *name, int def)
{
    const char *e = getenv(name);
    return e && *e ? *e != '0' : def;
}

static struct { uint32_t fast_bc, fast, slow, mipped, verified, mismatched; uint32_t slow_fmt[64], minf[16]; double ms, worst; } s_txs;

/* Full mip chain length when the game's MIN filter is a mip mode (3..6) and
 * mipmaps are on; 1 otherwise. */
static uint32_t tex_mips_wanted(const Nv2aTexture *t)
{
    uint32_t min = (t->filter >> 16) & 0xFF, full = 1, w = t->width, h = t->height;
    s_txs.minf[min & 15]++;
    if (!g_pc.mipmaps || is_linear(t->color) || min < 3 || min > 6) return 1;
    while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; full++; }
    return full;
}

static uint32_t dxt_block(uint32_t fmt)
{
    return fmt == 0x0C ? 8 : (fmt == 0x0E || fmt == 0x0F) ? 16 : 0;
}

static int is_pow2(uint32_t v) { return v && !(v & (v - 1)); }

/* One DXT block into 16 ARGB texels, as d3d8_dxt_decode_texel does it. */
static void dxt_block_decode(const uint8_t *b, uint32_t fmt, uint32_t px[16])
{
    uint32_t c[4], a[8], c0, c1, i, idx;
    uint64_t abits = 0;
    int bb16 = fmt != 0x0C;
    const uint8_t *cb = bb16 ? b + 8 : b;
    if (fmt == 0x0F) {
        a[0] = b[0]; a[1] = b[1];
        for (i = 0; i < 6; i++) abits |= (uint64_t)b[2 + i] << (8 * i);
        if (a[0] > a[1])
            for (i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
        else {
            for (i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
            a[6] = 0; a[7] = 255;
        }
    }
    c0 = (uint32_t)cb[0] | ((uint32_t)cb[1] << 8);
    c1 = (uint32_t)cb[2] | ((uint32_t)cb[3] << 8);
    for (i = 0; i < 2; i++) {
        uint32_t rgb = i ? c1 : c0;
        c[i] = (d3d8_expand_channel((rgb >> 11) & 0x1Fu, 5) << 16)
             | (d3d8_expand_channel((rgb >> 5) & 0x3Fu, 6) << 8)
             | d3d8_expand_channel(rgb & 0x1Fu, 5);
    }
    c[2] = c[3] = 0;
    if (c0 > c1 || bb16) {
        for (i = 0; i < 3; i++) {
            uint32_t sh = i * 8, m0 = (c[0] >> sh) & 0xFFu, m1 = (c[1] >> sh) & 0xFFu;
            c[2] |= ((2 * m0 + m1) / 3) << sh;
            c[3] |= ((m0 + 2 * m1) / 3) << sh;
        }
    } else {
        for (i = 0; i < 3; i++) {
            uint32_t sh = i * 8, m0 = (c[0] >> sh) & 0xFFu, m1 = (c[1] >> sh) & 0xFFu;
            c[2] |= ((m0 + m1) / 2) << sh;
        }
    }
    idx = (uint32_t)cb[4] | ((uint32_t)cb[5] << 8) | ((uint32_t)cb[6] << 16) | ((uint32_t)cb[7] << 24);
    for (i = 0; i < 16; i++) {
        uint32_t k = (idx >> (2 * i)) & 3u, al = 255u;
        if (fmt == 0x0E) {
            uint32_t nib = b[i >> 1];
            al = d3d8_expand_channel((i & 1u) ? (nib >> 4) : (nib & 0x0Fu), 4);
        } else if (fmt == 0x0F)
            al = a[(abits >> (3 * i)) & 7u];
        else if (c0 <= c1 && k == 3)
            al = 0;
        px[i] = (al << 24) | (c[k] & 0x00FFFFFFu);
    }
}

/* Level 0 of t into px (BGRA, width x height). */
static int tex_decode_fast(const Nv2aTexture *t, uint32_t *px)
{
    static int fast = -1, verify = -1;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset() + t->offset;
    uint32_t x, y, w = t->width, h = t->height;
    int done = 0;
    if (fast < 0) fast = tex_opt("OOGIE_TEX_FAST", 1);
    if (verify < 0) verify = tex_opt("OOGIE_TEX_VERIFY", 0);
    if (!fast) {
    } else if (dxt_block(t->color) && !(w & 3) && !(h & 3)) {
        uint32_t bb = dxt_block(t->color), blk[16], i;
        for (y = 0; y < h; y += 4)
            for (x = 0; x < w; x += 4) {
                dxt_block_decode(mem + ((size_t)(y / 4) * (w / 4) + x / 4) * bb, t->color, blk);
                for (i = 0; i < 16; i++) px[(size_t)(y + (i >> 2)) * w + x + (i & 3)] = blk[i];
            }
        done = 1;
    } else if ((t->color == 0x06 || t->color == 0x07) && is_pow2(w) && is_pow2(h)) {
        const uint32_t *src = (const uint32_t *)mem;
        uint32_t mx, my, ox, oy = 0, or_a = t->color == 0x07 ? 0xFF000000u : 0;
        xbox_swizzle_masks(w, h, &mx, &my);
        for (y = 0; y < h; y++) {
            uint32_t *d = px + (size_t)y * w;
            for (x = 0, ox = 0; x < w; x++) { d[x] = src[ox | oy] | or_a; ox = (ox - mx) & mx; }
            oy = (oy - my) & my;
        }
        done = 1;
    } else if (t->color == 0x13 && t->pitch >= w) {
        for (y = 0; y < h; y++) {
            const uint8_t *s = mem + (size_t)y * t->pitch;
            uint32_t *d = px + (size_t)y * w;
            for (x = 0; x < w; x++) d[x] = 0xFF000000u | (uint32_t)s[x] * 0x010101u;
        }
        done = 1;
    }
    if (!done) {
        s_txs.slow++;
        s_txs.slow_fmt[t->color & 63]++;
        return nv2a_backend_decode_texture(t, px);
    }
    s_txs.fast++;
    if (verify) {
        uint32_t *ref = (uint32_t *)malloc((size_t)w * h * 4);
        if (ref && nv2a_backend_decode_texture(t, ref)) {
            s_txs.verified++;
            if (memcmp(ref, px, (size_t)w * h * 4) && s_txs.mismatched++ < 8)
                fprintf(stderr, "[TEX] fast decode differs: 0x%08X %ux%u format 0x%02X\n",
                        t->offset, w, h, t->color);
        }
        free(ref);
    }
    return 1;
}

/* Make (or remake) e->tex with `nlv` levels of `fmt` (more than one level:
 * a render-target texture the GPU can fill the levels of). */
static int tex_alloc(TexEntry *e, const Nv2aTexture *t, uint32_t nlv, DXGI_FORMAT fmt)
{
    D3D11_TEXTURE2D_DESC td;
    if (e->tex && e->lv == nlv && e->dxgi == (uint32_t)fmt) return 1;
    if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
    if (e->tex) ID3D11Texture2D_Release(e->tex);
    e->srv = NULL; e->tex = NULL;
    memset(&td, 0, sizeof td);
    td.Width = t->width; td.Height = t->height; td.MipLevels = nlv; td.ArraySize = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (nlv > 1) {
        td.BindFlags |= D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    }
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &e->tex))) { e->tex = NULL; return 0; }
    if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex, NULL, &e->srv))) {
        ID3D11Texture2D_Release(e->tex); e->tex = NULL; e->srv = NULL; return 0;
    }
    e->lv = nlv; e->dxgi = (uint32_t)fmt;
    {
        /* OOGIE_TEX_TRACE=1: every texture made, for missing-picture reports
         * (owner 2026-10-25: a chapter card without its picture). */
        static int tr = -1;
        if (tr < 0) tr = tex_opt("OOGIE_TEX_TRACE", 0);
        if (tr) fprintf(stderr, "[TEX] made 0x%08X %ux%u format 0x%02X filter 0x%08X levels %u -> dxgi %u x%u (frame %u)\n",
                        t->offset, t->width, t->height, t->color, t->filter, t->levels, (unsigned)fmt, nlv, s_frame);
    }
    s_tex_created++;
    s_tex_bytes_created += (uint64_t)t->width * t->height * (fmt == DXGI_FORMAT_B8G8R8A8_UNORM ? (nlv > 1 ? 5 : 4) : 1);
    return 1;
}

/* Upload t into e. level0: level 0 already decoded (texture dump / pack
 * path), or NULL. Returns 0 when it cannot be decoded. */
static int tex_upload_(TexEntry *e, const Nv2aTexture *t, const uint32_t *level0)
{
    uint32_t nlv = tex_mips_wanted(t), bb = dxt_block(t->color);
    if (nlv == 1 && bb && !level0 && !(t->width & 3) && !(t->height & 3) && tex_opt("OOGIE_TEX_FAST", 1)) {
        DXGI_FORMAT f = t->color == 0x0C ? DXGI_FORMAT_BC1_UNORM : t->color == 0x0E ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
        if (!tex_alloc(e, t, 1, f)) return 0;
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)e->tex, 0, NULL,
            (const uint8_t *)xbox_GetMemoryOffset() + t->offset, (t->width / 4) * bb, 0);
        s_txs.fast_bc++;
        return 1;
    }
    if (!level0) {
        if (!tex_decode_fast(t, s_decode)) return 0;
        level0 = s_decode;
    }
    if (!tex_alloc(e, t, nlv, DXGI_FORMAT_B8G8R8A8_UNORM) && (nlv == 1 || !tex_alloc(e, t, nlv = 1, DXGI_FORMAT_B8G8R8A8_UNORM)))
        return 0;
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)e->tex, 0, NULL, level0, t->width * 4, 0);
    if (nlv > 1) {
        ID3D11DeviceContext_GenerateMips(s_ctx, e->srv);
        s_txs.mipped++;
    }
    return 1;
}

/* tex_upload_, timed for the [TEX] line. */
static int tex_upload(TexEntry *e, const Nv2aTexture *t, const uint32_t *level0)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER a, b;
    int r;
    double ms;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    r = tex_upload_(e, t, level0);
    QueryPerformanceCounter(&b);
    ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    s_txs.ms += ms;
    if (ms > s_txs.worst) s_txs.worst = ms;
    return r;
}

static void tex_stats_tick(void)
{
    static DWORD last;
    static int on = -1;
    DWORD now = GetTickCount();
    char b[512];
    int i, k = 0;
    if (on < 0) on = tex_opt("OOGIE_TEX_VERIFY", 0) || tex_opt("OOGIE_TEX_STATS", 0);
    if (!on || now - last < 10000) return;
    last = now;
    fprintf(stderr, "[TEX] uploads: %u straight to the GPU (BC), %u fast decode, %u reference decoder; %u with mipmaps made; verified %u, %u differ; %.0f ms in all, worst %.1f ms\n",
            s_txs.fast_bc, s_txs.fast, s_txs.slow, s_txs.mipped, s_txs.verified, s_txs.mismatched, s_txs.ms, s_txs.worst);
    for (i = 0; i < 64; i++)
        if (s_txs.slow_fmt[i]) k += snprintf(b + k, sizeof b - k, " 0x%02X:%u", i, s_txs.slow_fmt[i]);
    b[k] = 0;
    fprintf(stderr, "[TEX] reference decoder by format:%s\n", k ? b : " none");
    k = 0;
    for (i = 0; i < 16; i++)
        if (s_txs.minf[i]) k += snprintf(b + k, sizeof b - k, " min%d:%u", i, s_txs.minf[i]);
    b[k] = 0;
    fprintf(stderr, "[TEX] texture checks by MIN filter:%s\n", b);
}

static ID3D11ShaderResourceView *get_texture_(const Nv2aTexture *t)
{
    uint32_t key = PHYS(t->offset);
    if (InterlockedExchange(&s_tex_rescan, 0)) {
        repl_index();
        tex_cache_flush();                  /* textures are matched again as they are drawn */
    }
    size_t n;
    uint32_t h;
    int i;
    TexEntry *e = NULL;

    {
        RenderTarget *r = find_rt_by_va(t->offset);
        /* #49: a texture the GPU cannot have rendered is not the render
         * target at its address: compressed (DXT1/3/5) or bigger than the
         * target. The Chapter 2 card (512x512 DXT5 at 0xEA4000, the 128x128
         * glow target of every gameplay frame) was answered with the old
         * surface after any gameplay in the session. OOGIE_RT_ANY_FORMAT=1:
         * the old rule (any texture at a target's address is the target). */
        if (r) {
            static int any = -1;
            if (any < 0) { const char *e = getenv("OOGIE_RT_ANY_FORMAT"); any = e && *e && *e != '0'; }
            if (!any && (t->color == 0x0C || t->color == 0x0E || t->color == 0x0F
                         || t->width > r->w || t->height > r->h))
                r = NULL;
        }
        if (r && !rt_replaced(r)) {
            if (fog_trace_on()) fog_trace_key(s_ft.skey, s_ft.scnt, &s_ft.nsamp, r->key);
            s_last_tex_hash = 0xF0000000u | r->key;
            return r->srv;
        }
    }
    if (!t->width || !t->height || t->width > 4096 || t->height > 4096)
        return s_white_srv;
    for (i = 0; i < s_ntex; i++)
        if (s_tex[i].key == key && s_tex[i].w == t->width && s_tex[i].h == t->height
            && s_tex[i].color == t->color && s_tex[i].pitch == t->pitch) {
            e = &s_tex[i];
            break;
        }
    if (e && e->checked_frame == s_frame) {
        s_last_tex_hash = e->hash;
        return e->srv;
    }
    n = tex_bytes(t);
    h = hash_guest(t->offset, n, is_linear(t->color));
    s_last_tex_hash = h;
    if (e && e->hash == h && (!e->tex || e->lv == tex_mips_wanted(t))) {
        e->checked_frame = s_frame;
        return e->srv;
    }
    if (!e) {
        if (s_ntex < MAX_TEX) e = &s_tex[s_ntex++];
        else { e = &s_tex[s_tex_next]; s_tex_next = (s_tex_next + 1) % MAX_TEX; }
        if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
        if (e->tex) ID3D11Texture2D_Release(e->tex);
        memset(e, 0, sizeof *e);
        e->key = key; e->w = t->width; e->h = t->height; e->color = t->color; e->pitch = t->pitch;
    }
    if ((size_t)t->width * t->height > s_decode_cap) {
        free(s_decode);
        s_decode_cap = (size_t)t->width * t->height;
        s_decode = (uint32_t *)malloc(s_decode_cap * 4);
        if (!s_decode) { s_decode_cap = 0; return s_white_srv; }
    }
    if (!(g_pc.tex_dump || s_nrepl) || is_linear(t->color)) {
        /* No dump or pack to feed: straight to the GPU (tex_upload). */
        if (!e->tex && e->srv) { ID3D11ShaderResourceView_Release(e->srv); e->srv = NULL; }
        if (!tex_upload(e, t, NULL)) {
            e->hash = h; e->checked_frame = s_frame;
            if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
            if (e->tex) ID3D11Texture2D_Release(e->tex);
            e->srv = NULL; e->tex = NULL;
            return s_white_srv;
        }
        s_tex_uploads++;
        if (fog_trace_on()) s_ft.n_upload++;
        e->hash = h;
        e->checked_frame = s_frame;
        return e->srv;
    }
    if (!tex_decode_fast(t, s_decode)) {
        e->hash = h; e->checked_frame = s_frame;
        if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
        if (e->tex) ID3D11Texture2D_Release(e->tex);
        e->srv = NULL; e->tex = NULL;
        return s_white_srv;
    }
    {
        uint64_t ch = tex_content_hash(s_decode, t->width, t->height);
        ID3D11ShaderResourceView *rs;
        if (g_pc.tex_dump) tex_dump(ch, s_decode, t->width, t->height);
        rs = s_nrepl ? repl_find(ch, t->width, t->height) : NULL;
        if (rs) {                           /* a texture pack has this one */
            if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
            if (e->tex) ID3D11Texture2D_Release(e->tex);
            e->tex = NULL;
            ID3D11ShaderResourceView_AddRef(rs);
            e->srv = rs;
            e->hash = h;
            e->checked_frame = s_frame;
            return e->srv;
        }
    }
    if (!e->tex && e->srv) {                /* was a replacement; the game's picture changed */
        ID3D11ShaderResourceView_Release(e->srv);
        e->srv = NULL;
    }
    if (!tex_upload(e, t, s_decode))
        return s_white_srv;
    s_tex_uploads++;
    if (fog_trace_on()) s_ft.n_upload++;
    e->hash = h;
    e->checked_frame = s_frame;
    return e->srv;
}

/* #46 trace: count the stand-in white textures and re-uploads per frame. */
static ID3D11ShaderResourceView *get_texture(const Nv2aTexture *t)
{
    ID3D11ShaderResourceView *v = get_texture_(t);
    if (v == s_white_srv && fog_trace_on()) {
        s_ft.n_white++;
        s_ft.white_key = PHYS(t->offset);
        s_ft.white_fmt = t->color | (t->width << 8) | (t->height << 20);
    }
    return v;
}

/* ── render state objects ──────────────────────────────────────── */

static D3D11_BLEND blend_factor(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_COMPARISON_FUNC cmp_func(uint32_t f)
{
    switch (f) {
    case 0x200: return D3D11_COMPARISON_NEVER;
    case 0x201: return D3D11_COMPARISON_LESS;
    case 0x202: return D3D11_COMPARISON_EQUAL;
    case 0x203: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x204: return D3D11_COMPARISON_GREATER;
    case 0x205: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x206: return D3D11_COMPARISON_GREATER_EQUAL;
    default: return D3D11_COMPARISON_ALWAYS;
    }
}

static D3D11_STENCIL_OP stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
    }
}

static UINT8 write_mask(uint32_t cm)
{
    UINT8 m = 0;
    if (cm & 0x00010000u) m |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (cm & 0x00000100u) m |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (cm & 0x00000001u) m |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (cm & 0x01000000u) m |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    return m;
}

typedef struct { uint32_t k[5]; ID3D11BlendState *s; } BlendCache;
typedef struct { uint32_t k[8]; ID3D11DepthStencilState *s; } DepthCache;
static BlendCache s_bc[128]; static int s_nbc;
static DepthCache s_dc[128]; static int s_ndc;

static ID3D11BlendState *get_blend(uint32_t en, uint32_t src, uint32_t dst, uint32_t eq, uint32_t mask)
{
    uint32_t k[5] = { en, src, dst, eq, mask };
    D3D11_BLEND_DESC bd;
    int i;
    for (i = 0; i < s_nbc; i++)
        if (!memcmp(s_bc[i].k, k, sizeof k))
            return s_bc[i].s;
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = en ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = blend_factor(src, 0);
    bd.RenderTarget[0].DestBlend = blend_factor(dst, 0);
    bd.RenderTarget[0].BlendOp = blend_op(eq);
    bd.RenderTarget[0].SrcBlendAlpha = blend_factor(src, 1);
    bd.RenderTarget[0].DestBlendAlpha = blend_factor(dst, 1);
    bd.RenderTarget[0].BlendOpAlpha = blend_op(eq);
    bd.RenderTarget[0].RenderTargetWriteMask = write_mask(mask);
    if (s_nbc == 128) { ID3D11BlendState_Release(s_bc[0].s); s_nbc = 0; }
    memcpy(s_bc[s_nbc].k, k, sizeof k);
    ID3D11Device_CreateBlendState(s_dev, &bd, &s_bc[s_nbc].s);
    return s_bc[s_nbc++].s;
}

static ID3D11DepthStencilState *get_depth(const Nv2aRenderState *rs, int has_depth)
{
    uint32_t k[8];
    D3D11_DEPTH_STENCIL_DESC dd;
    int i;
    k[0] = has_depth && rs->depth_test_enable;
    k[1] = rs->depth_func;
    k[2] = has_depth && rs->depth_write;
    k[3] = has_depth && rs->stencil_enable;
    k[4] = rs->stencil_func | (rs->stencil_func_mask << 16);
    k[5] = rs->stencil_write_mask;
    k[6] = rs->stencil_fail | (rs->stencil_zfail << 16);
    k[7] = rs->stencil_zpass;
    for (i = 0; i < s_ndc; i++)
        if (!memcmp(s_dc[i].k, k, sizeof k))
            return s_dc[i].s;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = k[0] || k[2] ? TRUE : FALSE;
    dd.DepthFunc = k[0] ? cmp_func(rs->depth_func) : D3D11_COMPARISON_ALWAYS;
    dd.DepthWriteMask = k[2] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.StencilEnable = k[3] ? TRUE : FALSE;
    dd.StencilReadMask = (UINT8)rs->stencil_func_mask;
    dd.StencilWriteMask = (UINT8)rs->stencil_write_mask;
    dd.FrontFace.StencilFunc = cmp_func(rs->stencil_func);
    dd.FrontFace.StencilFailOp = stencil_op(rs->stencil_fail);
    dd.FrontFace.StencilDepthFailOp = stencil_op(rs->stencil_zfail);
    dd.FrontFace.StencilPassOp = stencil_op(rs->stencil_zpass);
    dd.BackFace = dd.FrontFace;
    if (s_ndc == 128) { ID3D11DepthStencilState_Release(s_dc[0].s); s_ndc = 0; }
    memcpy(s_dc[s_ndc].k, k, sizeof k);
    ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s_dc[s_ndc].s);
    return s_dc[s_ndc++].s;
}

/* ── per-frame tracking ────────────────────────────────────────── */

/* s_frame_rt, s_shown_rt, s_frame_chain: declared with the render targets. */

/* OOGIE_PRESENT_TRACE=<flag file>: while it exists, log per frame the
 * full-size surfaces drawn (in order, with draw counts and clears) and the
 * one presented -- for single black or white frames (owner's video). */
static int s_ptrace = -1;
static const char *s_ptrace_flag;
static char s_ptrace_line[1024];
static uint32_t s_ptrace_last_key, s_ptrace_run;

/* The flag file is looked at once per frame (ptrace_frame, from be_flip);
 * per draw it cost the game 50 fps. */
static int s_ptrace_active;

static void ptrace_frame(void)
{
    if (s_ptrace < 0) {
        s_ptrace_flag = getenv("OOGIE_PRESENT_TRACE");
        s_ptrace = s_ptrace_flag ? 1 : 0;
    }
    s_ptrace_active = s_ptrace && GetFileAttributesA(s_ptrace_flag) != INVALID_FILE_ATTRIBUTES;
}

static int ptrace_on(void)
{
    return s_ptrace_active;
}

static void ptrace_note(RenderTarget *r, const char *what)
{
    size_t n = strlen(s_ptrace_line);
    if (r->key == s_ptrace_last_key && *what == 'd') {
        s_ptrace_run++;
        return;
    }
    if (s_ptrace_run && n + 16 < sizeof s_ptrace_line)
        n += snprintf(s_ptrace_line + n, sizeof s_ptrace_line - n, "x%u", s_ptrace_run + 1);
    s_ptrace_run = 0;
    s_ptrace_last_key = *what == 'd' ? r->key : 0;
    if (n + 32 < sizeof s_ptrace_line)
        snprintf(s_ptrace_line + n, sizeof s_ptrace_line - n, " %s%08X", what, r->key);
}

static void note_frame_rt(RenderTarget *r)
{
    if (!r) return;
    r->drawn_frame = s_frame + 1;
    r->baseline = 1;
    if (fog_trace_on()) fog_trace_key(s_ft.dkey, s_ft.dcnt, &s_ft.ndraw, r->key);
    if (s_ptrace_active && r->w >= 640)
        ptrace_note(r, "d");
    if (!s_frame_rt || r->w * r->h >= s_frame_rt->w * s_frame_rt->h)
        s_frame_rt = r;
    s_fl_draws++;
    fl_seq('D', r->key);
    if (is_disp(r->key)) {
        s_frame_disp = r;
        s_fl_disp++;
        if (!s_disp_first) s_disp_first = r;
        else if (r != s_disp_last) s_disp_switches++;
        s_disp_last = r;
    }
    /* A surface presented in the last few flips is one of the game's display
     * buffers (it rotates three: 0x104000, 0x35C000, 0x230000). */
    if (r->shown_frame && s_frame + 1 - r->shown_frame <= 8
            && (!s_frame_chain || r->w * r->h >= s_frame_chain->w * s_frame_chain->h))
        s_frame_chain = r;
}

/* The first three rows; the combiner rows (s_consts.rcs onward) are filled
 * by the caller beforehand. Uploads the whole block. */
static void set_cb(float a0, float a1, float a2, float a3, float b0, float b1, float b2,
                   float b3, float c0, float c1, float c2, float c3)
{
    D3D11_MAPPED_SUBRESOURCE m;
    float *f = s_consts.rt;
    f[0] = a0; f[1] = a1; f[2] = a2; f[3] = a3;
    f = s_consts.tx;
    f[0] = b0; f[1] = b1; f[2] = b2; f[3] = b3;
    f = s_consts.at;
    f[0] = c0; f[1] = c1; f[2] = c2; f[3] = c3;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    memcpy(m.pData, &s_consts, sizeof s_consts);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb, 0);
}

/* Dynamic buffers used as rings: each draw's data goes after the previous
 * draw's (MAP_WRITE_NO_OVERWRITE) and the buffer is discarded only when it
 * wraps. Discarding a 256 KB+ buffer for each of ~2,000 draws a frame made the
 * driver keep a renamed copy of the whole buffer per draw until the GPU caught
 * up: process memory grew by 64 MB driver blocks, ~4 MB/s in a level
 * (KNOWN_ISSUES #44). */
typedef struct {
    ID3D11Buffer *buf;
    ID3D11ShaderResourceView *srv;  /* structured rings only */
    UINT size, pos, bind;
    int structured;
} Ring;
static Ring s_ring_vb = { NULL, NULL, 0, 0, D3D11_BIND_VERTEX_BUFFER, 0 };
static Ring s_ring_vb2 = { NULL, NULL, 0, 0, D3D11_BIND_VERTEX_BUFFER, 0 };
static Ring s_ring_ib = { NULL, NULL, 0, 0, D3D11_BIND_INDEX_BUFFER, 0 };
static Ring s_ring_vin = { NULL, NULL, 0, 0, D3D11_BIND_SHADER_RESOURCE, 1 };
static int s_srv_nooverwrite = -1;      /* NO_OVERWRITE allowed on a buffer SRV */
static uint32_t s_ring_wraps;           /* DISCARD maps of any ring (hitch log) */

static int ring_create(Ring *r, UINT need)
{
    D3D11_BUFFER_DESC bd;
    UINT size = r->structured ? (32u << 20) : (8u << 20);
    while (size < need * 2u) size *= 2u;
    if (r->srv) { ID3D11ShaderResourceView_Release(r->srv); r->srv = NULL; }
    if (r->buf) { ID3D11Buffer_Release(r->buf); r->buf = NULL; }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = size;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = r->bind;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (r->structured) {
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 16;
    }
    r->size = r->pos = 0;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &r->buf)))
        return 0;
    if (r->structured) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        memset(&vd, 0, sizeof vd);
        vd.Format = DXGI_FORMAT_UNKNOWN;
        vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        vd.Buffer.FirstElement = 0;
        vd.Buffer.NumElements = size / 16;
        if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)r->buf, &vd, &r->srv)))
            return 0;
    }
    r->size = size;
    r->pos = size;                      /* first write discards */
    return 1;
}

/* Append bytes (NULL: zeros) aligned to align; *offset gets where they went. */
static int ring_write(Ring *r, const void *data, UINT bytes, UINT align, UINT *offset)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    UINT start;
    if (!bytes) bytes = align;
    if ((!r->buf || bytes > r->size / 2) && !ring_create(r, bytes))
        return 0;
    if (r->structured && s_srv_nooverwrite < 0) {
        D3D11_FEATURE_DATA_D3D11_OPTIONS o;
        memset(&o, 0, sizeof o);
        s_srv_nooverwrite = SUCCEEDED(ID3D11Device_CheckFeatureSupport(s_dev,
            D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof o)) && o.MapNoOverwriteOnDynamicBufferSRV;
        fprintf(stderr, "[D3D11] NO_OVERWRITE on buffer SRVs: %s\n", s_srv_nooverwrite ? "yes" : "no");
    }
    start = (r->pos + align - 1) / align * align;
    if (start + bytes > r->size || (r->structured && !s_srv_nooverwrite)) {
        start = 0;
        mode = D3D11_MAP_WRITE_DISCARD;
        s_ring_wraps++;
    }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)r->buf, 0, mode, 0, &m)))
        return 0;
    if (data)
        memcpy((char *)m.pData + start, data, bytes);
    else
        memset((char *)m.pData + start, 0, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)r->buf, 0);
    r->pos = start + bytes;
    *offset = start;
    return 1;
}

/* Slot 0 the vertices, slot 1 their extras (NULL: zeros, for the clear quad). */
static int upload_vertices(const Nv2aVertex *v, const OogieVertexExtra *x, UINT count)
{
    ID3D11Buffer *bufs[2];
    UINT strides[2] = { sizeof(Nv2aVertex), sizeof(OogieVertexExtra) }, offs[2] = { 0, 0 };
    if (!ring_write(&s_ring_vb, v, count * (UINT)sizeof *v, sizeof *v, &offs[0])
        || !ring_write(&s_ring_vb2, x, count * (UINT)sizeof(OogieVertexExtra),
                       sizeof(OogieVertexExtra), &offs[1]))
        return 0;
    bufs[0] = s_ring_vb.buf; bufs[1] = s_ring_vb2.buf;
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 2, bufs, strides, offs);
    return 1;
}

static void bind_target(RenderTarget *r, DepthTarget *d)
{
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *none[4] = { NULL, NULL, NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, none);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &r->rtv, d ? d->dsv : NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)r->w * r->sc; vp.Height = (float)r->h * r->sc;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
}

/* ── the three back-end entry points ───────────────────────────── */

static int s_probe;                      /* texture probe (below) */

/* KNOWN_ISSUES #57: the clear rectangle of the clear being made, set by the
 * executor (patch clear_rect): [0] SET_CLEAR_RECT_HORIZONTAL, [1] _VERTICAL,
 * min in bits 15:0, max in 31:16, inclusive, surface pixels. Every clear here
 * used to wipe the whole surface; the game clears panels (the figure viewer,
 * the inventory, the loading screen's banner area) and the rest of the frame
 * it had drawn was lost: the owner's flickering menus. OOGIE_FULL_CLEARS=1
 * restores the old behaviour. */
uint32_t g_oogie_clear_rect[2];
static ID3D11DeviceContext1 *s_ctx1;      /* ClearView (D3D 11.1) */

/* The rectangle in target pixels; 0 if the clear covers the whole surface. */
static int clear_rect_px(const RenderTarget *r, D3D11_RECT *rc)
{
    static int full = -1;
    uint32_t h = g_oogie_clear_rect[0], v = g_oogie_clear_rect[1];
    uint32_t x0 = h & 0xFFFFu, x1 = h >> 16, y0 = v & 0xFFFFu, y1 = v >> 16;
    if (full < 0) { const char *e = getenv("OOGIE_FULL_CLEARS"); full = e && *e == '1'; }
    {
        static int lg = -1;
        static unsigned n;
        if (lg < 0) { const char *e = getenv("OOGIE_CLEAR_LOG"); lg = e && *e == '1'; }
        if (lg && n++ < 4000)
            fprintf(stderr, "[CLEARRECT] f=%u %08X %ux%u h=%08X v=%08X\n", s_frame, r->key, r->w, r->h, h, v);
    }
    if (full || (!h && !v) || x1 < x0 || y1 < y0)
        return 0;
    if (x0 == 0 && y0 == 0 && x1 + 1 >= (uint32_t)r->w && y1 + 1 >= (uint32_t)r->h)
        return 0;
    if (x0 >= (uint32_t)r->w || y0 >= (uint32_t)r->h)
        return 0;
    if (x1 + 1 > (uint32_t)r->w) x1 = (uint32_t)r->w - 1;
    if (y1 + 1 > (uint32_t)r->h) y1 = (uint32_t)r->h - 1;
    rc->left = (LONG)SCL(x0, r->sc); rc->right = (LONG)SCL(x1 + 1, r->sc);
    rc->top = (LONG)SCL(y0, r->sc);  rc->bottom = (LONG)SCL(y1 + 1, r->sc);
    return 1;
}

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    fl_seq((flags & 0xF0u) == 0xF0u ? 'C' : 'c', PHYS(s->color_va));
    {
        /* #51 signature: a frame buffer cleared after it was drawn in this
         * flip interval. With OOGIE_FRAME_LOG the methods leading here. */
        extern void oogie_pb_ring_dump(const char *why);
        static int dumps;
        RenderTarget *cr = find_rt_by_va(s->color_va);
        if (cr && is_disp(cr->key) && s_disp_last == cr && (flags & 0xF0u) == 0xF0u
                && cr->drawn_frame == s_frame + 1 && !s_snap_valid)
            snap_before_clear(cr);
        if (s_fl_draws >= 50 && cr && is_disp(cr->key) && s_disp_last == cr && (flags & 0xF0u) == 0xF0u
                && dumps < 3 && getenv("OOGIE_FRAME_LOG")) {
            char why[96];
            dumps++;
            snprintf(why, sizeof why, "f=%u clear of %08X after %u draws into it", s_frame, cr->key, s_fl_draws);
            oogie_pb_ring_dump(why);
        }
    }
    if (s_probe)
        fprintf(stderr, "[PROBE] clear tgt %08X %ux%u flags %X argb %08X z %08X zeta %08X\n",
                PHYS(s->color_va), s->width, s->height, flags, argb, zstencil, rs ? rs->zeta_va : 0);
    RenderTarget *r;
    DepthTarget *d = NULL;

    if (!init_device() || !s->color_va)
        return;
    r = get_rt(s);
    if (!r) return;
    if (flags & 0xF0u) { r->drawn_frame = s_frame + 1; r->baseline = 1; }
    if (flash_rec_on()) {                          /* #46: clears in the draw record */
        DrawRec d;
        memset(&d, 0, sizeof d);
        d.tgt = r->key; d.misc = 0xC1EA0000u | flags; d.n = argb; d.fog = zstencil;
        drawrec_add(&d);
    }
    if (s_ptrace_active && r->w >= 640 && (flags & 0xF0u)) {
        char what[24];
        snprintf(what, sizeof what, "c%02X:%08X@", flags & 0xF0u, argb);
        ptrace_note(r, what);
    }
    if (rs->zeta_va)
        d = get_dt(rs->zeta_va, r->w, r->h);
    if (flags & 0xF0u) {
        D3D11_RECT rc;
        int part = clear_rect_px(r, &rc);
        if (part && !s_ctx1) {
            static const GUID iid1 = { 0xbb2c6faa, 0xb5fb, 0x4082, { 0x8e, 0x6b, 0x38, 0x8b, 0x8c, 0xfa, 0x90, 0xe1 } };
            if (FAILED(ID3D11DeviceContext_QueryInterface(s_ctx, &iid1, (void **)&s_ctx1)))
                s_ctx1 = NULL;
        }
        {
            static unsigned n_part;
            if (part && (n_part++ < 10 || (n_part & 4095) == 0))
                fprintf(stderr, "[D3D11] clear of %08X limited to %ld,%ld-%ld,%ld (%ux%u surface; %u so far)\n",
                        r->key, rc.left, rc.top, rc.right, rc.bottom, r->w, r->h, n_part);
        }
        if ((flags & 0xF0u) == 0xF0u) {
            float c[4];
            c[0] = ((argb >> 16) & 0xFF) / 255.0f;
            c[1] = ((argb >> 8) & 0xFF) / 255.0f;
            c[2] = (argb & 0xFF) / 255.0f;
            c[3] = (argb >> 24) / 255.0f;
            if (part && s_ctx1)
                ID3D11DeviceContext1_ClearView(s_ctx1, (ID3D11View *)r->rtv, c, &rc, 1);
            else
                ID3D11DeviceContext_ClearRenderTargetView(s_ctx, r->rtv, c);
        } else {
            /* A partial clear (Oogie clears alpha alone): a full-surface quad
             * with only those channels written. */
            uint32_t cm = 0;
            Nv2aVertex v[6];
            float W = (float)r->w, H = (float)r->h;
            int i;
            if (flags & 0x10u) cm |= 0x00010000u;
            if (flags & 0x20u) cm |= 0x00000100u;
            if (flags & 0x40u) cm |= 0x00000001u;
            if (flags & 0x80u) cm |= 0x01000000u;
            memset(v, 0, sizeof v);
            {
                float X0 = 0, Y0 = 0, X1 = W, Y1 = H;
                if (part) {           /* the clear rectangle, in surface pixels */
                    X0 = (float)rc.left / r->sc; X1 = (float)rc.right / r->sc;
                    Y0 = (float)rc.top / r->sc;  Y1 = (float)rc.bottom / r->sc;
                }
                v[0].x = X0; v[0].y = Y0; v[1].x = X1; v[1].y = Y0; v[2].x = X0; v[2].y = Y1;
                v[3].x = X1; v[3].y = Y0; v[4].x = X1; v[4].y = Y1; v[5].x = X0; v[5].y = Y1;
            }
            for (i = 0; i < 6; i++) { v[i].rhw = 1.0f; v[i].diffuse = argb; }
            if (upload_vertices(v, NULL, 6)) {
                bind_target(r, NULL);
                s_consts.rcs[0] = 0;
                set_cb(1.0f / W, 1.0f / H, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0);
                ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
                ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
                ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
                ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
                ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
                ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
                ID3D11DeviceContext_OMSetBlendState(s_ctx, get_blend(0, 1, 0, 0x8006, cm), NULL, 0xFFFFFFFFu);
                ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, get_depth(rs, 0), 0);
                ID3D11DeviceContext_Draw(s_ctx, 6, 0);
            }
        }
    }
    if (d && (flags & 0x03u)) {
        UINT cf = 0;
        if (flags & 0x01u) cf |= D3D11_CLEAR_DEPTH;
        if (flags & 0x02u) cf |= D3D11_CLEAR_STENCIL;
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, d->dsv, cf,
            (float)(zstencil >> 8) / 16777215.0f, (UINT8)(zstencil & 0xFF));
    }
    r->last_frame = s_frame;
}

/* Sampler for a texture: [linear][clamp]. MIN filter 1 = nearest. */
static int sampler_for(const Nv2aTexture *t)
{
    int i = ((t->addr_u == 3 || t->addr_u == 4) ? 2 : 0) | 1;
    if (((t->filter >> 16) & 0xFF) == 1)
        i &= ~1;
    return i;
}

/* The combiner rows of s_consts for this batch; 0 when the title has not
 * programmed the combiners (or OOGIE_NO_RC=1). */
static int s_no_rc = -1;
static Nv2aCombiner s_cur_rc;
static int fill_combiner(void)
{
    Nv2aCombiner rc;
    float fog[3];
    int i;
    if (s_no_rc < 0)
        s_no_rc = getenv("OOGIE_NO_RC") && *getenv("OOGIE_NO_RC") == '1';
    if (s_no_rc || !oogie_backend_combiner(&rc, fog)) {
        s_consts.rcs[0] = 0;
        return 0;
    }
    s_consts.rcs[0] = 1;
    s_consts.rcs[1] = rc.control & 0xFFu;
    s_consts.rcs[2] = rc.control >> 8;
    s_consts.rcs[3] = rc.stage_program;
    for (i = 0; i < 8; i++) {
        s_consts.rcw[i][0] = rc.color_icw[i];
        s_consts.rcw[i][1] = rc.alpha_icw[i];
        s_consts.rcw[i][2] = rc.color_ocw[i];
        s_consts.rcw[i][3] = rc.alpha_ocw[i];
        nv2a_rc_unpack(rc.factor0[i], s_consts.k0[i]);
        nv2a_rc_unpack(rc.factor1[i], s_consts.k1[i]);
    }
    s_consts.fin[0] = rc.final0;
    s_consts.fin[1] = rc.final1;
    nv2a_rc_unpack(rc.final_c0, s_consts.fk0);
    nv2a_rc_unpack(rc.final_c1, s_consts.fk1);
    s_consts.fogc[0] = fog[0]; s_consts.fogc[1] = fog[1]; s_consts.fogc[2] = fog[2];
    s_consts.fogc[3] = 1.0f;
    s_cur_rc = rc;
    if (fog_trace_on()) {
        uint32_t c = ((uint32_t)(fog[0] * 255.0f) << 16) | ((uint32_t)(fog[1] * 255.0f) << 8)
                   | (uint32_t)(fog[2] * 255.0f);
        fog_trace_key(s_ft.fkey, s_ft.fcnt, &s_ft.nfog, c);
    }
    return 1;
}

/* Everything a draw needs except its vertices and vertex shader: target,
 * depth, textures, combiner pixel shader, constants and output state. Shared
 * by be_draw (CPU-transformed triangles) and be_draw_vsh (vertex programs on
 * the GPU). Returns the render target, or NULL to skip the draw. */
/* KNOWN_ISSUES #51: a draw that samples the surface it draws into. The Xbox
 * GPU allows it (the texture unit reads the old pixels); Direct3D 11 does
 * not: the view is unbound and every sample reads black. A copy of the
 * target taken before the draw stands in for it. */
static ID3D11Texture2D *s_hz_tex;
static ID3D11ShaderResourceView *s_hz_srv;
static UINT s_hz_w, s_hz_h;
static unsigned s_hz_count;

static ID3D11ShaderResourceView *hazard_copy(RenderTarget *r)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(r->tex, &td);
    if (!s_hz_tex || s_hz_w != td.Width || s_hz_h != td.Height) {
        if (s_hz_srv) { ID3D11ShaderResourceView_Release(s_hz_srv); s_hz_srv = NULL; }
        if (s_hz_tex) { ID3D11Texture2D_Release(s_hz_tex); s_hz_tex = NULL; }
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_hz_tex)))
            return NULL;
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_hz_tex, NULL, &s_hz_srv);
        s_hz_w = td.Width; s_hz_h = td.Height;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_hz_tex, (ID3D11Resource *)r->tex);
    if (s_hz_count++ < 10 || (s_hz_count & 4095) == 0)
        fprintf(stderr, "[D3D11] draw samples its own target %08X: using a copy (%u so far)\n",
                r->key, s_hz_count);
    return s_hz_srv;
}

static RenderTarget *prep_draw(const Nv2aSurface *s, const Nv2aRenderState *rs,
                               const Nv2aTexture *tex0)
{
    RenderTarget *r;
    DepthTarget *d = NULL;
    ID3D11ShaderResourceView *srv[4] = { NULL, NULL, NULL, NULL };
    ID3D11SamplerState *smp[4];
    float zscale, tw = 1, th = 1;
    float bf[4];
    int samp = 3, yuv = 0, use_rc, st;
    ID3D11PixelShader *rc_ps = NULL;

    r = get_rt(s);
    if (!r) return NULL;
    if (rs->zeta_va)
        d = get_dt(rs->zeta_va, r->w, r->h);
    if (tex0) {
        srv[0] = get_texture(tex0);
        tw = tex0->width ? 1.0f / (float)tex0->width : 1.0f;
        th = tex0->height ? 1.0f / (float)tex0->height : 1.0f;
        samp = sampler_for(tex0);
    }
    for (st = 0; st < 4; st++)
        smp[st] = s_samp[samp];
    /* Bink's YUV pattern: Y8 planes on stages 0-2 and a combiner of 5+
     * stages doing the conversion (trace mov1: 640x480 Y + two 320x240,
     * 7 stages). OOGIE_YUV_SWAP=1 swaps the chroma planes. */
    {
        Nv2aTexture t1, t2;
        if (tex0 && tex0->color == 0x13 && oogie_backend_combiner_stages() >= 5
            && oogie_backend_stage_texture(1, &t1) && oogie_backend_stage_texture(2, &t2)
            && t1.color == 0x13 && t2.color == 0x13) {
            static int swap = -1;
            if (swap < 0) swap = getenv("OOGIE_YUV_SWAP") && *getenv("OOGIE_YUV_SWAP") == '1';
            srv[1] = get_texture(&t1);
            srv[2] = get_texture(&t2);
            yuv = swap ? 2 : 1;
        }
    }
    use_rc = !yuv && fill_combiner();
    if (use_rc && !(rc_ps = rc_shader(&s_cur_rc)))
        use_rc = 0;
    if (!use_rc && !yuv && fog_trace_on())
        s_ft.n_norc++;
    if (use_rc)
        for (st = 1; st < 4; st++) {
            Nv2aTexture t;
            if (oogie_backend_stage_texture(st, &t)) {
                srv[st] = get_texture(&t);
                smp[st] = s_samp[sampler_for(&t)];
            }
        }
    {
        ID3D11ShaderResourceView *hz = NULL;
        for (st = 0; st < 4; st++)
            if (srv[st] && srv[st] == r->srv) {
                if (!hz) hz = hazard_copy(r);
                srv[st] = hz;
            }
    }
if (flash_rec_on()) {
        DrawRec d;
        uint32_t h = 2166136261u, k;
        const uint32_t *w = (const uint32_t *)&s_cur_rc;
        memset(&d, 0, sizeof d);
        d.tgt = r->key;
        d.tex = tex0 ? PHYS(tex0->offset) : 0;
        d.texfmt = tex0 ? (tex0->color | (tex0->width << 8) | (tex0->height << 20)) : 0;
        d.texhash = tex0 ? s_last_tex_hash : 0;
        if (use_rc) for (k = 0; k < sizeof s_cur_rc / 4; k++) h = (h ^ w[k]) * 16777619u;
        d.rc = use_rc ? h : 0;
        d.fogc = ((uint32_t)(s_consts.fogc[0] * 255) << 16) | ((uint32_t)(s_consts.fogc[1] * 255) << 8)
               | (uint32_t)(s_consts.fogc[2] * 255);
        d.blend = (rs->blend_enable << 31) | ((rs->blend_src & 0x7FFF) << 16) | (rs->blend_dst & 0xFFFF);
        d.vs = s_dr_vs; d.vc = s_dr_vc; d.n = s_dr_n;
        d.misc = (rs->color_mask & 0x01010101u) | ((uint32_t)rs->alpha_test_enable << 1)
               | ((uint32_t)rs->depth_test_enable << 2) | ((uint32_t)(rs->depth_func & 0xF) << 4)
               | ((uint32_t)(rs->depth_write & 1) << 3);
        d.fog = s_dr_fog;
        drawrec_add(&d);
    }
    if (fog_trace_on() && s_ft.npost < 24) {
        /* post-processing steps: draws that read a surface, or draw into a
         * surface that is not a display buffer */
        RenderTarget *src = tex0 ? find_rt_by_va(tex0->offset) : NULL;
        if (r->shown_frame && rs->zeta_va) s_ft.zeta = PHYS(rs->zeta_va);
        /* the scene before post-processing: the display buffer drawn this
         * frame, the first time a post step reads it */
        if (src && src->shown_frame && !r->shown_frame && !s_ft.pre_done
                && src->drawn_frame == s_frame + 1 && s_ft.measure > 1) {
            s_ft.pre_mean = rt_mean(src);
            s_ft.pre_done = 1;
        }
        if (src || !r->shown_frame) {
            int n = (int)strlen(s_ft.post);
            char tl[24];
            if (src) snprintf(tl, sizeof tl, "%X", src->key & 0xFFFFF0);
            else if (tex0) snprintf(tl, sizeof tl, "T%X/%X/%ux%u", PHYS(tex0->offset), tex0->color,
                                    tex0->width, tex0->height);
            else snprintf(tl, sizeof tl, "-");
            snprintf(s_ft.post + n, sizeof s_ft.post - n, " %X<%s%s b%X/%X/%u c%08X",
                     r->key & 0xFFFFF0, tl,
                     srv[0] == s_white_srv ? "w" : (src && srv[0] != src->srv ? "t" : ""),
                     rs->blend_src & 0xFFF, rs->blend_dst & 0xFFF, rs->blend_enable,
                     use_rc ? s_cur_rc.factor0[0] : 0u);
            s_ft.npost++;
        }
    }
    for (st = 0; st < 4; st++)
        if (!srv[st]) srv[st] = s_white_srv;
    zscale = rs->depth_max > 1.0f ? 1.0f / rs->depth_max : 1.0f / 16777215.0f;
    bind_target(r, d);
    set_cb(1.0f / (float)r->w, 1.0f / (float)r->h, zscale, s_vin_base,
           tw, th, tex0 ? 1.0f : 0.0f, (float)yuv,
           rs->alpha_test_enable ? 1.0f : 0.0f, (float)(rs->alpha_func - 0x200),
           (float)rs->alpha_ref, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_PSSetShader(s_ctx, rc_ps ? rc_ps : s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smp);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
    bf[0] = ((rs->blend_color >> 16) & 0xFF) / 255.0f;
    bf[1] = ((rs->blend_color >> 8) & 0xFF) / 255.0f;
    bf[2] = (rs->blend_color & 0xFF) / 255.0f;
    bf[3] = (rs->blend_color >> 24) / 255.0f;
    if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004
        || rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004)
        bf[0] = bf[1] = bf[2] = bf[3];
    ID3D11DeviceContext_OMSetBlendState(s_ctx,
        get_blend(rs->blend_enable && !(rs->blend_src == 1 && rs->blend_dst == 0),
                  rs->blend_src, rs->blend_dst, rs->blend_eq, rs->color_mask), bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, get_depth(rs, d != NULL), rs->stencil_ref & 0xFF);
    return r;
}

/* Texture probe (diagnostic, 2026-10-18): while logs\texprobe.flag exists,
 * one frame of textured pre-transformed (2D) draws is logged -- texture,
 * and per quad the screen and texel rectangles -- and every texture used is
 * saved as a 32-bit BMP in logs\texprobe\. Used to find the game's font
 * sheets for the themed settings menu. */
static int s_probe;                      /* 1: this frame is being probed */
static unsigned s_probe_n;

static void probe_save_tex(const Nv2aTexture *t)
{
    static uint32_t saved[256]; static int nsaved;
    char path[260];
    uint32_t *px, i, rowb;
    FILE *f;
    for (i = 0; i < (uint32_t)nsaved; i++) if (saved[i] == t->offset) return;
    if (nsaved < 256) saved[nsaved++] = t->offset;
    if (t->width == 0 || t->height == 0 || t->width > 4096 || t->height > 4096) return;
    px = (uint32_t *)malloc((size_t)t->width * t->height * 4);
    if (!px) return;
    if (!nv2a_backend_decode_texture(t, px)) { free(px); return; }
    CreateDirectoryA("logs\\texprobe", NULL);
    snprintf(path, sizeof path, "logs\\texprobe\\%08X_%ux%u_%02X.bmp", t->offset, t->width, t->height, t->color);
    f = fopen(path, "wb");
    if (f) {
        uint8_t h[54] = { 'B', 'M' };
        uint32_t fsz = 54 + t->width * t->height * 4;
        int32_t hh = -(int32_t)t->height;
        rowb = t->width * 4;
        memcpy(h + 2, &fsz, 4); h[10] = 54; h[14] = 40;
        memcpy(h + 18, &t->width, 4); memcpy(h + 22, &hh, 4);
        h[26] = 1; h[28] = 32;
        fwrite(h, 1, 54, f);
        for (i = 0; i < t->height; i++) fwrite((uint8_t *)px + (size_t)i * rowb, 1, rowb, f);
        fclose(f);
    }
    free(px);
}

static void probe_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    uint32_t i;
    const Nv2aTexture *t = b->texture;
    {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        uint32_t k;
        for (k = 0; k < b->count; k++) {
            const Nv2aVertex *q = b->vertices + k;
            if (q->x < x0) x0 = q->x; if (q->x > x1) x1 = q->x;
            if (q->y < y0) y0 = q->y; if (q->y > y1) y1 = q->y;
        }
        fprintf(stderr, "[PROBE] cpu %u tgt %08X tex %08X n %u depth %u bbox %.0f,%.0f-%.0f,%.0f\n", s_probe_n,
                PHYS(s->color_va), t ? t->offset : 0, b->count, b->state ? b->state->depth_test_enable : 9, x0, y0, x1, y1);
    }
    if (!t || b->count > 3000) return;
    probe_save_tex(t);
    fprintf(stderr, "[PROBE] draw %u tgt %08X tex %08X %ux%u fmt %02X rhw %g z %g n %u\n", s_probe_n++, PHYS(s->color_va),
            t->offset, t->width, t->height, t->color, b->vertices[0].rhw, b->vertices[0].z, b->count);
    for (i = 0; i + 5 < b->count; i += 6) {
        const Nv2aVertex *v = b->vertices + i;
        float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y, u0 = v[0].u, u1 = v[0].u, w0 = v[0].v, w1 = v[0].v;
        int k;
        for (k = 1; k < 6; k++) {
            if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x;
            if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
            if (v[k].u < u0) u0 = v[k].u; if (v[k].u > u1) u1 = v[k].u;
            if (v[k].v < w0) w0 = v[k].v; if (v[k].v > w1) w1 = v[k].v;
        }
        fprintf(stderr, "[PROBE]   q xy %.1f,%.1f-%.1f,%.1f uv %.1f,%.1f-%.1f,%.1f c %08X\n",
                x0, y0, x1, y1, u0, w0, u1, w1, v[0].diffuse);
    }
}

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    RenderTarget *r;

    if (!init_device() || !s->color_va || b->count < 3 || !b->vertices)
        return;
    if (s_probe)
        probe_draw(s, b);
    s_dr_vs = 0; s_dr_vc = 0; s_dr_n = b->count; s_dr_fog = 0;
    if (fog_trace_on()) {
        const OogieVertexExtra *x = oogie_backend_vertex_extra();
        uint32_t i;
        s_ft.n_cpu++; s_ft.v_cpu += b->count;
        if (x) for (i = 0; i < b->count; i++) s_ft.fog_sum += x[i].fog;
        else s_ft.fog_sum += b->count;
    }
    if (!upload_vertices(b->vertices, oogie_backend_vertex_extra(), b->count))
        return;
    r = prep_draw(s, b->state, b->texture);
    if (!r) return;
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_Draw(s_ctx, b->count, 0);
    r->last_frame = s_frame;
    note_frame_rt(r);
}

/* ── vertex programs on the GPU (port/src/oogie_gpu_vsh.h) ─────────
 *
 * Each NV2A vertex program becomes an HLSL vertex shader, generated with the
 * same rules as the runtime's interpreter (nv2a_vsh_interp.c: field layout,
 * MAC/ILU semantics, R1 for a paired ILU, reads before writes, defaults of
 * the output registers) and cached by its words. The shader ends the way the
 * executor + be_draw would: w == +0 taken as 1, anti-aliasing scale, surface
 * pixels to clip space (homogeneous, so the GPU clips at w = 0 instead of the
 * executor dropping or splitting triangles), stage coordinates normalised,
 * fog factor as fog_factor(). Inputs come as a structured buffer of the
 * float4s the program reads (VS t8), constants in cbuffer b1. */
typedef struct {
    float c[192][4];
    float aa[4];                /* aa_sx, aa_sy */
    uint32_t fm[4];             /* fog_enable, fog_mode */
    float fp[4];                /* fog_param 0, 1 */
    float ts[4][4];             /* per stage coordinate scale (x, y) */
} VshConsts;
static VshConsts s_vconsts, s_vconsts_last;
static ID3D11Buffer *s_vcb;
static int s_vcb_valid;

static uint32_t vf(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

/* A source operand (0 A, 1 B, 2 C) as HLSL. inmap[i]: index into the input
 * buffer of input register i. */
static void vsh_src(char *out, size_t cap, const uint32_t *ins, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;
    char base[48];
    if (which == 0) {
        neg = vf(ins, 1, 8, 1); swz = vf(ins, 1, 0, 8);
        mux = vf(ins, 2, 26, 2); reg = vf(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = vf(ins, 2, 25, 1); swz = vf(ins, 2, 17, 8);
        mux = vf(ins, 2, 11, 2); reg = vf(ins, 2, 13, 4);
    } else {
        neg = vf(ins, 2, 10, 1); swz = vf(ins, 2, 2, 8);
        mux = vf(ins, 3, 28, 2);
        reg = (vf(ins, 2, 0, 2) << 2) | vf(ins, 3, 30, 2);
    }
    if (mux == 1)
        snprintf(base, sizeof base, "R%u", reg == 12 ? 12u : (reg < 12 ? reg : 0u));
    else if (mux == 2)
        snprintf(base, sizeof base, "v%u", vf(ins, 1, 9, 4));
    else if (vf(ins, 3, 1, 1))
        snprintf(base, sizeof base, "cr(a0 + %u)", vf(ins, 1, 13, 8));
    else
        snprintf(base, sizeof base, "vc[%u]", vf(ins, 1, 13, 8));
    snprintf(out, cap, "%s%s.%c%c%c%c", neg ? "-" : "", base, sw[(swz >> 6) & 3],
             sw[(swz >> 4) & 3], sw[(swz >> 2) & 3], sw[swz & 3]);
}

static void vsh_write(Str *s, const char *dst, uint32_t mask, const char *src)
{
    char m[5];
    int k = 0;
    if (!mask) return;
    if (mask & 8) m[k++] = 'x';
    if (mask & 4) m[k++] = 'y';
    if (mask & 2) m[k++] = 'z';
    if (mask & 1) m[k++] = 'w';
    m[k] = 0;
    sadd(s, "    %s.%s = %s.%s;\n", dst, m, src, m);
}

/* Temp register as a write target; NULL for 13-15 (no write). */
static const char *vsh_temp(uint32_t t)
{
    static const char *const n[13] = { "R0", "R1", "R2", "R3", "R4", "R5", "R6",
                                       "R7", "R8", "R9", "R10", "R11", "R12" };
    return t <= 12 ? n[t] : NULL;
}

static char *vsh_generate(const OogieVshBatch *b)
{
    Str s = { 0 };
    uint32_t i, k = 0, ins_i;
    sadd(&s, "%s", HLSL_COMMON);
    sadd(&s, "cbuffer V : register(b1) { float4 vc[192]; float4 vaa; uint4 vfm; float4 vfp; float4 vts[4]; };\n"
             "StructuredBuffer<float4> VIN : register(t8);\n"
             "float4 cr(int i) { return (i >= 0 && i < 192) ? vc[i] : vc[0]; }\n"
             "float fogf(float d) { if (vfm.x == 0) return 1.0; float px = vfp.x, py = vfp.y, f; uint m = vfm.y;\n"
             "  if (m == 0x800 || m == 0x802) f = px + exp2(d * py * 16.0) - 1.5;\n"
             "  else if (m == 0x801 || m == 0x803) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
             "  else f = px + d * py - 1.0;\n"
             "  if (m == 0x802 || m == 0x803 || m == 0x804) f = abs(f);\n"
             "  if (isnan(f)) f = 1.0; return saturate(f); }\n"
             "VO vsp(uint vid : SV_VertexID) {\n");
    for (i = 0; i < 16; i++) {
        if (b->input_mask & (1u << i))
            sadd(&s, "  float4 v%u = VIN[(uint)rt.w + vid * %u + %u];\n", i, b->ninputs, k++);
        else
            sadd(&s, "  float4 v%u = 0;\n", i);
    }
    sadd(&s, "  float4 R0 = 0, R1 = 0, R2 = 0, R3 = 0, R4 = 0, R5 = 0, R6 = 0, R7 = 0;\n"
             "  float4 R8 = 0, R9 = 0, R10 = 0, R11 = 0, R12 = 0;\n"
             "  float4 o1 = 0, o2 = 0, o3 = float4(0, 0, 0, 1), o4 = float4(0, 0, 0, 1), o5 = 0;\n"
             "  float4 o6 = 0, o7 = 0, o8 = 0, o9 = float4(0, 0, 0, 1), o10 = float4(0, 0, 0, 1);\n"
             "  float4 o11 = float4(0, 0, 0, 1), o12 = float4(0, 0, 0, 1);\n"
             "  int a0 = 0;\n");
    for (ins_i = 0; ins_i < b->ninsn; ins_i++) {
        const uint32_t *ins = b->program[ins_i];
        uint32_t mac = vf(ins, 1, 21, 4), ilu = vf(ins, 1, 25, 3);
        uint32_t mac_mask = vf(ins, 3, 24, 4), ilu_mask = vf(ins, 3, 16, 4);
        uint32_t tdst = vf(ins, 3, 20, 4);
        uint32_t omask = vf(ins, 3, 12, 4), oaddr = vf(ins, 3, 3, 8);
        char A[64], B[64], C[64];
        const char *d;
        vsh_src(A, sizeof A, ins, 0);
        vsh_src(B, sizeof B, ins, 1);
        vsh_src(C, sizeof C, ins, 2);
        sadd(&s, "  { float4 A = %s, B = %s, C = %s, M = A, L = C;\n", A, B, C);
        switch (mac) {
        case 2: sadd(&s, "    M = A * B;\n"); break;
        case 3: sadd(&s, "    M = A + C;\n"); break;
        case 4: sadd(&s, "    M = A * B + C;\n"); break;
        case 5: sadd(&s, "    M = dot(A.xyz, B.xyz).xxxx;\n"); break;
        case 6: sadd(&s, "    M = (dot(A.xyz, B.xyz) + B.w).xxxx;\n"); break;
        case 7: sadd(&s, "    M = dot(A, B).xxxx;\n"); break;
        case 8: sadd(&s, "    M = float4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case 9: sadd(&s, "    M = (A < B) ? A : B;\n"); break;
        case 10: sadd(&s, "    M = (A >= B) ? A : B;\n"); break;
        case 11: sadd(&s, "    M = (A < B) ? 1.0 : 0.0;\n"); break;
        case 12: sadd(&s, "    M = (A >= B) ? 1.0 : 0.0;\n"); break;
        default: break;                                 /* mov, nop, arl: A */
        }
        switch (ilu) {
        case 2: sadd(&s, "    L = (C.x == 0.0 ? asfloat(0x7F800000) : 1.0 / C.x).xxxx;\n"); break;
        case 3: sadd(&s, "    { float f = 1.0 / C.x; float a = abs(f);\n"
                         "      if (a < 5.42101e-20) f = f < 0 ? -5.42101e-20 : 5.42101e-20;\n"
                         "      if (a > 1.884467e19) f = f < 0 ? -1.884467e19 : 1.884467e19;\n"
                         "      L = f.xxxx; }\n"); break;
        case 4: sadd(&s, "    L = rsqrt(abs(C.x)).xxxx;\n"); break;
        case 5: sadd(&s, "    { float fl = floor(C.x); L = float4(exp2(fl), C.x - fl, exp2(C.x), 1.0); }\n"); break;
        case 6: sadd(&s, "    { float ax = abs(C.x);\n"
                         "      if (ax == 0.0) L = float4(-asfloat(0x7F800000), 1.0, -asfloat(0x7F800000), 1.0);\n"
                         "      else { float e = floor(log2(ax)); L = float4(e, ax / exp2(e), log2(ax), 1.0); } }\n"); break;
        case 7: sadd(&s, "    { float nl = max(C.x, 0.0), nh = max(C.y, 0.0), p = clamp(C.w, -127.9961, 127.9961);\n"
                         "      float pw = nh == 0.0 ? (p == 0.0 ? 1.0 : (p > 0.0 ? 0.0 : asfloat(0x7F800000))) : pow(nh, p);\n"
                         "      L = float4(1.0, nl, C.x > 0.0 ? pw : 0.0, 1.0); }\n"); break;
        default: break;                                 /* mov, nop: C */
        }
        if (mac == 13)
            sadd(&s, "    a0 = (int)floor(A.x + 0.001);\n");
        if (mac && mac != 13 && mac_mask && (d = vsh_temp(tdst)) != NULL)
            vsh_write(&s, d, mac_mask, "M");
        if (ilu && ilu_mask && (d = vsh_temp(mac ? 1u : tdst)) != NULL)
            vsh_write(&s, d, ilu_mask, "L");
        if (omask && vf(ins, 3, 11, 1)) {
            const char *src = vf(ins, 3, 2, 1) ? "L" : "M";
            char on[8];
            if (oaddr == 0)
                vsh_write(&s, "R12", omask, src);
            else if (oaddr < 13) {
                snprintf(on, sizeof on, "o%u", oaddr);
                vsh_write(&s, on, omask, src);
            }
        }
        sadd(&s, "  }\n");
    }
    sadd(&s, "  float4 P = R12;\n"
             "  if (asuint(P.w) == 0) P.w = 1.0;\n"
             /* the CPU path's be_draw: 1/w <= 1e-30 (w = +inf from a program
              * that took rcp of 0, as Bink's quad does) draws at w = 1 */
             "  if (P.w >= 1e30) P.w = 1.0;\n"
             "  P.x *= vaa.x; P.y *= vaa.y;\n"
             /* 16:9: the 2D overlay keeps its shape (vaa.z, be_draw_vsh) */
             /* #60: with vaa.w, a vertex on the left or right edge of the 4:3
              * screen stays on the edge of the wide one, so pieces anchored
              * to the edges (the load transition's sweeping shapes, edge
              * frames) reach the sides; everything else is squeezed */
             /* HUD at Screen Edges: a corner piece is squeezed about the
              * side of the screen instead of its centre (vfp.z: -0.5 left,
              * +0.5 right, 0 centre), so it keeps its place in that corner */
             "  if (vaa.z > 0) { float px = P.x / P.w, pv = (0.5 + vfp.z) / rt.x;\n"
             "    if (vaa.w <= 0 || (px > 0.5 && px < 1.0 / rt.x - 0.5))\n"
             "      P.x = pv + (P.x - pv * P.w) * vaa.z; }\n"
             "  VO o;\n"
             "  o.p = float4((P.x * rt.x * 2.0 - 1.0) * P.w, (1.0 - P.y * rt.y * 2.0) * P.w,\n"
             "               saturate(P.z * rt.z) * P.w, P.w);\n"
             "  o.c = saturate(o3); o.s = o4;\n"
             "  o.t = o9.xy * vts[0].xy;\n"
             "  o.fq = float2(fogf(o5.x), o9.w);\n"
             "  o.t1 = float4(o10.xy * vts[1].xy, o10.zw);\n"
             "  o.t2 = float4(o11.xy * vts[2].xy, o11.zw);\n"
             "  o.t3 = float4(o12.xy * vts[3].xy, o12.zw);\n"
             "  return o; }\n");
    return s.p;
}

typedef struct {
    uint32_t hash, ninsn, input_mask, ninputs;
    uint32_t *words;
    ID3D11VertexShader *vs;
} VshShader;
#define MAX_VSH_SHADERS 512
static VshShader s_vsh[MAX_VSH_SHADERS];
static int s_nvsh, s_vsh_failed;

static ID3D11VertexShader *vsh_shader(const OogieVshBatch *b)
{
    static int last = -1;
    uint32_t h = 2166136261u, i, nw = b->ninsn * 4;
    const uint32_t *w = (const uint32_t *)b->program;
    ID3DBlob *code = NULL, *err = NULL;
    VshShader *e;
    char *src;
    int k;
    for (i = 0; i < nw; i++)
        h = (h ^ w[i]) * 16777619u;
    h ^= b->input_mask * 2654435761u;
#define VSH_MATCH(e) ((e)->hash == h && (e)->ninsn == b->ninsn \
                      && (e)->input_mask == b->input_mask && (e)->words \
                      && !memcmp((e)->words, w, nw * 4))
    if (last >= 0 && VSH_MATCH(&s_vsh[last]))
        return s_vsh[last].vs;
    for (k = 0; k < s_nvsh; k++)
        if (VSH_MATCH(&s_vsh[k])) {
            last = k;
            return s_vsh[k].vs;
        }
#undef VSH_MATCH
    if (s_nvsh == MAX_VSH_SHADERS)
        return NULL;
    src = vsh_generate(b);
    if (!src)
        return NULL;
    e = &s_vsh[s_nvsh];
    memset(e, 0, sizeof *e);
    e->hash = h; e->ninsn = b->ninsn; e->input_mask = b->input_mask; e->ninputs = b->ninputs;
    e->words = (uint32_t *)malloc(nw * 4);
    if (e->words) memcpy(e->words, w, nw * 4);
    if (FAILED(compile_cached(src, strlen(src), "oogie_vsh", "vsp", "vs_4_0", &code, &err))) {
        if (s_vsh_failed++ < 3)
            fprintf(stderr, "[D3D11] vertex program shader failed: %s\n%s\n",
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?", src);
    } else {
        ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                        ID3D10Blob_GetBufferSize(code), NULL, &e->vs);
    }
    if (code) ID3D10Blob_Release(code);
    if (err) ID3D10Blob_Release(err);
    free(src);
    if (!e->words) { e->vs = NULL; return NULL; }
    last = s_nvsh++;
    if (s_nvsh <= 8 || (s_nvsh & (s_nvsh - 1)) == 0)
        fprintf(stderr, "[D3D11] vertex programs: %d (shaders: %u compiled in %.0f ms, %u from the disk cache)\n",
                s_nvsh, s_sh_compiled, s_sh_compile_ms, s_sh_loaded);
    return e->vs;
}

static int fill_buffer(ID3D11Buffer *buf, const void *data, UINT bytes)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return 0;
    memcpy(m.pData, data, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)buf, 0);
    return 1;
}

static int be_draw_vsh(const Nv2aSurface *s, const Nv2aRenderState *rs,
                       const Nv2aTexture *tex0, const OogieVshBatch *b)
{
    RenderTarget *r;
    ID3D11VertexShader *vs;
    ID3D11Buffer *cbs[2];
    UINT in_bytes = b->nverts * b->ninputs * 16, st, in_off = 0, ib_off = 0;

    if (!init_device())
        return 0;
    if (s_feature_level < D3D_FEATURE_LEVEL_11_0)
        return 0;                                  /* structured buffers: 11.0 only; the CPU path draws it */
    if (!s->color_va || b->nindices < 3)
        return 1;                                  /* nothing to draw: done */
    if (s_probe) {
        /* Texture probe: the texture, and the raw inputs of small batches. */
        uint32_t v, a, ph = 2166136261u;
        for (v = 0; v < b->ninsn * 4; v++) ph = (ph ^ ((const uint32_t *)b->program)[v]) * 16777619u;
        if (tex0) probe_save_tex(tex0);
        fprintf(stderr, "[PROBE] vsh %u tgt %08X tex %08X %ux%u fmt %02X verts %u idx %u mask %X depth %u prog %08X/%u"
                        " zeta %08X zfunc %X zw %u sten %u/%X/%X blend %u %X %X atest %u %X %u cmask %08X\n",
                s_probe_n++, PHYS(s->color_va), tex0 ? tex0->offset : 0, tex0 ? tex0->width : 0,
                tex0 ? tex0->height : 0, tex0 ? tex0->color : 0,
                b->nverts, b->nindices, b->input_mask, rs->depth_test_enable, ph, b->ninsn,
                rs->zeta_va, rs->depth_func, rs->depth_write, rs->stencil_enable, rs->stencil_func,
                rs->stencil_ref, rs->blend_enable, rs->blend_src, rs->blend_dst,
                rs->alpha_test_enable, rs->alpha_func, rs->alpha_ref, rs->color_mask);
        for (v = 0; v < b->nverts && v < 4; v++) {
            char line[512]; size_t n = 0;
            for (a = 0; a < b->ninputs && n + 64 < sizeof line; a++) {
                const float *f = b->inputs + ((size_t)v * b->ninputs + a) * 4;
                n += (size_t)snprintf(line + n, sizeof line - n, " [%g %g %g %g]", f[0], f[1], f[2], f[3]);
            }
            fprintf(stderr, "[PROBE]   v%u%s\n", v, line);
        }
    }
    if (!(vs = vsh_shader(b)))
        return 0;                                  /* the CPU path draws it */
    if (!ring_write(&s_ring_vin, b->ninputs ? b->inputs : NULL, in_bytes ? in_bytes : 16, 16, &in_off)
        || !ring_write(&s_ring_ib, b->indices, b->nindices * 4, 4, &ib_off))
        return 0;
    /* the shader reads VIN[base + vid * ninputs + k]; base rides in rt.w */
    s_vin_base = (float)(in_off / 16);
    if (flash_rec_on()) {
        uint32_t h = 2166136261u, i;
        for (i = 0; i < b->ninsn * 4; i++) h = (h ^ ((const uint32_t *)b->program)[i]) * 16777619u;
        s_dr_vs = h;
        h = 2166136261u;
        if (s_flash_vc)
            for (i = 0; i < 192 * 4; i++) h = (h ^ ((const uint32_t *)b->constants)[i]) * 16777619u;
        else {
            /* without the constants hash: the program's start slot and length */
            uint32_t start = 0;
            const uint32_t (*base)[4] = oogie_vsh_program(&start);
            h = 0x5A000000u | ((uint32_t)(b->program - base) << 8) | (b->ninsn & 0xFF);
        }
        s_dr_vc = h; s_dr_n = b->nindices;
        s_dr_fog = (b->fog_enable << 31) | (b->fog_mode & 0xFFFF);
    }
    if (fog_trace_on()) {
        s_ft.n_vsh++;
        if (b->fog_enable) s_ft.n_vsh_fog++;
        fog_trace_mode(b->fog_enable, b->fog_mode, b->fog_param[0], b->fog_param[1]);
        if (b->nindices > s_ft.big_n) {
            const uint32_t *w = (const uint32_t *)b->constants;
            uint32_t h = 2166136261u, i;
            for (i = 0; i < sizeof s_vconsts.c / 4; i++) h = (h ^ w[i]) * 16777619u;
            s_ft.big_n = b->nindices; s_ft.big_hash = h;
            /* OOGIE_VC_DUMP=<file>: that batch's program hash and constants,
             * one 3,088-byte record per frame (written at the flip). */
            memcpy(s_ft_big_c, b->constants, sizeof s_ft_big_c);
            h = 2166136261u;
            for (i = 0; i < b->ninsn * 4; i++) h = (h ^ ((const uint32_t *)b->program)[i]) * 16777619u;
            s_ft_big_prog = h; s_ft_big_ninsn = b->ninsn;
        }
    }
    memcpy(s_vconsts.c, b->constants, sizeof s_vconsts.c);
    s_vconsts.aa[0] = b->aa_sx; s_vconsts.aa[1] = b->aa_sy;
    s_vconsts.aa[2] = 0;
    s_vconsts.aa[3] = 0;
    s_vconsts.fp[2] = 0;
    if (b->ninsn > 4 && is_disp(PHYS(s->color_va)))
        s_has3d_cur = 1;
    if (g_pc.hide_borders && b->ninsn <= 4 && b->ninputs >= 2 && (b->input_mask & 1) && b->nverts
            && b->nverts <= 8 && is_disp(PHYS(s->color_va))) {
        /* Hide Black Borders (settings menu): the game paints black bands
         * along the screen edges -- 17 px top and bottom in play, growing to
         * film bars in cutscenes -- and off-screen side masks (probe tp3:
         * untextured black quads, the 2D pass-through program). A black
         * band that touches an edge and is not the whole screen is skipped,
         * so the picture reaches the edges. */
        uint32_t v;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, cmax = 0;
        for (v = 0; v < b->nverts; v++) {
            const float *p = b->inputs + (size_t)v * b->ninputs * 4;
            const float *c = p + 4;
            if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0];
            if (p[1] < y0) y0 = p[1]; if (p[1] > y1) y1 = p[1];
            if (c[0] > cmax) cmax = c[0]; if (c[1] > cmax) cmax = c[1]; if (c[2] > cmax) cmax = c[2];
        }
        if (cmax < 0.02f && (!tex0 || (tex0->width <= 2 && tex0->height <= 2))
                && (y0 <= 0.5f || y1 >= 479.5f || x0 <= 0.5f || x1 >= 639.5f)
                && ((y1 - y0) < 200.0f || (x1 - x0) < 200.0f))
            return 1;                                  /* drawn: nothing to show */
    }
    if (wide_2d() && b->ninsn <= 4 && (b->input_mask & 1) && b->nverts
            && is_disp(PHYS(s->color_va))) {
        /* KNOWN_ISSUES #53: in 16:9 the frame is shown a third wider, so the
         * 2D overlay (health, hints, menus over the 3D) would be stretched.
         * The game draws it with a 3-instruction pass-through program in
         * screen pixels (probe tp3: B1654753, pumpkins at x 31-78). Such a
         * batch is squeezed by 3/4 about the centre unless it spans the
         * whole width (fades, film bars, full-screen effects). */
        uint32_t v;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (v = 0; v < b->nverts; v++) {
            float x = b->inputs[(size_t)v * b->ninputs * 4];
            float y = b->inputs[(size_t)v * b->ninputs * 4 + 1];
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
        /* Off-screen edge masks (x -20..0, 640..660) and full-width
         * quads stay where they are. */
        if (x0 >= -0.5f && x1 <= 640.5f && !(x0 <= 2.0f && x1 >= 638.0f)) {
            static int edges = -1;
            if (edges < 0) { const char *e = getenv("OOGIE_WIDE_EDGES"); edges = !(e && *e == '0'); }
            s_vconsts.aa[2] = (4.0f / 3.0f) / s_wide_ar;
            /* #60 (owner: the load transition's shapes stop short of the
             * sides in widescreen): pieces that touch a side of the 4:3
             * screen are stretched to the side of the wide one (shader:
             * vaa.w). OOGIE_WIDE_EDGES=0 squeezes them like the rest. */
            s_vconsts.aa[3] = (edges && (x0 <= 0.5f || x1 >= 639.5f)) ? 1.0f : 0.0f;
            /* HUD at Screen Edges (settings, Display): a piece wholly in a
             * top corner (outer 30% across, top 30% down: the health
             * pumpkins at x 31-78, gauges, counters) stays in that corner of
             * the wide screen. Not the bottom corners: the game draws its
             * prompts in pieces ("View fig" + "ures?", "Yes No"), and the
             * pieces that fell in a bottom corner were pulled apart (owner,
             * 2026-10-23: "subtitles now have gaps"). A piece goes by its
             * centre: the combo counter's word and its digit are separate
             * pieces, and the tall digit reaches below the band, so the word
             * moved and the digit did not (owner 2026-10-24, combo2_41:
             * "COMI5"). Centre in the top 35% and the outer 35% across, at
             * most 240 px wide: both move together. */
            {
                float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
                if (g_pc.hud_edges && cy <= 168.0f && x1 - x0 <= 240.0f) {
                    if (cx <= 224.0f) s_vconsts.fp[2] = -0.5f;
                    else if (cx >= 416.0f) s_vconsts.fp[2] = 0.5f;
                }
            }
        }
    }
    s_vconsts.fm[0] = b->fog_enable; s_vconsts.fm[1] = b->fog_mode;
    s_vconsts.fp[0] = b->fog_param[0]; s_vconsts.fp[1] = b->fog_param[1];
    for (st = 0; st < 4; st++) {
        s_vconsts.ts[st][0] = b->tscale[st][0];
        s_vconsts.ts[st][1] = b->tscale[st][1];
    }
    if (!s_vcb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof(VshConsts);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vcb)))
            return 0;
    }
    /* Most batches of a frame share their constants: upload on change. */
    if (!s_vcb_valid || memcmp(&s_vconsts, &s_vconsts_last, sizeof s_vconsts)) {
        if (!fill_buffer(s_vcb, &s_vconsts, sizeof s_vconsts))
            return 0;
        s_vconsts_last = s_vconsts;
        s_vcb_valid = 1;
    }
    r = prep_draw(s, rs, tex0);
    s_vin_base = 0;
    if (!r)
        return 1;
    cbs[0] = s_cb; cbs[1] = s_vcb;
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ring_ib.buf, DXGI_FORMAT_R32_UINT, ib_off);
    ID3D11DeviceContext_VSSetShader(s_ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 2, cbs);
    ID3D11DeviceContext_VSSetShaderResources(s_ctx, 8, 1, &s_ring_vin.srv);
    ID3D11DeviceContext_DrawIndexed(s_ctx, b->nindices, 0, 0);
    r->last_frame = s_frame;
    note_frame_rt(r);
    return 1;
}

static void write_bmp(ID3D11Texture2D *src, uint32_t w, uint32_t h)
{
    static int seq = -1;
    const char *prefix = getenv("OOGIE_SHOT_PREFIX");
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    char path[512];
    FILE *f;
    uint32_t row = w * 3, pad = (4 - (row & 3)) & 3, fsz = 54 + (row + pad) * h, y, x;
    uint8_t hdr[54];

    if (!prefix) return;
    if (seq < 0) {                     /* continue after files already there */
        seq = 0;
        for (;;) {
            snprintf(path, sizeof path, "%s%05d.bmp", prefix, seq);
            if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) break;
            seq++;
        }
    }
    ID3D11Texture2D_GetDesc(src, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st))) return;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)src);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        if (fog_trace_on())
            fprintf(stderr, "[SHOT] %05d f=%u\n", seq, s_frame);
        snprintf(path, sizeof path, "%s%05d.bmp", prefix, seq++);
        f = fopen(path, "wb");
        if (f) {
            memset(hdr, 0, sizeof hdr);
            hdr[0] = 'B'; hdr[1] = 'M'; memcpy(hdr + 2, &fsz, 4); hdr[10] = 54; hdr[14] = 40;
            memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); hdr[26] = 1; hdr[28] = 24;
            fwrite(hdr, 1, 54, f);
            for (y = 0; y < h; y++) {
                const uint8_t *p = (const uint8_t *)m.pData + (size_t)(h - 1 - y) * m.RowPitch;
                uint8_t zero[3] = { 0, 0, 0 };
                for (x = 0; x < w; x++)
                    fwrite(p + x * 4, 1, 3, f);      /* B G R */
                fwrite(zero, 1, pad, f);
            }
            fclose(f);
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}

/* Frame pacing. On the Xbox a flip (NV097_FLIP_STALL) completes at the next
 * vertical blank, so a game that waits for its flips runs at the TV's rate.
 * The executor completes flips at once (its own note: "a title that paces
 * itself on the flip runs as fast as it can draw"), and Oogie's Revenge did:
 * 200+ fps once the DSP waits were gone. This flip runs on the executor's
 * thread, so holding it until the next 1/60 s boundary paces the GPU, and the
 * game, which waits on the GPU, follows. OOGIE_FPS_CAP=<hz> changes the rate;
 * 0 turns pacing off. */
static int s_vsync_paces;              /* vsync does the pacing this frame */
static UINT s_sync_interval;

static void pace_flip(void)
{
    static int hz = -1;
    static LARGE_INTEGER freq, next;
    LARGE_INTEGER now;

    if (hz < 0) {
        QueryPerformanceFrequency(&freq);
        timeBeginPeriod(1);              /* 1 ms Sleep granularity */
    }
    hz = g_pc.fps_cap < 0 ? 0 : g_pc.fps_cap;   /* settings menu, live */
    if (!hz || s_vsync_paces)
        return;
    QueryPerformanceCounter(&now);
    if (!next.QuadPart || now.QuadPart - next.QuadPart > freq.QuadPart / 4) {
        next.QuadPart = now.QuadPart;   /* first frame, or far behind: resync */
    }
    next.QuadPart += freq.QuadPart / hz;
    for (;;) {
        LONGLONG left;
        QueryPerformanceCounter(&now);
        left = next.QuadPart - now.QuadPart;
        if (left <= 0)
            break;
        if (left > freq.QuadPart / 500)  /* more than 2 ms: sleep, else spin */
            Sleep(1);
    }
}

/* Settings menu changes, applied live (g_pc, pc_settings.c). */
static void apply_samplers(int aniso);
static void apply_config(void)
{
    static long gen = -1;
    static int a_full = 0, a_scale = -1, a_aniso = 1, a_aspect = -1;
    if (gen == g_pc.gen) return;
    gen = g_pc.gen;
    s_keep_aspect = g_pc.aspect != 2;
    if (g_pc.fullscreen != a_full) {
        if (g_pc.fullscreen != s_fullscreen) toggle_fullscreen();
        a_full = g_pc.fullscreen;
    } else if (s_fullscreen && g_pc.fullscreen) {
        /* Monitor changed while fullscreen: leave and re-enter on it. */
        static int a_mon = 0;
        if (a_mon != g_pc.monitor) { toggle_fullscreen(); toggle_fullscreen(); a_mon = g_pc.monitor; }
    }
    if (a_aspect >= 0 && g_pc.aspect != a_aspect && a_scale >= 0)
        a_scale = -2;                      /* the window takes the new shape */
    a_aspect = g_pc.aspect;
    if (g_pc.window_scale != a_scale) {
        if (a_scale != -1 && !s_fullscreen) {
            int k = g_pc.window_scale;
            RECT r, wa;
            if (k <= 0 && SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
                int fw = (wa.right - wa.left) * 9 / 10 / 640, fh = ((wa.bottom - wa.top) * 9 / 10 - 40) / 480;
                k = fw < fh ? fw : fh;
            }
            if (k < 1) k = 1;
            r.left = 0; r.top = 0; r.right = 640 * k; r.bottom = 480 * k;
            if (g_pc.aspect == 1) r.right = (480 * k * 16 + 8) / 9;
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            ShowWindow(s_hwnd, SW_RESTORE);
            SetWindowPos(s_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        }
        a_scale = g_pc.window_scale;
    }
    if (g_pc.aniso != a_aniso) { apply_samplers(g_pc.aniso); a_aniso = g_pc.aniso; }
    {
        static int a_mips = -1;      /* Mipmaps changed: every texture is made again as drawn */
        if (g_pc.mipmaps != a_mips) { if (a_mips >= 0) InterlockedExchange(&s_tex_rescan, 1); a_mips = g_pc.mipmaps; }
    }
}

/* Vsync: Present waits for the monitor. When the refresh rate is a whole
 * multiple of the frame cap (60 Hz, 120 Hz at 60 fps ...) that wait also
 * paces the game and the timer stays out of it; otherwise the timer paces
 * and Present waits one refresh. */
static void choose_sync(void)
{
    static DWORD t;
    static int hz = 60;
    DWORD now = GetTickCount();
    if (now - t > 2000 || !t) {
        MONITORINFOEXA mi; DEVMODEA dm;
        t = now;
        mi.cbSize = sizeof mi;
        memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
        if (GetMonitorInfoA(MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO *)&mi)
                && EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            hz = (int)dm.dmDisplayFrequency;
    }
    s_vsync_paces = 0; s_sync_interval = 0;
    if (!g_pc.vsync) return;
    s_sync_interval = 1;
    if (g_pc.fps_cap > 0) {
        int k = (hz + g_pc.fps_cap / 2) / g_pc.fps_cap;
        if (k >= 1 && k <= 4 && abs(hz - k * g_pc.fps_cap) <= 1) { s_vsync_paces = 1; s_sync_interval = (UINT)k; }
    }
}

static float s_fps_shown;
float g_oogie_worst_ms;          /* slowest frame in the last 2 s, for the counter */
static double s_worst_cur;
static int s_hitch_skip;         /* the flip after the menu closes: not a hitch */

/* Anti-aliasing and sharpening on the finished picture, at render resolution,
 * before it is scaled to the window (so the settings menu and the frame-rate
 * counter, drawn after, stay as they are). Returns the view to present. */
static ID3D11Texture2D *s_post_tex[2];
static ID3D11RenderTargetView *s_post_rtv[2];
static ID3D11ShaderResourceView *s_post_srv[2];
static UINT s_post_w, s_post_h;

/* Colour-Blind Filter: the Daltonize correction (Fidaner, Lin, Ozguven;
 * public algorithm, written here): simulate what a dichromat sees (RGB ->
 * LMS cone space, drop the missing cone, back), take the colour that was
 * lost, and move it into channels they can see. One matrix:
 * M = I + E (I - Sim), Sim = LMS2RGB * D * RGB2LMS. type 1 protanopia,
 * 2 deuteranopia, 3 tritanopia. */
static void cb_matrix(int type, float m[3][3])
{
    static const float rgb2lms[3][3] = { { 17.8824f, 43.5161f, 4.11935f },
        { 3.45565f, 27.1554f, 3.86714f }, { 0.0299566f, 0.184309f, 1.46709f } };
    static const float lms2rgb[3][3] = { { 0.0809444479f, -0.130504409f, 0.116721066f },
        { -0.0102485335f, 0.0540193266f, -0.113614708f }, { -0.000365296938f, -0.00412161469f, 0.693511405f } };
    static const float err[3][3] = { { 0, 0, 0 }, { 0.7f, 1, 0 }, { 0.7f, 0, 1 } };
    float d[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, t[3][3], sim[3][3], lost[3][3];
    int i, j, k;
    if (type == 1) { d[0][0] = 0; d[0][1] = 2.02344f; d[0][2] = -2.52581f; }
    else if (type == 2) { d[1][0] = 0.494207f; d[1][1] = 0; d[1][2] = 1.24827f; }
    else { d[2][0] = -0.395913f; d[2][1] = 0.801109f; d[2][2] = 0; }
    for (i = 0; i < 3; i++) for (j = 0; j < 3; j++)
        for (t[i][j] = 0, k = 0; k < 3; k++) t[i][j] += d[i][k] * rgb2lms[k][j];
    for (i = 0; i < 3; i++) for (j = 0; j < 3; j++)
        for (sim[i][j] = 0, k = 0; k < 3; k++) sim[i][j] += lms2rgb[i][k] * t[k][j];
    for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) lost[i][j] = (i == j ? 1.0f : 0.0f) - sim[i][j];
    for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) {
        float v = 0;
        for (k = 0; k < 3; k++) v += err[i][k] * lost[k][j];
        m[i][j] = (i == j ? 1.0f : 0.0f) + v;
    }
}

static ID3D11ShaderResourceView *post_process(RenderTarget *r)
{
    static const float aa_span[3] = { 0, 4.0f, 8.0f };
    static const float sharp_k[4] = { 0, 0.3f, 0.6f, 1.0f };
    ID3D11ShaderResourceView *src = r->srv, *none = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_VIEWPORT vp;
    int pass, k = 0, aa = g_pc.aa, sh = g_pc.sharpen, cf = g_pc.colour_filter;
    float cm[3][3];
    if (aa < 0 || aa > 2) aa = 0;
    if (sh < 0 || sh > 3) sh = 0;
    if (cf < 0 || cf > 3) cf = 0;
    if ((!aa || !s_aa_ps) && (!sh || !s_sharp_ps) && (!cf || !s_cb_ps)) return src;
    if (cf) cb_matrix(cf, cm);
    ID3D11Texture2D_GetDesc(r->tex, &td);
    if (td.Width != s_post_w || td.Height != s_post_h) {
        int i;
        for (i = 0; i < 2; i++) {
            if (s_post_srv[i]) ID3D11ShaderResourceView_Release(s_post_srv[i]);
            if (s_post_rtv[i]) ID3D11RenderTargetView_Release(s_post_rtv[i]);
            if (s_post_tex[i]) ID3D11Texture2D_Release(s_post_tex[i]);
            s_post_srv[i] = NULL; s_post_rtv[i] = NULL; s_post_tex[i] = NULL;
        }
        s_post_w = s_post_h = 0;
        td.MipLevels = td.ArraySize = 1; td.SampleDesc.Count = 1; td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0; td.MiscFlags = 0;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (i = 0; i < 2; i++) {
            if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_post_tex[i]))) return src;
            ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_post_tex[i], NULL, &s_post_rtv[i]);
            ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_post_tex[i], NULL, &s_post_srv[i]);
        }
        s_post_w = td.Width; s_post_h = td.Height;
    }
    vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)s_post_w; vp.Height = (float)s_post_h;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    for (pass = 0; pass < 3; pass++) {
        ID3D11PixelShader *ps = pass == 0 ? (aa ? s_aa_ps : NULL) : pass == 1 ? (sh ? s_sharp_ps : NULL)
                              : (cf ? s_cb_ps : NULL);
        float k0s[4];
        if (!ps) continue;
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_post_rtv[k], NULL);
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        if (pass == 2) {                /* colour matrix: rows in tx, at, k0[0] */
            memcpy(k0s, s_consts.k0[0], sizeof k0s);
            s_consts.k0[0][0] = cm[2][0]; s_consts.k0[0][1] = cm[2][1]; s_consts.k0[0][2] = cm[2][2];
            set_cb(0, 0, 1, 1, cm[0][0], cm[0][1], cm[0][2], 0, cm[1][0], cm[1][1], cm[1][2], 0);
            memcpy(s_consts.k0[0], k0s, sizeof k0s);
        } else
            set_cb(0, 0, 1, 1, 1.0f / (float)s_post_w, 1.0f / (float)s_post_h,
                   pass == 0 ? aa_span[aa] : sharp_k[sh], 0, 0, 0, 0, 0);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
        ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &src);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_samp_blit);
        ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, get_blend(0, 1, 0, 0x8006, 0x01010101u), NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
        src = s_post_srv[k];
        k ^= 1;
    }
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    {
        /* OOGIE_POST_SPLIT=1 (tests): the left half stays unprocessed, to
         * compare both in one frame. */
        static int split = -1;
        if (split < 0) { const char *e = getenv("OOGIE_POST_SPLIT"); split = e && *e == '1'; }
        if (split && src != r->srv) {
            D3D11_BOX box;
            box.left = 0; box.top = 0; box.front = 0; box.back = 1;
            box.right = s_post_w / 2; box.bottom = s_post_h;
            ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_post_tex[k ^ 1], 0, 0, 0, 0,
                                                      (ID3D11Resource *)r->tex, 0, &box);
        }
    }
    return src;
}

/* Draw a finished game picture to the window, with the settings menu and
 * the frame-rate counter over it, and show it. */
static void present_rt(RenderTarget *r, int shot, int wide)
{
    D3D11_TEXTURE2D_DESC bd;
    ID3D11Texture2D *bb = NULL;
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *none = NULL;
    float vis_w = 640.0f, vis_h = 480.0f;
    static const float black[4] = { 0, 0, 0, 1 };
    ID3D11ShaderResourceView *pic;

    back_resize();
    IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11Texture2D_GetDesc(bb, &bd);
    ID3D11Texture2D_Release(bb);
    if (bd.Height >= 240) s_native_h = bd.Height;   /* Render Resolution "Native" */
    if (r->w > 640) { vis_w = (float)r->w; vis_h = (float)r->w * 0.75f; }
    if (vis_h > (float)r->h) vis_h = (float)r->h;
    pic = post_process(r);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_back_rtv, NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)bd.Width; vp.Height = (float)bd.Height;
    {
        float war = (float)bd.Width / (float)(bd.Height ? bd.Height : 1);
        if (war < 4.0f / 3.0f) war = 4.0f / 3.0f;
        if (war > 3.6f) war = 3.6f;
        s_wide_ar = war;
    }
    if (g_pc.aspect == 1 && wide) {
        /* Widescreen 3D frame: drawn for this window's shape, fills it. */
    } else if (g_pc.aspect != 2) {
        /* 4:3: the sides black, a blurred copy of the picture, or the
         * picture stretched (settings menu, Sides of 4:3 Screens). */
        float ar = 4.0f / 3.0f;
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_back_rtv, black);
        if (vp.Width > vp.Height * ar * 1.01f && g_pc.side_fill == 1 && s_fill_ps) {
            ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
            set_cb(0, 0, vis_w / (float)r->w, vis_h / (float)r->h,
                   (vp.Height * ar) / vp.Width, 0.012f, 0, 0, 0, 0, 0, 0);
            ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
            ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
            ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
            ID3D11DeviceContext_PSSetShader(s_ctx, s_fill_ps, NULL, 0);
            ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
            ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &pic);
            ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_samp_blit);
            ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
            ID3D11DeviceContext_OMSetBlendState(s_ctx, get_blend(0, 1, 0, 0x8006, 0x01010101u), NULL, 0xFFFFFFFFu);
            ID3D11DeviceContext_Draw(s_ctx, 3, 0);
        }
        if (g_pc.side_fill == 2) ar = vp.Width / vp.Height;
        if (vp.Width > vp.Height * ar) {
            vp.Width = vp.Height * ar;
            vp.TopLeftX = ((float)bd.Width - vp.Width) * 0.5f;
        } else {
            vp.Height = vp.Width / ar;
            vp.TopLeftY = ((float)bd.Height - vp.Height) * 0.5f;
        }
    }
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    set_cb(0, 0, vis_w / (float)r->w, vis_h / (float)r->h, 0, 0, 0, 0, 0, 0, 0, 0);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_blit_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &pic);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_samp_blit);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, get_blend(0, 1, 0, 0x8006, 0x01010101u), NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    if (shot) {
        const char *flag = getenv("OOGIE_SHOT_FLAG");
        if (flag && GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES)
            write_bmp(r->tex, SCL(vis_w, r->sc), SCL(vis_h, r->sc));
    }
    if (InterlockedExchange(&s_shot_req, 0)) {
        /* F12: the picture as shown, before the settings menu and the
         * frame-rate counter go over it. */
        ID3D11Texture2D *cur = NULL, *st = NULL;
        D3D11_TEXTURE2D_DESC sd;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&cur))) {
            ID3D11Texture2D_GetDesc(cur, &sd);
            sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &sd, NULL, &st))) {
                ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)cur);
                if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
                    pc_shot_save_bgra(m.pData, sd.Width, sd.Height, m.RowPitch);
                    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
                }
                ID3D11Texture2D_Release(st);
            }
            ID3D11Texture2D_Release(cur);
        }
    }
    pc_menu_render(s_dev, s_ctx, s_back_rtv, bd.Width, bd.Height, s_fps_shown);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    choose_sync();
    {
        HRESULT hr = IDXGISwapChain_Present(s_swap, s_sync_interval, 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            /* The graphics driver stopped (it crashed and Windows reset it,
             * it was updated, or the GPU was switched). Every D3D call fails
             * from here, which left a frozen black window: say so instead. */
            HRESULT why = ID3D11Device_GetDeviceRemovedReason(s_dev);
            extern void pc_log_drain(DWORD max_ms);
            fprintf(stderr, "[D3D11] graphics device lost (Present 0x%08lX, reason 0x%08lX): closing\n",
                    (unsigned long)hr, (unsigned long)why);
            pc_log_drain(2000);
            MessageBoxA(s_hwnd, "The graphics driver stopped responding (it crashed, was updated, or the graphics "
                        "card changed), so Project Oogie has to close.\n\nYour saves are safe (they are also "
                        "backed up in saves_backup). Start the game again to continue.\n\nIf this keeps "
                        "happening, update the graphics driver or lower the Render Resolution (F1 > Graphics).",
                        "Project Oogie", MB_ICONERROR);
            ExitProcess(3);
        }
    }
}

/* The settings menu is open: the game waits here (the GPU executor does not
 * return, so the game's next frame never completes) while the menu runs on
 * top of the last picture. */
void pc_stall_note_flip_thread(void);   /* pc_stall.c */
void pc_stall_start(void);
static void menu_loop(void)
{
    MSG msg;
    fprintf(stderr, "[MENU] open\n");
    keys_release_all();
    while (pc_menu_is_open()) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        pc_menu_update_state();
        apply_config();
        if (s_shown_rt) present_rt(s_shown_rt, 0, s_shown_wide);
        if (!g_pc.vsync || !s_shown_rt) Sleep(15);
    }
    keys_release_all();
    s_hitch_skip = 1;
    fprintf(stderr, "[MENU] closed\n");
}

/* OOGIE_D3D_DEBUG=1: the debug layer's messages, copied to the log at each
 * flip (at most 300 in all, warnings and errors; COMPATIBILITY.md). */
static void debug_layer_drain(void)
{
    static ID3D11InfoQueue *q;
    static int tried;
    static unsigned shown;
    UINT64 n, i;
    if (!s_debug_layer || shown >= 300) return;
    if (!tried) {
        tried = 1;
        if (FAILED(ID3D11Device_QueryInterface(s_dev, &IID_ID3D11InfoQueue, (void **)&q))) q = NULL;
    }
    if (!q) return;
    n = ID3D11InfoQueue_GetNumStoredMessages(q);
    for (i = 0; i < n && shown < 300; i++) {
        SIZE_T len = 0;
        D3D11_MESSAGE *m;
        if (FAILED(ID3D11InfoQueue_GetMessage(q, i, NULL, &len)) || !len) continue;
        m = (D3D11_MESSAGE *)malloc(len);
        if (!m) break;
        if (SUCCEEDED(ID3D11InfoQueue_GetMessage(q, i, m, &len))
                && m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
            fprintf(stderr, "[D3D11 debug] %s #%d: %.*s\n",
                    m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" :
                    m->Severity == D3D11_MESSAGE_SEVERITY_ERROR ? "ERROR" : "WARNING",
                    (int)m->ID, (int)m->DescriptionByteLength, m->pDescription);
            shown++;
        }
        free(m);
    }
    ID3D11InfoQueue_ClearStoredMessages(q);
}

static void be_flip(void)
{
    MSG msg;
    pc_stall_note_flip_thread();
    debug_layer_drain();
    {
        /* OOGIE_RING_AT_FLIP=<frame> (with OOGIE_FRAME_LOG=1): the last
         * methods before three flips from that frame on (#57 loading screen). */
        static long at = -2;
        static int n;
        extern void oogie_pb_ring_dump(const char *why);
        if (at == -2) { const char *e = getenv("OOGIE_RING_AT_FLIP"); at = e ? atol(e) : -1; }
        if (at >= 0 && (long)s_frame >= at && n < 3) {
            char why[64];
            snprintf(why, sizeof why, "flip at f=%u", s_frame);
            oogie_pb_ring_dump(why);
            n++;
        }
    }
    RenderTarget *r;

    if (!init_device())
        return;
    r = s_frame_chain ? s_frame_chain : (s_frame_rt ? s_frame_rt : s_shown_rt);
    if (s_ndisp) {
        /* #51: a frame buffer drawn this frame, else the one on screen. */
        RenderTarget *d = s_frame_disp ? s_frame_disp
                        : (s_shown_rt && is_disp(s_shown_rt->key) ? s_shown_rt : NULL);
        if (s_disp_switches && s_disp_first && s_disp_first != s_disp_last) {
            /* A deferred flip: show the frame that was finished. */
            static unsigned n_def;
            d = s_disp_first;
            if (n_def++ < 10 || (n_def & 1023) == 0)
                fprintf(stderr, "[PRESENT] f=%u deferred flip: showing finished %08X, not %08X (%u so far)\n",
                        s_frame, s_disp_first->key, s_disp_last->key, n_def);
        }
        if (d && d != r) {
            static unsigned n_diff;
            if (n_diff++ < 20 || (n_diff & 1023) == 0)
                fprintf(stderr, "[PRESENT] f=%u frame buffer %08X instead of guess %08X%s (%u so far)\n",
                        s_frame, d->key, r ? r->key : 0, s_frame_disp ? "" : " (repeat)", n_diff);
        }
        if (d) r = d;
    }
    if (s_snap_valid && s_snap.srv)
        r = &s_snap;                      /* #51: the frame finished before the clear */
    {
        /* KNOWN_ISSUES #51/#57 (2026-10-21, owner watching the pause menu
         * flicker; flip dump: about 4 frames in 10 black, in runs of up to
         * 15): the choice above shows the buffer drawn since the last flip,
         * and when an interval holds only the next frame's clear, that is a
         * just-cleared buffer. The console shows what the CRTC scans out:
         * D3D's vblank handler writes the displayed buffer's address to
         * PCRTC_START (sub_001D8928, `mov [regs + 0x600800], eax`, from
         * sub_001DB020 / sub_001DB140), and the game never clears or draws a
         * buffer while it is on screen. So that buffer is shown when it is
         * one of ours and has been drawn. If PCRTC_START stops moving for 30
         * flips (no vblank handler), the old choice is used.
         * OOGIE_PRESENT_CRTC=0 turns it off. */
        extern volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);
        static int on = -1;
        static uint32_t last_a, still;
        static unsigned n_used;
        volatile uint32_t *cr;
        if (on < 0) { const char *e = getenv("OOGIE_PRESENT_CRTC"); on = !(e && *e == '0'); }
        s_crtc_used = 0;
        if (on && (cr = xbox_Nv2aRegPtr(0x600800)) != NULL) {
            /* The vblank path (sub_001D8928) is not taken with immediate
             * swaps (OOGIE_FORCE_IMMEDIATE, the default), so PCRTC_START
             * stays 0; the executor records the buffer each frame finished
             * in at FLIP_INCREMENT_WRITE (patch flip_done), which is that
             * same buffer one step earlier. */
            extern volatile uint32_t g_oogie_crtc_addr;   /* recomp_manual.c: sub_001D8928_enter */
            uint32_t a = PHYS(g_oogie_flip_done ? g_oogie_flip_done
                              : g_oogie_crtc_addr ? g_oogie_crtc_addr : *cr);
            RenderTarget *c = a ? find_rt_by_va(a) : NULL;
            if (a == last_a) { if (still < 1000000u) still++; } else { still = 0; last_a = a; }
            {
                static unsigned n_dbg;
                if (n_dbg++ < 12 || (!c && (n_dbg & 2047) == 0))
                    fprintf(stderr, "[PRESENT] f=%u finished buffer 0x%08X -> %s (choice %08X, displays %u)\n", s_frame, a,
                            c ? (c->drawn_frame ? "our buffer" : "ours, never drawn") : "no buffer of ours", r ? r->key : 0, s_ndisp);
            }
            if (c && c->tex && c->srv && c->drawn_frame && still < 30) {
                if (n_used++ < 5 || (n_used & 4095) == 0)
                    fprintf(stderr, "[PRESENT] f=%u showing the finished buffer %08X, not %08X (%u so far)\n",
                            s_frame, c->key, r ? r->key : 0, n_used);
                r = c;
                s_crtc_used = 1;
            }
        }
    }
    {
        /* KNOWN_ISSUES #57: a near-empty frame between full ones (owner's
         * figure viewer, every ~8 frames: 305 draws, 1, 305) is held back:
         * the frame before it stays on screen for that flip. Only one in a
         * row, so a real change to a sparse screen (a fade) still shows, one
         * frame late. OOGIE_SHOW_SPARSE=1 turns this off. */
        static int off = -1, held;
        static float ema;
        static unsigned n_held;
        if (off < 0) { const char *e = getenv("OOGIE_SHOW_SPARSE"); off = e && *e == '1'; }
        if (!off && !s_crtc_used && r && s_shown_rt && r != s_shown_rt && !held && ema >= 60.0f   /* heavy scenes only: films and simple menus alternate sparse frames */
                && (float)s_fl_disp * 4.0f < ema && is_disp(r->key) && !pc_menu_is_open()) {
            held = 1;
            if (n_held++ < 20 || (n_held & 1023) == 0)
                fprintf(stderr, "[PRESENT] f=%u held back a near-empty frame (%u draws, usual %.0f): previous frame kept (%u so far)\n",
                        s_frame, s_fl_disp, ema, n_held);
            r = s_shown_rt;
        } else {
            held = 0;
        }
        ema += ((float)s_fl_disp - ema) * 0.125f;
    }
    if (s_ptrace_active) {
        if (s_ptrace_run) {
            size_t n = strlen(s_ptrace_line);
            snprintf(s_ptrace_line + n, sizeof s_ptrace_line - n, "x%u", s_ptrace_run + 1);
        }
        fprintf(stderr, "[PRESENT] %u:%s -> %08X%s\n", s_frame, s_ptrace_line,
                r ? r->key : 0, s_frame_rt ? "" : " (nothing drawn: repeat)");
        s_ptrace_line[0] = 0;
        s_ptrace_run = 0;
        s_ptrace_last_key = 0;
    }
    if (s_ptrace)
        ptrace_frame();
    apply_config();
    if (r) {
        s_shown_wide = wide_frame();
        present_rt(r, 1, s_shown_wide);
        InterlockedIncrement(&g_oogie_presents);
        s_shown_rt = r;
        r->shown_frame = s_frame + 1;
        s_ft.shown = r->key;
    }
    pace_flip();
    auto_scale_tick();
    tex_stats_tick();
    power_tick();
    {
        /* logs\d3dtrace.flag present: dump who called D3D in the last three
         * frames, once per 2 s while it exists (menu flicker work, #57). */
        static DWORD last;
        DWORD now = GetTickCount();
        if (now - last > 2000 && GetFileAttributesA("logs\\d3dtrace.flag") != INVALID_FILE_ATTRIBUTES) {
            extern void oogie_d3d_trace_dump(void);
            last = now;
            oogie_d3d_trace_dump();
        }
    }
    {
        /* OOGIE_TEST_SLOW=<ms>: a slow GPU, for tests (#51). */
        static int slow = -1;
        if (slow < 0) { const char *e = getenv("OOGIE_TEST_SLOW"); slow = e ? atoi(e) : 0; }
        if (slow > 0) Sleep((DWORD)slow);
    }
    {
        /* Frame hitches (owner, 2026-10-18: a drop every ~45 s at 60 fps):
         * a frame over 25 ms is logged with what the back end did in it.
         * Always on; one line per hitch. */
        static LARGE_INTEGER freq, last;
        static uint32_t up0, cr0, wr0, sh0, n_hitch;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        if (last.QuadPart && !s_hitch_skip) {
            double ms = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
            if (ms > s_worst_cur && !pc_menu_is_open()) s_worst_cur = ms;
            if (ms > 25.0 && n_hitch++ < 2000)
                fprintf(stderr, "[HITCH] f=%u %.1f ms: tex uploads %u, new textures %u, ring wraps %u,"
                        " shaders compiled %u\n", s_frame, ms, s_tex_uploads - up0,
                        s_tex_created - cr0, s_ring_wraps - wr0, s_sh_compiled - sh0);
        }
        last = now;
        s_hitch_skip = 0;
        up0 = s_tex_uploads; cr0 = s_tex_created; wr0 = s_ring_wraps; sh0 = s_sh_compiled;
    }
    fog_trace_flip();
    if (flash_rec_on() && s_shown_rt)
        drawrec_flip(s_shown_rt->tex, s_shown_rt->w, s_shown_rt->h);
    {
        /* #49: guest memory under each target drawn this frame, the baseline
         * rt_replaced compares against once the game stops drawing it. */
        int i;
        for (i = 0; i < s_nrt; i++)
            if (s_rt[i].baseline == 1 && s_rt[i].key) {
                s_rt[i].guest_hash = rt_guest_hash(&s_rt[i]);
                s_rt[i].baseline = 2;
            }
    }
    s_frame_rt = NULL;
    s_frame_chain = NULL;
    s_frame_disp = NULL;
    s_disp_first = s_disp_last = NULL;
    s_disp_switches = 0;
    s_snap_valid = 0;
    s_has3d_prev = s_has3d_cur;
    s_has3d_cur = 0;
    {
        /* Texture probe: one frame each time the flag file appears. */
        static int armed = 1, on = -1;
        int flag;
        /* A file check every 16 frames can block on a busy disk ([STALL]
         * sk58b), so the probe is opt-in: OOGIE_TEXPROBE=1. */
        if (on < 0) { const char *e = getenv("OOGIE_TEXPROBE"); on = e && *e == '1'; }
        flag = on && (s_frame & 15) == 0 && GetFileAttributesA("logs\\texprobe.flag") != INVALID_FILE_ATTRIBUTES;
        if (s_probe) { s_probe = 0; fprintf(stderr, "[PROBE] end\n"); }
        else if (flag && armed) { s_probe = 1; armed = 0; s_probe_n = 0; fprintf(stderr, "[PROBE] frame %u\n", s_frame + 1); }
        if (on && (s_frame & 15) == 0 && !flag && GetFileAttributesA("logs\\texprobe.flag") == INVALID_FILE_ATTRIBUTES) armed = 1;
    }
    {
        static int logged;
        disp_refresh();
        if (s_ndisp && !logged) {
            logged = 1;
            fprintf(stderr, "[PRESENT] frame buffers from the D3D device: %08X %08X %08X (%d)\n",
                    s_disp[0], s_ndisp > 1 ? s_disp[1] : 0, s_ndisp > 2 ? s_disp[2] : 0, s_ndisp);
        }
    }
    s_frame++;
    {
        static DWORD t0; static uint32_t f0;
        DWORD now = GetTickCount();
        if (!t0) { t0 = now; f0 = s_frame; }
        if (now - t0 >= 2000) {
            char title[160];
            snprintf(title, sizeof title,
                     "The Nightmare Before Christmas: Oogie's Revenge (Project Oogie, D3D11) | %.1f fps",
                     (s_frame - f0) * 1000.0 / (double)(now - t0));
            SetWindowTextA(s_hwnd, title);
            g_oogie_worst_ms = (float)s_worst_cur;
            s_worst_cur = 0;
            {
                /* One line every 2 s with OOGIE_MEM_STATS=1: what the back
                 * end holds, against the process's private memory. */
                static int ms = -1;
                if (ms < 0) ms = getenv("OOGIE_MEM_STATS") != NULL;
                if (ms) {
                    PROCESS_MEMORY_COUNTERS_EX pm;
                    memset(&pm, 0, sizeof pm);
                    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
                    fprintf(stderr, "[MEM] frame %u private %llu MB | textures live %d created %u (%llu MB) uploads %u | rts %d | vsh %d rc %d\n",
                            s_frame, (unsigned long long)(pm.PrivateUsage >> 20), s_ntex, s_tex_created,
                            (unsigned long long)(s_tex_bytes_created >> 20), s_tex_uploads, s_nrt, s_nvsh, s_nrcs);
                }
            }
            t0 = now; f0 = s_frame;
        }
    }
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    keys_sync();
    {
        /* Frame-rate counter: frames over the last half second. */
        static DWORD t0; static uint32_t f0;
        DWORD now = GetTickCount();
        if (!t0) { t0 = now; f0 = s_frame; }
        if (now - t0 >= 500) { s_fps_shown = (float)(s_frame - f0) * 1000.0f / (float)(now - t0); t0 = now; f0 = s_frame; }
    }
    {
        /* OOGIE_FRAME_LOG=1: one line per presented frame -- draws, draws into
         * frame buffers, what was shown -- and the game's Swap calls by path
         * (recomp_manual.c), for the menu flicker (#51). */
        static int fl = -1;
        extern volatile long g_oogie_swap_flip, g_oogie_swap_finish;
        static long pf, pn;
        if (fl < 0) { const char *e = getenv("OOGIE_FRAME_LOG"); fl = e && *e && *e != '0'; }
        if (fl) {
            size_t n = strlen(s_fl_seq);
            if (s_fl_run > 1 && n + 8 < sizeof s_fl_seq) snprintf(s_fl_seq + n, sizeof s_fl_seq - n, "*%u", s_fl_run);
            fprintf(stderr, "[FRAME] f=%u shown %08X draws %u disp %u swaps flip %ld finish %ld |%s\n",
                    s_frame, s_shown_rt ? s_shown_rt->key : 0, s_fl_draws, s_fl_disp,
                    g_oogie_swap_flip - pf, g_oogie_swap_finish - pn, s_fl_seq);
            pf = g_oogie_swap_flip; pn = g_oogie_swap_finish;
        }
        {
            /* Always on (owner, 2026-10-19: menus still flicker for them, not
             * in any scripted run): a frame that drew under half of what the
             * frames either side drew, while those two agree, is logged with
             * all three frames' clear/draw records. At most 200 lines. */
            static char seq[3][sizeof s_fl_seq];
            static unsigned dn[3], n_fl;
            size_t n = strlen(s_fl_seq);
            if (!fl && s_fl_run > 1 && n + 8 < sizeof s_fl_seq) snprintf(s_fl_seq + n, sizeof s_fl_seq - n, "*%u", s_fl_run);
            memmove(seq[0], seq[1], sizeof seq[0] * 2);
            memmove(&dn[0], &dn[1], sizeof dn[0] * 2);
            snprintf(seq[2], sizeof seq[2], "%s", s_fl_seq);
            dn[2] = s_fl_disp;
            if (dn[0] >= 4 && dn[2] >= 4 && dn[0] <= 400 && dn[2] <= 400
                    && dn[1] * 2 < (dn[0] < dn[2] ? dn[0] : dn[2])
                    && (dn[0] > dn[2] ? dn[0] - dn[2] : dn[2] - dn[0]) * 5 <= (dn[0] > dn[2] ? dn[0] : dn[2])
                    && n_fl++ < 200 && !pc_menu_is_open())
                fprintf(stderr, "[FLICKER] f=%u frame draws %u then %u then %u |%s ||%s ||%s\n",
                        s_frame - 1, dn[0], dn[1], dn[2], seq[0], seq[1], seq[2]);
            /* The #51 shape: the buffer shown was cleared after most of this
             * interval's drawing into it ("D104*134 C104 D104"), so it held
             * a clear and a few quads. Shown from the copy taken before the
             * clear, that is handled ("caught"); otherwise it is logged. */
            if (s_shown_rt && !pc_menu_is_open() && strlen(seq[2]) + 16 < sizeof seq[2]) {   /* not a cut-off record */
                static unsigned n_caught, n_missed;
                int caught = s_shown_rt == &s_snap;
                char want[8];
                const char *t = seq[2];
                unsigned head = 0, tail = 0, total = 0;
                int cleared = 0;
                snprintf(want, sizeof want, "%03X", (s_shown_rt->key >> 12) & 0xFFF);
                while ((t = strchr(t, ' ')) != NULL) {
                    char kind = t[1];
                    unsigned cnt = 1;
                    t++;
                    if (strncmp(t + 1, want, 3) != 0) continue;
                    if (t[4] == '*') cnt = (unsigned)atoi(t + 5);
                    if (kind == 'C' && total) { head = total; tail = 0; cleared = 1; }
                    else if (kind == 'D') { total += cnt; tail += cnt; }
                }
                if (cleared && head >= 4 && tail * 2 < head) {
                    if (caught) n_caught++; else n_missed++;
                    if (!caught && n_missed <= 4) {
                        extern void oogie_d3d_trace_dump(void);
                        oogie_d3d_trace_dump();
                    }
                    if ((!caught && n_missed <= 200) || (caught && (n_caught <= 3 || (n_caught & 1023) == 0)))
                        fprintf(stderr, "[FLICKER] f=%u buffer %s cleared after %u of its draws (%u after): %s (caught %u, missed %u) |%s\n",
                                s_frame - 1, want, head, tail, caught ? "showed the copy" : "SHOWN BROKEN",
                                n_caught, n_missed, seq[2]);
                }
            }
        }
        s_fl_draws = 0; s_fl_disp = 0;
        s_fl_seq[0] = 0; s_fl_last_kind = 0; s_fl_last_key = 0; s_fl_run = 0;
    }
    pc_menu_update_state();
    if (pc_menu_is_open())
        menu_loop();
}

static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip };

/* Called from main.c before the game starts. */
void oogie_d3d11_register(void)
{
    const char *e = getenv("OOGIE_GPU_VSH");
    nv2a_backend_register(&s_backend);
    pc_stall_start();
    /* Vertex programs on the GPU unless OOGIE_GPU_VSH=0 (oogie_gpu_vsh.h). */
    if (!(e && *e == '0'))
        oogie_gpu_vsh_register(be_draw_vsh);
    fprintf(stderr, "[D3D11] back end registered (window opens at the first GPU command)%s\n",
            (e && *e == '0') ? "" : ", vertex programs on the GPU");
    fflush(stderr);
}
