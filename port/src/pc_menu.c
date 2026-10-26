/* Project Oogie: the in-game settings menu (see pc_menu.h).
 *
 * Look: the game's own menus (Chapter Select) -- gold-to-orange lettering
 * for the title and the selected row, dull olive for the rest, on a dark
 * cloudy panel with a pale moon. The font is "Mountains of Christmas" (Font
 * Diner, Apache 2.0, assets/fonts/), a hand-lettered face in the same Tim
 * Burton style; if the file is missing, Georgia is used. The game's own menu
 * words are pictures, not a font, so they cannot spell new text.
 *
 * Model: the Burnout 3 port (Takedawn) and the BLACK recomp. Pages with
 * LB/RB (Q/E), rows with up/down, values with left/right, A/Enter to change
 * or rebind, Y/Delete for the default, B/Esc to close. Every change is
 * applied at once (the back end reads g_pc each frame) and saved to
 * settings.ini. */
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pc_menu.h"
#include "pc_settings.h"
#include "pc_input.h"

/* ---- state ---------------------------------------------------------------- */
static volatile LONG s_open;
static volatile LONG s_toggle_req;
static HWND s_hwnd;
static unsigned char s_keys_down[256];

void pc_menu_set_window(HWND h) { s_hwnd = h; }
int pc_menu_is_open(void) { return s_open != 0; }
void pc_menu_request_toggle(void) { InterlockedExchange(&s_toggle_req, 1); }

/* Owner 2026-10-24: "locked out from the controls" in two sessions. A key
 * whose key-up went to another window (Alt+Tab, a notification, the Windows
 * key) stayed in s_keys_down for good, and after the menu next closed the
 * game was held off until "every key is let go", i.e. for ever. A key now
 * counts only while Windows says it is physically down; the record is
 * cleared when the window loses focus (pc_menu_keys_reset). */
int pc_menu_keys_held(void)
{
    int i, held = 0;
    for (i = 1; i < 256; i++)
        if (s_keys_down[i]) {
            if (GetAsyncKeyState(i) & 0x8000) held = 1;
            else s_keys_down[i] = 0;
        }
    return held;
}

void pc_menu_keys_reset(void)
{
    memset(s_keys_down, 0, sizeof s_keys_down);
}

float oogie_audio_gain(void)
{
    float g = (float)g_pc.volume / 100.0f;
    g = g * g;                                   /* even steps to the ear */
    if (s_open)
        return 0.0f;
    if (g_pc.mute_bg && s_hwnd && GetForegroundWindow() != s_hwnd)
        return 0.0f;
    return g;
}

/* ---- the menu's content --------------------------------------------------- */
enum { P_DISPLAY, P_GRAPHICS, P_AUDIO, P_CONTROLS, P_KEYBOARD, P_PAD, P_MODS, P_ABOUT, P_COUNT };
static const char *const s_page_names[P_COUNT] = {
    "Display", "Graphics", "Audio", "Controls", "Keyboard", "Controller", "Mods", "About"
};

/* Mods page (dev cycle 4): one row per folder in mods\, On or Off (a file
 * named "disabled" in the mod's folder turns it off; the file layer,
 * patch mods_folder_a, reads the list again after a change). */
#define MOD_SLOTS 24
static char s_mod_name[MOD_SLOTS][64];
static int s_mod_on[MOD_SLOTS];
static char s_mods_status[96] = "No mods yet";
extern void oogie_mods_rescan(void);
extern void oogie_textures_rescan(void);   /* d3d11_backend.c */

enum { IT_CHOICE, IT_ACTION, IT_KEY, IT_PAD, IT_INFO };

typedef struct { const char *name; int value; } Opt;

typedef struct {
    int page;
    const char *label;
    const char *help;
    int type;
    int *field;              /* IT_CHOICE: the g_pc field */
    const Opt *opts; int nopts;
    int def;
    const char *sec, *key;   /* where it is saved; key also names a binding */
    const char *keydef;      /* IT_KEY / IT_PAD: the default binding text */
    int restart;             /* takes effect at the next start */
    void (*action)(void);
} Item;

static const Opt o_onoff[] = { { "Off", 0 }, { "On", 1 } };
static const Opt o_mode[] = { { "Windowed", 0 }, { "Fullscreen", 1 } };
static const Opt o_aspect[] = { { "4:3 (Original)", 0 }, { "Widescreen (Fill Screen)", 1 }, { "Stretch to Window", 2 } };
static const Opt o_fps[] = { { "30", 30 }, { "60 (Original)", 60 } };
static const Opt o_side[] = { { "Black", 0 }, { "Blurred Picture", 1 }, { "Stretch", 2 } };
/* Render Resolution by the height the game's 480 lines are drawn at (the
 * width follows the shape); 1..4 are the old multiples, kept for old
 * settings files. -1 = the screen's or window's own height. */
static const Opt o_scale[] = { { "Auto (best that holds 60 fps)", 0 }, { "Native (match my screen)", -1 },
    { "480p (original)", 1 }, { "720p", 720 }, { "900p", 900 }, { "960p", 2 }, { "1080p", 1080 },
    { "1200p", 1200 }, { "1440p", 3 }, { "1800p", 1800 }, { "1920p", 4 }, { "2160p (4K)", 2160 } };
static const Opt o_aniso[] = { { "Off", 1 }, { "2x", 2 }, { "4x", 4 }, { "8x", 8 }, { "16x", 16 } };
static const Opt o_aa[] = { { "Off", 0 }, { "Low", 1 }, { "High", 2 } };
static const Opt o_abuf[] = { { "Small (least delay)", 0 }, { "Large (no crackles)", 1 } };
static const Opt o_repeat[] = { { "Off", 0 }, { "A", 1 }, { "B", 2 }, { "X", 3 }, { "Y", 4 } };
static const Opt o_cfilt[] = { { "Off", 0 }, { "Protanopia (red-weak)", 1 }, { "Deuteranopia (green-weak)", 2 },
    { "Tritanopia (blue-weak)", 3 } };
static const Opt o_sharp[] = { { "Off", 0 }, { "Low", 1 }, { "Medium", 2 }, { "High", 3 } };
/* Quality Preset (dev cycle 8, owner: "run this on a potato ... to a 5090"):
 * one row that sets the five picture settings below it. Not saved itself:
 * it shows the preset the five match, or Custom (s_preset, preset_match). */
static const Opt o_preset[] = { { "Custom", 0 }, { "Low (weak PCs, least memory)", 1 }, { "Medium", 2 },
    { "High", 3 }, { "Ultra (strongest PCs)", 4 } };
static int s_preset;
/* render_scale, aa, sharpen, mipmaps, aniso for Low .. Ultra */
static const int s_preset_vals[4][5] = {
    { 1, 0, 0, 0, 1 },          /* Low: 480p, no AA or sharpening, compressed textures */
    { 720, 1, 1, 1, 4 },        /* Medium: 720p */
    { 0, 2, 1, 1, 8 },          /* High: Auto (best that holds 60 fps) */
    { -1, 2, 1, 1, 16 },        /* Ultra: Native, the screen's own resolution */
};
static int *const s_preset_fields[5] = { &g_pc.render_scale, &g_pc.aa, &g_pc.sharpen, &g_pc.mipmaps, &g_pc.aniso };
static int preset_match(void)
{
    int p, k;
    for (p = 0; p < 4; p++) {
        for (k = 0; k < 5 && *s_preset_fields[k] == s_preset_vals[p][k]; k++)
            ;
        if (k == 5) return p + 1;
    }
    return 0;
}
static const Opt o_rumble[] = { { "Off", 0 }, { "Normal", 1 }, { "Strong", 2 } };
static Opt o_volume[21];
static Opt o_fov[15];
static Opt o_monitor[9];  static char s_mon_names[9][64];  static int s_nmon;
static Opt o_winsize[10]; static char s_win_names[10][48]; static int s_nwin;

static void act_reset_keys(void);
static void act_reset_pad(void);
static void act_open_folder(void);
static void act_reset_all(void);
static void act_close(void);
static void act_open_mods(void);
static void mods_scan(void);

static Item s_items[] = {
    { P_DISPLAY, "Display Mode", "Play in a window or in borderless fullscreen (also F11 or Alt+Enter).",
      IT_CHOICE, &g_pc.fullscreen, o_mode, 2, 0, "Display", "fullscreen" },
    { P_DISPLAY, "Monitor", "The monitor fullscreen uses.",
      IT_CHOICE, &g_pc.monitor, o_monitor, 0, 0, "Display", "monitor" },
    { P_DISPLAY, "Window Size", "Size of the game window. Auto fits your screen. The window can also be resized by hand.",
      IT_CHOICE, &g_pc.window_scale, o_winsize, 0, 0, "Display", "window_scale" },
    { P_DISPLAY, "Aspect Ratio", "Widescreen draws the 3D world for your screen's shape: edge to edge, nothing stretched.",
      IT_CHOICE, &g_pc.aspect, o_aspect, 3, 0, "Display", "aspect" },
    { P_DISPLAY, "Hide Black Borders", "Remove the black edges the game paints, and the film bars in cutscenes.",
      IT_CHOICE, &g_pc.hide_borders, o_onoff, 2, 1, "Display", "hide_borders" },
    { P_DISPLAY, "Sides of 4:3 Screens", "What fills the sides when a menu or film is 4:3 in a wide window.",
      IT_CHOICE, &g_pc.side_fill, o_side, 3, 1, "Display", "side_fill" },
    { P_DISPLAY, "Skip Intro Logos", "Skip the company logos film when the game starts.",
      IT_CHOICE, &g_pc.skip_intro, o_onoff, 2, 0, "Display", "skip_intro" },
    { P_DISPLAY, "HUD at Screen Edges", "Widescreen: health and gauges sit in the corners of your screen.",
      IT_CHOICE, &g_pc.hud_edges, o_onoff, 2, 1, "Display", "hud_edges" },
    { P_DISPLAY, "Field of View", "How wide the game camera sees. 100% is the original.",
      IT_CHOICE, &g_pc.fov, o_fov, 15, 100, "Display", "field_of_view" },
    { P_DISPLAY, "Vertical Sync", "Wait for the monitor before showing each frame: no tearing.",
      IT_CHOICE, &g_pc.vsync, o_onoff, 2, 1, "Display", "vsync" },
    { P_DISPLAY, "Frame Rate Cap", "60 is the game's own speed; its logic, films and music run per frame.",
      IT_CHOICE, &g_pc.fps_cap, o_fps, 2, 60, "Display", "fps_cap" },

    { P_GRAPHICS, "Quality Preset", "Sets the five settings below at once: Low for weak laptops, Ultra for the strongest PCs.",
      IT_CHOICE, &s_preset, o_preset, 5, 3, "Graphics", "preset" },
    { P_GRAPHICS, "Render Resolution", "Sharper picture. If the game slows below 60 fps (it then runs slow), pick a lower value.",
      IT_CHOICE, &g_pc.render_scale, o_scale, 12, 0, "Graphics", "render_scale" },
    { P_GRAPHICS, "Anisotropic Filtering", "Sharper textures on surfaces seen at an angle (floors, walls).",
      IT_CHOICE, &g_pc.aniso, o_aniso, 5, 1, "Graphics", "anisotropic" },
    { P_GRAPHICS, "Anti-Aliasing", "Smooths jagged edges on characters and buildings. Almost free.",
      IT_CHOICE, &g_pc.aa, o_aa, 3, 2, "Graphics", "anti_aliasing" },
    { P_GRAPHICS, "Sharpening", "Crisper textures and text, most useful below Native resolution.",
      IT_CHOICE, &g_pc.sharpen, o_sharp, 4, 1, "Graphics", "sharpening" },
    { P_GRAPHICS, "Mipmaps", "Smoother distant ground and walls, no shimmer. Off is the original look and uses the least video memory.",
      IT_CHOICE, &g_pc.mipmaps, o_onoff, 2, 1, "Graphics", "mipmaps" },
    { P_GRAPHICS, "Colour-Blind Filter", "Shifts colours you cannot tell apart into ones you can.",
      IT_CHOICE, &g_pc.colour_filter, o_cfilt, 4, 0, "Graphics", "colour_filter" },
    { P_GRAPHICS, "Show Frame Rate", "Frames per second in the top-left corner.",
      IT_CHOICE, &g_pc.show_fps, o_onoff, 2, 0, "Graphics", "show_fps" },

    { P_AUDIO, "Master Volume", "Volume of everything: music, voices and sound effects.",
      IT_CHOICE, &g_pc.volume, o_volume, 21, 100, "Audio", "volume" },
    { P_AUDIO, "Mute in Background", "Silence the game while another window is in front.",
      IT_CHOICE, &g_pc.mute_bg, o_onoff, 2, 0, "Audio", "mute_in_background" },
    { P_AUDIO, "Pause in Background", "Switching to another window pauses the game (this menu opens).",
      IT_CHOICE, &g_pc.pause_bg, o_onoff, 2, 0, "Audio", "pause_in_background" },
    { P_AUDIO, "Audio Buffer", "Large rides out loading hiccups without crackles; Small has the least delay.",
      IT_CHOICE, &g_pc.audio_buffer, o_abuf, 2, 1, "Audio", "buffer", NULL, 1 },

    { P_CONTROLS, "Rumble", "Controller vibration strength.",
      IT_CHOICE, &g_pc.rumble, o_rumble, 3, 1, "Controller", "rumble" },
    { P_CONTROLS, "Auto-Repeat Held Button", "Hold this button and it presses itself about 10 times a second. For button mashing.",
      IT_CHOICE, &g_pc.auto_repeat, o_repeat, 5, 0, "Controller", "auto_repeat" },
    { P_CONTROLS, "PlayStation and Other Pads", "DualShock 4, DualSense, Switch Pro and others, through SDL2. Xbox pads always work.",
      IT_CHOICE, &g_pc.other_pads, o_onoff, 2, 1, "Controller", "other_pads", NULL, 1 },
    { P_CONTROLS, "Reset Keyboard Keys", "Put every key back to the default layout.",
      IT_ACTION, NULL, NULL, 0, 0, NULL, NULL, NULL, 0, act_reset_keys },
    { P_CONTROLS, "Reset Controller Buttons", "Put every pad button back to the original Xbox layout.",
      IT_ACTION, NULL, NULL, 0, 0, NULL, NULL, NULL, 0, act_reset_pad },
    { P_CONTROLS, "Connected Controllers", NULL, IT_INFO },

#define KB(lbl, key, def) { P_KEYBOARD, lbl, "Press A or Enter, then the new key. Delete clears, Y resets.", IT_KEY, NULL, NULL, 0, 0, "Keyboard", key, def }
    KB("A (jump, confirm)", "A", "Down, Z"),
    KB("B (back)", "B", "Right, X"),
    KB("X (Soul Robber)", "X", "Left, C"),
    KB("Y", "Y", "Up, V"),
    KB("Black", "Black", "E"),
    KB("White", "White", "Q"),
    KB("Left Trigger", "LeftTrigger", "1"),
    KB("Right Trigger", "RightTrigger", "3"),
    KB("Start", "Start", "Enter"),
    KB("Back (map)", "Back", "Backspace"),
    KB("Walk Up", "LeftStickUp", "W, Numpad8"),
    KB("Walk Down", "LeftStickDown", "S, Numpad2"),
    KB("Walk Left", "LeftStickLeft", "A, Numpad4"),
    KB("Walk Right", "LeftStickRight", "D, Numpad6"),
    KB("Camera Up", "RightStickUp", "I"),
    KB("Camera Down", "RightStickDown", "K"),
    KB("Camera Left", "RightStickLeft", "J"),
    KB("Camera Right", "RightStickRight", "L"),
    KB("D-pad Up", "DpadUp", "T"),
    KB("D-pad Down", "DpadDown", "G"),
    KB("D-pad Left", "DpadLeft", "F"),
    KB("D-pad Right", "DpadRight", "H"),
    KB("Left Stick Click", "LeftStickClick", "Shift"),
    KB("Right Stick Click", "RightStickClick", "Ctrl"),
#undef KB
#define PADB(lbl, key, def) { P_PAD, lbl, "Press A or Enter, then the pad button for it. Y resets.", IT_PAD, NULL, NULL, 0, 0, "ControllerButtons", key, def }
    PADB("A (jump, confirm)", "A", "A"),
    PADB("B (back)", "B", "B"),
    PADB("X (Soul Robber)", "X", "X"),
    PADB("Y", "Y", "Y"),
    PADB("Black", "Black", "LB"),
    PADB("White", "White", "RB"),
    PADB("Left Trigger", "LeftTrigger", "LT"),
    PADB("Right Trigger", "RightTrigger", "RT"),
    PADB("Start", "Start", "Start"),
    PADB("Back (map)", "Back", "Back, Touchpad"),
    PADB("Left Stick Click", "LeftStickClick", "LS"),
    PADB("Right Stick Click", "RightStickClick", "RS"),
    PADB("D-pad Up", "DpadUp", "DUp"),
    PADB("D-pad Down", "DpadDown", "DDown"),
    PADB("D-pad Left", "DpadLeft", "DLeft"),
    PADB("D-pad Right", "DpadRight", "DRight"),
#undef PADB

    { P_MODS, s_mods_status, NULL, IT_INFO },
    { P_MODS, "Open Mods Folder", "Mods go in mods\\<mod name>\\files\\ (game files) and \\textures\\ (texture packs).",
      IT_ACTION, NULL, NULL, 0, 0, NULL, NULL, NULL, 0, act_open_mods },
    { P_MODS, "Save Game Textures", "For texture packs: every texture the game shows is saved to texture_dump\\ as a PNG.",
      IT_CHOICE, &g_pc.tex_dump, o_onoff, 2, 0, "TexDump", "save_textures" },
#define MODSLOT(k) { -1, s_mod_name[k], "Turn this mod on or off. Files the game loads from now on use the change.", \
                     IT_CHOICE, &s_mod_on[k], o_onoff, 2, 1, "Mods", s_mod_name[k] }
    MODSLOT(0), MODSLOT(1), MODSLOT(2), MODSLOT(3), MODSLOT(4), MODSLOT(5), MODSLOT(6), MODSLOT(7),
    MODSLOT(8), MODSLOT(9), MODSLOT(10), MODSLOT(11), MODSLOT(12), MODSLOT(13), MODSLOT(14), MODSLOT(15),
    MODSLOT(16), MODSLOT(17), MODSLOT(18), MODSLOT(19), MODSLOT(20), MODSLOT(21), MODSLOT(22), MODSLOT(23),
#undef MODSLOT

    { P_ABOUT, "Settings File", NULL, IT_INFO },
    { P_ABOUT, "Open Settings Folder", "Show settings.ini and the saves folder in Explorer.",
      IT_ACTION, NULL, NULL, 0, 0, NULL, NULL, NULL, 0, act_open_folder },
    { P_ABOUT, "Reset All Settings", "Every option back to its default. Press twice to confirm.",
      IT_ACTION, NULL, NULL, 0, 0, NULL, NULL, NULL, 0, act_reset_all },
    { P_ABOUT, "Close Menu", "Back to the game (also B, Esc or F1).",
      IT_ACTION, NULL, NULL, 0, 0, NULL, NULL, NULL, 0, act_close },
};
#define NITEMS ((int)(sizeof s_items / sizeof s_items[0]))

static int s_page, s_sel[P_COUNT], s_scroll[P_COUNT];
static int s_capture;           /* 1 key, 2 pad: waiting for the new binding */
static int s_cap_item;
static int s_cap_armed;         /* pad capture: all buttons released first */
static DWORD s_cap_t0;
static int s_reset_armed;       /* Reset All pressed once */
static char s_note[160];        /* one-line message under the rows */
static DWORD s_note_t;
static int s_restart_note;

static void note(const char *m) { snprintf(s_note, sizeof s_note, "%s", m); s_note_t = GetTickCount(); }

/* ---- dynamic options ------------------------------------------------------ */
typedef struct { int n; } MonEnum;
static BOOL CALLBACK mon_cb(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    MONITORINFO mi;
    (void)dc; (void)r; (void)lp;
    if (s_nmon >= 9) return FALSE;
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(m, &mi);
    snprintf(s_mon_names[s_nmon], sizeof s_mon_names[0], "%d: %ldx%ld%s", s_nmon,
             mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
             (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (Primary)" : "");
    o_monitor[s_nmon].name = s_mon_names[s_nmon];
    o_monitor[s_nmon].value = s_nmon;
    s_nmon++;
    return TRUE;
}

static void build_options(void)
{
    int i, maxk = 4;
    RECT wa;
    for (i = 0; i <= 20; i++) {
        static char names[21][8];
        snprintf(names[i], sizeof names[i], "%d", i * 5);
        o_volume[i].name = names[i]; o_volume[i].value = i * 5;
    }
    for (i = 0; i < 15; i++) {
        static char fn[15][24];
        int v = 70 + i * 5;
        snprintf(fn[i], sizeof fn[i], v == 100 ? "%d%% (Original)" : "%d%%", v);
        o_fov[i].name = fn[i]; o_fov[i].value = v;
    }
    s_nmon = 0;
    snprintf(s_mon_names[0], sizeof s_mon_names[0], "The Window's Monitor");
    o_monitor[0].name = s_mon_names[0]; o_monitor[0].value = 0;
    s_nmon = 1;
    {
        /* Monitors numbered from 1 in the order Windows lists them. */
        int k = 1;
        HMONITOR dummy = NULL; (void)dummy;
        s_nmon = 1;
        EnumDisplayMonitors(NULL, NULL, mon_cb, 0);
        for (i = 1; i < s_nmon; i++) { /* fix the numbering after the first slot */
            char tmp[64]; snprintf(tmp, sizeof tmp, "%s", s_mon_names[i]);
            { char *c = strchr(tmp, ':'); snprintf(s_mon_names[i], sizeof s_mon_names[0], "%d%s", k++, c ? c : ""); }
            o_monitor[i].value = i;
        }
    }
    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
        maxk = ((wa.bottom - wa.top) - 40) / 480;
        if ((wa.right - wa.left) / 640 < maxk) maxk = (wa.right - wa.left) / 640;
        if (maxk < 1) maxk = 1;
        if (maxk > 8) maxk = 8;
    }
    snprintf(s_win_names[0], sizeof s_win_names[0], "Auto (fit the screen)");
    o_winsize[0].name = s_win_names[0]; o_winsize[0].value = 0;
    s_nwin = 1;
    for (i = 1; i <= maxk && s_nwin < 10; i++) {
        snprintf(s_win_names[s_nwin], sizeof s_win_names[0], "%dx%d",
                 g_pc.aspect == 1 ? (480 * i * 16 + 8) / 9 : 640 * i, 480 * i);
        o_winsize[s_nwin].name = s_win_names[s_nwin]; o_winsize[s_nwin].value = i;
        s_nwin++;
    }
    for (i = 0; i < NITEMS; i++) {
        if (s_items[i].opts == o_monitor) s_items[i].nopts = s_nmon;
        if (s_items[i].opts == o_winsize) s_items[i].nopts = s_nwin;
    }
}

/* ---- key names (as settings.ini [Keyboard] spells them) -------------------- */
static const char *vk_name(int vk, char *buf)
{
    static const struct { int vk; const char *n; } t[] = {
        { VK_UP, "Up" }, { VK_DOWN, "Down" }, { VK_LEFT, "Left" }, { VK_RIGHT, "Right" },
        { VK_RETURN, "Enter" }, { VK_BACK, "Backspace" }, { VK_SPACE, "Space" }, { VK_TAB, "Tab" },
        { VK_SHIFT, "Shift" }, { VK_CONTROL, "Ctrl" }, { VK_MENU, "Alt" }, { VK_ESCAPE, "Escape" },
        { VK_INSERT, "Insert" }, { VK_DELETE, "Delete" }, { VK_HOME, "Home" }, { VK_END, "End" },
        { VK_PRIOR, "PageUp" }, { VK_NEXT, "PageDown" }, { VK_OEM_COMMA, "Comma" },
        { VK_OEM_PERIOD, "Period" }, { VK_OEM_1, "Semicolon" }, { VK_OEM_2, "Slash" },
        { VK_OEM_7, "Quote" }, { VK_OEM_4, "LeftBracket" }, { VK_OEM_6, "RightBracket" },
        { VK_OEM_MINUS, "Minus" }, { VK_OEM_PLUS, "Equals" },
    };
    int i;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { buf[0] = (char)vk; buf[1] = 0; return buf; }
    if (vk >= VK_F1 && vk <= VK_F12) { sprintf(buf, "F%d", vk - VK_F1 + 1); return buf; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { sprintf(buf, "Numpad%d", vk - VK_NUMPAD0); return buf; }
    for (i = 0; i < (int)(sizeof t / sizeof t[0]); i++) if (t[i].vk == vk) return t[i].n;
    return NULL;
}

/* ---- applying and saving -------------------------------------------------- */
static int opt_index(const Item *it)
{
    int i;
    for (i = 0; i < it->nopts; i++) if (it->opts[i].value == *it->field) return i;
    return -1;
}

static void save_choice(const Item *it)
{
    if (it->field == &s_preset) {      /* a preset: its five settings, each saved as usual */
        int p = s_preset, k, i;
        if (p < 1 || p > 4) return;
        for (k = 0; k < 5; k++)
            for (i = 0; i < NITEMS; i++)
                if (s_items[i].field == s_preset_fields[k]) {
                    *s_preset_fields[k] = s_preset_vals[p - 1][k];
                    save_choice(&s_items[i]);
                }
        note(p == 1 ? "Low: 480p, no smoothing or sharpening, least video memory."
           : p == 2 ? "Medium: 720p with light smoothing."
           : p == 3 ? "High: the best resolution that holds 60 fps, full smoothing."
                    : "Ultra: your screen's own resolution, everything on.");
        return;
    }
    if (it->sec && !strcmp(it->sec, "TexDump")) {   /* Save Game Textures, kept under [Mods] */
        pc_settings_set("Mods", "save_textures", *it->field ? "1" : "0");
        note(*it->field ? "Textures are saved to texture_dump\\ as the game shows them." : "Texture saving off.");
        return;
    }
    if (it->sec && !strcmp(it->sec, "Mods")) {
        /* a mod switched on or off: its "disabled" file */
        char p[MAX_PATH];
        snprintf(p, sizeof p, "mods\\%s\\disabled", it->key);
        if (*it->field) {
            DeleteFileA(p);
        } else {
            HANDLE f = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        }
        oogie_mods_rescan();
        oogie_textures_rescan();
        mods_scan();                    /* the count at the top */
        note(*it->field ? "Mod on: files the game loads from now on come from it (some need the next area)."
                        : "Mod off: files the game loads from now on come from the disc.");
        return;
    }
    if (!strcmp(it->key, "window_scale") && *it->field == 0) {
        pc_settings_set(it->sec, it->key, "auto");
    } else if (!strcmp(it->key, "aspect")) {
        static const char *const a[] = { "4:3", "fill", "stretch" };
        pc_settings_set(it->sec, it->key, a[*it->field < 0 || *it->field > 2 ? 0 : *it->field]);
    } else {
        char b[16]; snprintf(b, sizeof b, "%d", *it->field);
        pc_settings_set(it->sec, it->key, b);
    }
    InterlockedIncrement(&g_pc.gen);
    if (it->restart) { s_restart_note = 1; note("This one takes effect the next time the game starts."); }
}

static void change(Item *it, int dir)
{
    int i;
    if (it->type != IT_CHOICE || !it->nopts) return;
    if (it->field == &s_preset) s_preset = preset_match();
    i = opt_index(it);
    if (i < 0) i = 0;
    else i = (i + dir + it->nopts) % it->nopts;
    if (it->field == &s_preset && it->opts[i].value == 0)   /* Custom is shown, not chosen */
        i = (i + dir + it->nopts) % it->nopts;
    *it->field = it->opts[i].value;
    save_choice(it);
    if (it->field == &g_pc.aspect) build_options();
}

static void set_default(Item *it)
{
    if (it->type == IT_CHOICE) { *it->field = it->def; save_choice(it); }
    else if (it->type == IT_KEY) { pc_settings_set(it->sec, it->key, it->keydef); oogie_keymap_reload(); }
    else if (it->type == IT_PAD) { pc_settings_set(it->sec, it->key, it->keydef); pc_input_binds_reload(); }
}

static void act_reset_keys(void)
{
    int i;
    for (i = 0; i < NITEMS; i++) if (s_items[i].type == IT_KEY) pc_settings_set(s_items[i].sec, s_items[i].key, s_items[i].keydef);
    oogie_keymap_reload();
    note("Keyboard keys reset to the default layout.");
}

static void act_reset_pad(void)
{
    int i;
    for (i = 0; i < NITEMS; i++) if (s_items[i].type == IT_PAD) pc_settings_set(s_items[i].sec, s_items[i].key, s_items[i].keydef);
    pc_input_binds_reload();
    note("Controller buttons reset to the original Xbox layout.");
}

static void act_open_folder(void)
{
    char full[MAX_PATH];
    char args[MAX_PATH + 16];
    if (!GetFullPathNameA(pc_settings_path(), sizeof full, full, NULL)) return;
    snprintf(args, sizeof args, "/select,\"%s\"", full);
    ShellExecuteA(NULL, "open", "explorer.exe", args, NULL, SW_SHOWNORMAL);
    note("Opened in Explorer.");
}

static void act_reset_all(void)
{
    int i;
    if (!s_reset_armed) { s_reset_armed = 1; note("Press again to reset every setting."); return; }
    s_reset_armed = 0;
    for (i = 0; i < NITEMS; i++)
        if (s_items[i].type == IT_CHOICE && s_items[i].field && !(s_items[i].sec && !strcmp(s_items[i].sec, "Mods")))
            set_default(&s_items[i]);           /* settings only: mods stay as they are */
    act_reset_keys();
    act_reset_pad();
    note("Every setting is back to its default.");
}

static void act_close(void) { InterlockedExchange(&s_open, 0); }

/* The Mods page's rows, from the folders in mods\. */
static void mods_scan(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int i, n = 0, on = 0;
    for (i = 0; i < NITEMS; i++)
        if (s_items[i].sec && !strcmp(s_items[i].sec, "Mods")) s_items[i].page = -1;
    h = FindFirstFileA("mods\\*", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char chk[MAX_PATH];
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' || n >= MOD_SLOTS) continue;
            snprintf(s_mod_name[n], sizeof s_mod_name[0], "%s", fd.cFileName);
            snprintf(chk, sizeof chk, "mods\\%s\\disabled", fd.cFileName);
            s_mod_on[n] = GetFileAttributesA(chk) == INVALID_FILE_ATTRIBUTES;
            on += s_mod_on[n];
            n++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    for (i = 0; i < NITEMS && n; i++)
        if (s_items[i].sec && !strcmp(s_items[i].sec, "Mods") && s_items[i].field >= &s_mod_on[0]
                && s_items[i].field < &s_mod_on[0] + n)
            s_items[i].page = P_MODS;
    if (n) snprintf(s_mods_status, sizeof s_mods_status, "%d mod%s found, %d on", n, n == 1 ? "" : "s", on);
    else snprintf(s_mods_status, sizeof s_mods_status, "No mods yet");
}

static void act_open_mods(void)
{
    char full[MAX_PATH];
    CreateDirectoryA("mods", NULL);
    if (GetFileAttributesA("mods\\How to add mods.txt") == INVALID_FILE_ATTRIBUTES) {
        FILE *f = fopen("mods\\How to add mods.txt", "w");
        if (f) {
            fputs("Project Oogie mods\n\n"
                  "Each mod is a folder here with a folder named \"files\" inside it.\n"
                  "Put replacement game files in \"files\" under the same path they have on the disc, for example:\n\n"
                  "  mods\\My HD Movies\\files\\Movie\\mv_04.bik\n\n"
                  "Turn mods on or off in the game's settings (F1, Mods page). A mod with a file named \"disabled\"\n"
                  "in its folder is off. If two mods replace the same file, the one whose name sorts first wins.\n"
                  "Textures: see the Texture Packs section of the Mods page help.\n", f);
            fclose(f);
        }
    }
    if (GetFullPathNameA("mods", sizeof full, full, NULL))
        ShellExecuteA(NULL, "open", "explorer.exe", full, NULL, SW_SHOWNORMAL);
    note("Opened the mods folder in Explorer.");
}

/* ---- navigation ------------------------------------------------------------ */
static int page_items(int page, int idx[], int max)
{
    int i, n = 0;
    for (i = 0; i < NITEMS && n < max; i++) if (s_items[i].page == page) idx[n++] = i;
    return n;
}

static int selectable(const Item *it) { return it->type != IT_INFO; }

static void move_sel(int dir)
{
    int idx[64], n = page_items(s_page, idx, 64), k = s_sel[s_page], tries = n;
    if (!n) return;
    do { k = (k + dir + n) % n; } while (!selectable(&s_items[idx[k]]) && --tries > 0);
    s_sel[s_page] = k;
    s_reset_armed = 0;
}

static void fix_sel(void)
{
    int idx[64], n = page_items(s_page, idx, 64);
    if (s_sel[s_page] >= n) s_sel[s_page] = 0;
    if (n && !selectable(&s_items[idx[s_sel[s_page]]])) move_sel(1);
}

static void page_turn(int dir)
{
    s_page = (s_page + dir + P_COUNT) % P_COUNT;
    s_reset_armed = 0;
    fix_sel();
}

static Item *cur_item(void)
{
    int idx[64], n = page_items(s_page, idx, 64);
    return n ? &s_items[idx[s_sel[s_page]]] : NULL;
}

static void activate(void)
{
    Item *it = cur_item();
    if (!it) return;
    if (it->type == IT_CHOICE) change(it, 1);
    else if (it->type == IT_ACTION && it->action) it->action();
    else if (it->type == IT_KEY) { s_capture = 1; s_cap_item = (int)(it - s_items); s_cap_t0 = GetTickCount(); }
    else if (it->type == IT_PAD) { s_capture = 2; s_cap_item = (int)(it - s_items); s_cap_armed = 0; s_cap_t0 = GetTickCount(); }
}

static void open_menu(void)
{
    build_options();
    mods_scan();
    fix_sel();
    s_capture = 0;
    s_reset_armed = 0;
    s_note[0] = 0;
    InterlockedExchange(&s_open, 1);
}

int pc_menu_update_state(void)
{
    if (InterlockedExchange(&s_toggle_req, 0)) {
        if (s_open) { InterlockedExchange(&s_open, 0); return 0; }
        open_menu();
        return 1;
    }
    return 0;
}

int pc_menu_key(int vk, int down)
{
    if ((unsigned)vk < 256) {
        if (down) s_keys_down[vk] = 1;
        else if (!s_open) { s_keys_down[vk] = 0; return 0; }
        else s_keys_down[vk] = 0;
    }
    if (!s_open) {
        if (down && vk == VK_F1) { open_menu(); return 1; }
        return 0;
    }
    if (!down) return 1;
    if (s_capture == 1) {
        Item *it = &s_items[s_cap_item];
        char b[16];
        const char *n;
        if (vk == VK_ESCAPE) { s_capture = 0; return 1; }
        if (vk == VK_DELETE) { pc_settings_set(it->sec, it->key, "none"); s_capture = 0; oogie_keymap_reload(); return 1; }
        if (vk == VK_F1 || vk == VK_F11) return 1;
        n = vk_name(vk, b);
        if (n) { pc_settings_set(it->sec, it->key, n); oogie_keymap_reload(); s_capture = 0; }
        return 1;
    }
    if (s_capture == 2) {
        if (vk == VK_ESCAPE || vk == VK_BACK) s_capture = 0;
        return 1;
    }
    switch (vk) {
    case VK_F1: case VK_ESCAPE: case VK_BACK: InterlockedExchange(&s_open, 0); break;
    case VK_UP: case 'W': move_sel(-1); break;
    case VK_DOWN: case 'S': move_sel(1); break;
    case VK_LEFT: case 'A': { Item *it = cur_item(); if (it) change(it, -1); } break;
    case VK_RIGHT: case 'D': { Item *it = cur_item(); if (it) change(it, 1); } break;
    case VK_RETURN: case VK_SPACE: activate(); break;
    case 'Q': case VK_PRIOR: page_turn(-1); break;
    case 'E': case VK_NEXT: case VK_TAB: page_turn((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1); break;
    case VK_DELETE: { Item *it = cur_item(); if (it) set_default(it); } break;
    }
    return 1;
}

/* Pad navigation, polled every menu frame: edges, with auto-repeat on the
 * directions. */
static void pad_poll(void)
{
    static uint32_t prev;
    static DWORD rep_t;
    uint32_t b = pc_input_raw_buttons(), edge = b & ~prev;
    DWORD now = GetTickCount();
    const uint32_t dirs = (1u << PB_DUP) | (1u << PB_DDOWN) | (1u << PB_DLEFT) | (1u << PB_DRIGHT);
    /* Held direction: first repeat 410 ms after the press, then every 110 ms.
     * The test is signed: rep_t is set in the future at the press, and as an
     * unsigned difference that read as long past, so every press repeated on
     * the next frame (owner 2026-10-25: rows skipped with a pad). */
    if ((b & dirs) && (b & dirs) == (prev & dirs)) {
        if ((LONG)(now - rep_t) > 110) { edge |= b & dirs; rep_t = now; }
    } else if (edge & dirs) {
        rep_t = now + 300;                 /* first repeat after 410 ms */
    }
    prev = b;
    if (s_capture == 2) {
        int i;
        if (!s_cap_armed) { if (!b) s_cap_armed = 1; return; }
        for (i = 0; i < PB_COUNT; i++)
            if ((edge & (1u << i)) && i != PB_GUIDE) {
                Item *it = &s_items[s_cap_item];
                pc_settings_set(it->sec, it->key, pc_input_button_name(i));
                pc_input_binds_reload();
                s_capture = 0;
                return;
            }
        if (now - s_cap_t0 > 8000) s_capture = 0;
        return;
    }
    if (s_capture) return;
    if (edge & (1u << PB_DUP)) move_sel(-1);
    if (edge & (1u << PB_DDOWN)) move_sel(1);
    if (edge & (1u << PB_DLEFT)) { Item *it = cur_item(); if (it) change(it, -1); }
    if (edge & (1u << PB_DRIGHT)) { Item *it = cur_item(); if (it) change(it, 1); }
    if (edge & (1u << PB_A)) activate();
    if (edge & ((1u << PB_B) | (1u << PB_START))) {
        if (!(b & (1u << PB_BACK))) InterlockedExchange(&s_open, 0);
    }
    if (edge & (1u << PB_Y)) { Item *it = cur_item(); if (it) set_default(it); }
    if (edge & ((1u << PB_LB) | (1u << PB_LT))) page_turn(-1);
    if (edge & ((1u << PB_RB) | (1u << PB_RT))) page_turn(1);
}

/* ---- drawing ------------------------------------------------------------- */
#define FONT_PX 72
#define ATLAS_W 2048
#define ATLAS_H 1024
#define MOON_PX 192

typedef struct { short x, y, w, h, xoff, adv; } Glyph;
static Glyph s_glyph[128];
static int s_line_h;
static int s_gfx_failed, s_gfx_ok;
static ID3D11ShaderResourceView *s_atlas_srv;
static ID3D11VertexShader *s_vs;
static ID3D11PixelShader *s_ps;
static ID3D11InputLayout *s_il;
static ID3D11Buffer *s_vb, *s_cb;
static ID3D11SamplerState *s_smp;
static ID3D11BlendState *s_blend;
static ID3D11RasterizerState *s_rs;
static ID3D11DepthStencilState *s_ds;
static float s_white_u, s_white_v;
static float s_moon_uv[4];

typedef struct { float x, y, u, v; UINT32 c; } MV;
#define MAXV 60000
static MV s_v[MAXV];
static int s_nv;
static float s_sc, s_ox, s_oy;   /* design canvas (1600x900) -> pixels */
/* Three layers, drawn in order: 0 shapes under the art, 1 the game's art
 * (full colour, s_art_srv), 2 text and shapes over it. */
#define LAYERV (MAXV / 3)
static MV s_lv[3][LAYERV];
static int s_ln[3];
static int s_layer;

/* ---- the game's own menu art (dev cycle 4) ---------------------------------
 * The pause menu's frames, hook cursor, vines and button pictures, read from
 * the player's disc (game\stm_data\sbuty_u.stm; format in
 * docs/REVERSE_ENGINEERING.md: .stm entries of 24-byte name, offset, size,
 * folders with bit 31 set; .pig files are DDS, DXT5) and packed into one
 * 512x256 texture: sbmenu03 at (0,0), sbmenu12 at (256,0), button00 at
 * (256,64). Piece rectangles were measured from the sheets' transparency
 * (the disc is checked to be the USA release). Without the file the menu
 * keeps its drawn look. OOGIE_MENU_ART=0 turns the art off. */
#define ART_W 512
#define ART_H 256
typedef struct { short x, y, w, h; } Piece;
static const Piece ART_FRAME = { 6, 180, 115, 73 };     /* rough rectangle frame */
static const Piece ART_HOOK  = { 2, 82, 51, 43 };       /* hook cursor (points left) */
static const Piece ART_VINE  = { 2, 145, 140, 30 };     /* vine divider */
static const Piece ART_ORN   = { 256 + 73, 1, 183, 37 };/* ornamental vine with arrows */
static const Piece ART_BTN_B = { 256 + 1, 64 + 1, 19, 22 };
static const Piece ART_BTN_A = { 256 + 23, 64 + 1, 19, 22 };
static const Piece ART_BTN_Y = { 256 + 45, 64 + 2, 19, 20 };
static ID3D11ShaderResourceView *s_art_srv;
static ID3D11PixelShader *s_ps_art;
static int s_art_ok;

static UINT32 RGBA_(int r, int g, int b, int a) { return (UINT32)r | ((UINT32)g << 8) | ((UINT32)b << 16) | ((UINT32)a << 24); }

/* Palette taken from the game's Chapter Select screen. */
#define C_GOLD_T   RGBA_(250, 222, 140, 255)
#define C_GOLD_B   RGBA_(214, 150, 54, 255)
#define C_ORNG_T   RGBA_(252, 170, 70, 255)
#define C_ORNG_B   RGBA_(196, 84, 20, 255)
#define C_OLIVE_T  RGBA_(160, 152, 104, 255)
#define C_OLIVE_B  RGBA_(112, 104, 70, 255)
#define C_PALE     RGBA_(214, 206, 176, 255)
#define C_SHADOW   RGBA_(0, 0, 0, 200)

static int load_file(const char *path, void **data, DWORD *size)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n;
    if (f == INVALID_HANDLE_VALUE) return 0;
    *size = GetFileSize(f, NULL);
    *data = malloc(*size ? *size : 1);
    if (!*data || !ReadFile(f, *data, *size, &n, NULL) || n != *size) { CloseHandle(f); free(*data); return 0; }
    CloseHandle(f);
    return 1;
}

static int font_atlas(ID3D11Device *dev)
{
    static const char *const names[] = {
        "assets\\fonts\\MountainsofChristmas-Bold.ttf",
        "..\\assets\\fonts\\MountainsofChristmas-Bold.ttf",
    };
    char exedir[MAX_PATH], path[MAX_PATH];
    void *ttf = NULL; DWORD ttf_n = 0, nf = 0;
    const char *face = "Georgia";
    HDC dc;
    HBITMAP bmp;
    BITMAPINFO bi;
    UINT32 *bits = NULL;
    HFONT font;
    TEXTMETRICA tm;
    int c, x = 4, y = 4, i;
    UINT32 *rgba;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Texture2D *tex = NULL;

    GetModuleFileNameA(NULL, exedir, sizeof exedir);
    { char *s = strrchr(exedir, '\\'); if (s) *s = 0; }
    for (i = 0; i < 4 && !ttf; i++) {
        if (i < 2) snprintf(path, sizeof path, "%s", names[i]);
        else snprintf(path, sizeof path, "%s\\%s", exedir, names[i - 2]);
        if (load_file(path, &ttf, &ttf_n) && AddFontMemResourceEx(ttf, ttf_n, NULL, &nf))
            face = "Mountains of Christmas";
    }
    if (strcmp(face, "Georgia"))
        fprintf(stderr, "[MENU] font: Mountains of Christmas (%s)\n", path);
    else
        fprintf(stderr, "[MENU] font file not found (assets\\fonts): using Georgia\n");

    dc = CreateCompatibleDC(NULL);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = ATLAS_W; bi.bmiHeader.biHeight = -ATLAS_H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!bmp || !bits) { DeleteDC(dc); return 0; }
    SelectObject(dc, bmp);
    memset(bits, 0, (size_t)ATLAS_W * ATLAS_H * 4);
    font = CreateFontA(-FONT_PX, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
    SelectObject(dc, font);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    GetTextMetricsA(dc, &tm);
    s_line_h = tm.tmHeight;
    for (c = 32; c < 127; c++) {
        ABC abc; char ch = (char)c;
        int w, pad = 6;
        if (!GetCharABCWidthsA(dc, (UINT)c, (UINT)c, &abc)) { abc.abcA = 0; abc.abcB = FONT_PX / 2; abc.abcC = 0; }
        w = (int)abc.abcB + pad * 2;
        if (x + w + 4 > ATLAS_W) { x = 4; y += s_line_h + 6; }
        if (y + s_line_h + 4 > ATLAS_H - MOON_PX - 8) break;
        TextOutA(dc, x + pad - abc.abcA, y, &ch, 1);
        s_glyph[c].x = (short)x; s_glyph[c].y = (short)y;
        s_glyph[c].w = (short)w; s_glyph[c].h = (short)s_line_h;
        s_glyph[c].xoff = (short)(abc.abcA - pad);
        s_glyph[c].adv = (short)(abc.abcA + abc.abcB + abc.abcC);
        x += w + 4;
    }
    GdiFlush();
    rgba = (UINT32 *)malloc((size_t)ATLAS_W * ATLAS_H * 4);
    if (!rgba) return 0;
    for (i = 0; i < ATLAS_W * ATLAS_H; i++) {
        UINT32 p = bits[i];
        UINT32 r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        UINT32 a = r > g ? (r > b ? r : b) : (g > b ? g : b);
        rgba[i] = 0x00FFFFFFu | (a << 24);
    }
    /* A white block for flat shapes, and a soft full moon. */
    for (y = ATLAS_H - 8; y < ATLAS_H; y++) for (x = 0; x < 8; x++) rgba[y * ATLAS_W + x] = 0xFFFFFFFFu;
    s_white_u = 4.0f / ATLAS_W; s_white_v = (ATLAS_H - 4.0f) / ATLAS_H;
    {
        int mx = ATLAS_W - MOON_PX - 4, my = ATLAS_H - MOON_PX - 4;
        float r0 = MOON_PX * 0.5f;
        for (y = 0; y < MOON_PX; y++) for (x = 0; x < MOON_PX; x++) {
            float dx = x + 0.5f - r0, dy = y + 0.5f - r0, d = sqrtf(dx * dx + dy * dy) / r0;
            float disc = d < 0.62f ? 1.0f : (d < 0.66f ? (0.66f - d) / 0.04f : 0.0f);
            float glow = d < 1.0f ? (1.0f - d) * 0.35f : 0.0f;
            float a = disc + glow * (1.0f - disc);
            rgba[(my + y) * ATLAS_W + mx + x] = 0x00FFFFFFu | ((UINT32)(a * 255.0f) << 24);
        }
        s_moon_uv[0] = (float)mx / ATLAS_W; s_moon_uv[1] = (float)my / ATLAS_H;
        s_moon_uv[2] = (float)(mx + MOON_PX) / ATLAS_W; s_moon_uv[3] = (float)(my + MOON_PX) / ATLAS_H;
    }
    DeleteObject(font); DeleteObject(bmp); DeleteDC(dc);

    memset(&td, 0, sizeof td);
    td.Width = ATLAS_W; td.Height = ATLAS_H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = rgba; sd.SysMemPitch = ATLAS_W * 4; sd.SysMemSlicePitch = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, &sd, &tex))) { free(rgba); return 0; }
    free(rgba);
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &s_atlas_srv);
    ID3D11Texture2D_Release(tex);
    return s_atlas_srv != NULL;
}

static const char s_hlsl[] =
"cbuffer C : register(b0) { float4 inv; };\n"
"Texture2D T : register(t0); SamplerState S : register(s0);\n"
"struct VI { float2 p : POSITION; float2 t : TEXCOORD0; float4 c : COLOR0; };\n"
"struct VO { float4 p : SV_Position; float2 t : TEXCOORD0; float4 c : COLOR0; };\n"
"VO vs(VI i) { VO o; o.p = float4(i.p.x * inv.x * 2.0 - 1.0, 1.0 - i.p.y * inv.y * 2.0, 0, 1);\n"
"  o.t = i.t; o.c = i.c; return o; }\n"
"float4 ps(VO i) : SV_Target { float a = T.Sample(S, i.t).a; return float4(i.c.rgb, i.c.a * a); }\n"
"float4 psart(VO i) : SV_Target { return T.Sample(S, i.t) * i.c; }\n";

/* A file inside an .stm archive by its path ("menu/pig/sbmenu03.pig"). */
static const BYTE *stm_find(const BYTE *d, DWORD n, DWORD off, DWORD count, const char *path, DWORD *size)
{
    char part[25];
    const char *slash = strchr(path, '/');
    size_t len = slash ? (size_t)(slash - path) : strlen(path);
    DWORD k;
    if (len > 24) return NULL;
    memcpy(part, path, len); part[len] = 0;
    for (k = 0; k < count && off + k * 32 + 32 <= n; k++) {
        const BYTE *e = d + off + k * 32;
        char name[25];
        DWORD o, s;
        memcpy(name, e, 24); name[24] = 0;
        memcpy(&o, e + 24, 4); memcpy(&s, e + 28, 4);
        if (_stricmp(name, part)) continue;
        if (s & 0x80000000u)
            return slash ? stm_find(d, n, o, s & 0x7FFFFFFFu, slash + 1, size) : NULL;
        if (slash || o + s > n) return NULL;
        *size = s;
        return d + o;
    }
    return NULL;
}

/* A DXT5 (BC3) DDS into the art texture at (ox, oy). */
static int dds_dxt5_blit(const BYTE *dds, DWORD size, UINT32 *dst, int ox, int oy)
{
    DWORD w, h, bx, by;
    const BYTE *b;
    if (size < 128 || memcmp(dds, "DDS ", 4) || memcmp(dds + 84, "DXT5", 4)) return 0;
    memcpy(&h, dds + 12, 4); memcpy(&w, dds + 16, 4);
    if (ox + (int)w > ART_W || oy + (int)h > ART_H || 128 + ((w + 3) / 4) * ((h + 3) / 4) * 16 > size) return 0;
    b = dds + 128;
    for (by = 0; by < (h + 3) / 4; by++)
        for (bx = 0; bx < (w + 3) / 4; bx++, b += 16) {
            int a[8], i, px;
            UINT32 c[4], cbits, abits_lo, abits_hi;
            UINT16 c0 = (UINT16)(b[8] | (b[9] << 8)), c1 = (UINT16)(b[10] | (b[11] << 8));
            int r0 = (c0 >> 11) & 31, g0 = (c0 >> 5) & 63, b0 = c0 & 31;
            int r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
            int R[4], G[4], B[4];
            R[0] = (r0 << 3) | (r0 >> 2); G[0] = (g0 << 2) | (g0 >> 4); B[0] = (b0 << 3) | (b0 >> 2);
            R[1] = (r1 << 3) | (r1 >> 2); G[1] = (g1 << 2) | (g1 >> 4); B[1] = (b1 << 3) | (b1 >> 2);
            R[2] = (2 * R[0] + R[1]) / 3; G[2] = (2 * G[0] + G[1]) / 3; B[2] = (2 * B[0] + B[1]) / 3;
            R[3] = (R[0] + 2 * R[1]) / 3; G[3] = (G[0] + 2 * G[1]) / 3; B[3] = (B[0] + 2 * B[1]) / 3;
            for (i = 0; i < 4; i++) c[i] = (UINT32)R[i] | ((UINT32)G[i] << 8) | ((UINT32)B[i] << 16);
            a[0] = b[0]; a[1] = b[1];
            for (i = 1; i < 7; i++)
                a[i + 1] = a[0] > a[1] ? ((7 - i) * a[0] + i * a[1]) / 7
                         : (i <= 4 ? ((5 - i) * a[0] + i * a[1]) / 5 : (i == 5 ? 0 : 255));
            abits_lo = (UINT32)b[2] | ((UINT32)b[3] << 8) | ((UINT32)b[4] << 16);
            abits_hi = (UINT32)b[5] | ((UINT32)b[6] << 8) | ((UINT32)b[7] << 16);
            cbits = (UINT32)b[12] | ((UINT32)b[13] << 8) | ((UINT32)b[14] << 16) | ((UINT32)b[15] << 24);
            for (px = 0; px < 16; px++) {
                DWORD x = bx * 4 + (px & 3), y = by * 4 + (px >> 2);
                int ai = (int)(((px < 8 ? abits_lo >> (3 * px) : abits_hi >> (3 * (px - 8)))) & 7);
                if (x < w && y < h)
                    dst[(oy + y) * ART_W + ox + x] = c[(cbits >> (2 * px)) & 3] | ((UINT32)a[ai] << 24);
            }
        }
    return 1;
}

static void art_load(ID3D11Device *dev)
{
    static const struct { const char *path; int x, y; } sheets[3] = {
        { "menu/pig/sbmenu03.pig", 0, 0 }, { "menu/pig/sbmenu12.pig", 256, 0 }, { "common/pig/button00.pig", 256, 64 } };
    const char *off = getenv("OOGIE_MENU_ART");
    void *d = NULL;
    DWORD n = 0, root, cnt, i;
    UINT32 *px;
    int ok = 1;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Texture2D *tex = NULL;
    if (off && *off == '0') return;
    if (!load_file("game\\stm_data\\sbuty_u.stm", &d, &n) || n < 32 || memcmp(d, "@STM", 4)) {
        fprintf(stderr, "[MENU] game menu art not found (game\\stm_data\\sbuty_u.stm): drawn frames instead\n");
        free(d);
        return;
    }
    memcpy(&root, (BYTE *)d + 24, 4); memcpy(&cnt, (BYTE *)d + 28, 4);
    (void)root;
    px = (UINT32 *)calloc((size_t)ART_W * ART_H, 4);
    for (i = 0; px && i < 3; i++) {
        DWORD sz = 0;
        const BYTE *p = stm_find((const BYTE *)d, n, 0x20, cnt, sheets[i].path, &sz);
        if (!p || !dds_dxt5_blit(p, sz, px, sheets[i].x, sheets[i].y)) {
            fprintf(stderr, "[MENU] menu art: %s not readable\n", sheets[i].path);
            ok = 0;
        }
    }
    free(d);
    if (!px || !ok) { free(px); return; }
    memset(&td, 0, sizeof td);
    td.Width = ART_W; td.Height = ART_H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = px; sd.SysMemPitch = ART_W * 4; sd.SysMemSlicePitch = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, &sd, &tex))) {
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &s_art_srv);
        ID3D11Texture2D_Release(tex);
    }
    free(px);
    s_art_ok = s_art_srv != NULL && s_ps_art != NULL;
    fprintf(stderr, s_art_ok ? "[MENU] game menu art read from the disc (frames, cursor, vines, buttons)\n"
                             : "[MENU] menu art texture failed: drawn frames instead\n");
}

static int gfx_init(ID3D11Device *dev)
{
    ID3DBlob *b = NULL, *err = NULL;
    D3D11_INPUT_ELEMENT_DESC il[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC ss;
    D3D11_BLEND_DESC bl;
    D3D11_RASTERIZER_DESC rd;
    D3D11_DEPTH_STENCIL_DESC dd;

    if (!font_atlas(dev)) return 0;
    if (FAILED(D3DCompile(s_hlsl, sizeof s_hlsl - 1, "menu", NULL, NULL, "vs", "vs_4_0", 0, 0, &b, &err))) return 0;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_vs);
    ID3D11Device_CreateInputLayout(dev, il, 3, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), &s_il);
    ID3D10Blob_Release(b);
    if (FAILED(D3DCompile(s_hlsl, sizeof s_hlsl - 1, "menu", NULL, NULL, "ps", "ps_4_0", 0, 0, &b, &err))) return 0;
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_ps);
    ID3D10Blob_Release(b);
    if (SUCCEEDED(D3DCompile(s_hlsl, sizeof s_hlsl - 1, "menu", NULL, NULL, "psart", "ps_4_0", 0, 0, &b, &err))) {
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_ps_art);
        ID3D10Blob_Release(b);
        art_load(dev);
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof s_v; bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_vb);
    bd.ByteWidth = 16; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_cb);
    memset(&ss, 0, sizeof ss);
    ss.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ss.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(dev, &ss, &s_smp);
    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(dev, &bl, &s_blend);
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(dev, &rd, &s_rs);
    memset(&dd, 0, sizeof dd);
    ID3D11Device_CreateDepthStencilState(dev, &dd, &s_ds);
    return s_vs && s_ps && s_il && s_vb && s_cb && s_smp && s_blend && s_rs && s_ds;
}

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                 UINT32 ct, UINT32 cb)
{
    MV *v;
    if (s_ln[s_layer] + 6 > LAYERV) return;
    x0 = s_ox + x0 * s_sc; x1 = s_ox + x1 * s_sc;
    y0 = s_oy + y0 * s_sc; y1 = s_oy + y1 * s_sc;
    v = s_lv[s_layer] + s_ln[s_layer];
    v[0].x = x0; v[0].y = y0; v[0].u = u0; v[0].v = v0; v[0].c = ct;
    v[1].x = x1; v[1].y = y0; v[1].u = u1; v[1].v = v0; v[1].c = ct;
    v[2].x = x0; v[2].y = y1; v[2].u = u0; v[2].v = v1; v[2].c = cb;
    v[3] = v[1];
    v[4].x = x1; v[4].y = y1; v[4].u = u1; v[4].v = v1; v[4].c = cb;
    v[5] = v[2];
    s_ln[s_layer] += 6;
}

/* A piece of the game's art (layer 1), tinted by c; flip mirrors it. */
static void art(const Piece *p, float x0, float y0, float x1, float y1, UINT32 c, int flip)
{
    float u0 = (float)p->x / ART_W, u1 = (float)(p->x + p->w) / ART_W;
    float v0 = (float)p->y / ART_H, v1 = (float)(p->y + p->h) / ART_H;
    int sv = s_layer;
    s_layer = 1;
    if (flip) { float t = u0; u0 = u1; u1 = t; }
    quad(x0, y0, x1, y1, u0, v0, u1, v1, c, c);
    s_layer = sv;
}

/* A part of a piece (source pixels sx..sx+sw, sy..sy+sh) on a rectangle. */
static void art_part(const Piece *p, float sx, float sy, float sw, float sh,
                     float x0, float y0, float x1, float y1, UINT32 c)
{
    Piece q;
    q.x = (short)(p->x + sx); q.y = (short)(p->y + sy); q.w = (short)sw; q.h = (short)sh;
    art(&q, x0, y0, x1, y1, c, 0);
}

/* A frame piece stretched over a rectangle by nine slices: the corners keep
 * their shape (m source pixels, drawn k times larger), the edges stretch. */
static void art_frame(const Piece *p, float x0, float y0, float x1, float y1, float m, float k, UINT32 c)
{
    float w = p->w, h = p->h, cm = m * k;
    art_part(p, 0, 0, m, m, x0, y0, x0 + cm, y0 + cm, c);
    art_part(p, w - m, 0, m, m, x1 - cm, y0, x1, y0 + cm, c);
    art_part(p, 0, h - m, m, m, x0, y1 - cm, x0 + cm, y1, c);
    art_part(p, w - m, h - m, m, m, x1 - cm, y1 - cm, x1, y1, c);
    art_part(p, m, 0, w - 2 * m, m, x0 + cm, y0, x1 - cm, y0 + cm, c);
    art_part(p, m, h - m, w - 2 * m, m, x0 + cm, y1 - cm, x1 - cm, y1, c);
    art_part(p, 0, m, m, h - 2 * m, x0, y0 + cm, x0 + cm, y1 - cm, c);
    art_part(p, w - m, m, m, h - 2 * m, x1 - cm, y0 + cm, x1, y1 - cm, c);
}

/* A divider: the ornamental vine in the middle, plain vine repeated out to
 * both ends. */
static void art_divider(float x0, float x1, float yc, UINT32 c)
{
    float oh = 34, ow = ART_ORN.w * oh / ART_ORN.h, cx = (x0 + x1) * 0.5f;
    float vh = 22, vw = ART_VINE.w * vh / ART_VINE.h, x;
    for (x = cx - ow * 0.5f - vw; x > x0 - vw * 0.5f; x -= vw)
        art(&ART_VINE, x, yc - vh * 0.5f, x + vw, yc + vh * 0.5f, c, 0);
    for (x = cx + ow * 0.5f; x < x1 - vw * 0.5f; x += vw)
        art(&ART_VINE, x, yc - vh * 0.5f, x + vw, yc + vh * 0.5f, c, 1);
    art(&ART_ORN, cx - ow * 0.5f, yc - oh * 0.5f, cx + ow * 0.5f, yc + oh * 0.5f, c, 0);
}

static void rect(float x0, float y0, float x1, float y1, UINT32 ct, UINT32 cb)
{
    quad(x0, y0, x1, y1, s_white_u, s_white_v, s_white_u, s_white_v, ct, cb);
}

static float text_w(const char *s, float px)
{
    float k = px / FONT_PX, w = 0;
    for (; *s; s++) { int c = (unsigned char)*s; if (c < 32 || c > 126) c = '?'; w += s_glyph[c].adv * k; }
    return w;
}

static void text_pass(float x, float y, float px, const char *s, UINT32 ct, UINT32 cb)
{
    float k = px / FONT_PX;
    int sv = s_layer;
    s_layer = 2;                        /* text over the art */
    for (; *s; s++) {
        int c = (unsigned char)*s;
        const Glyph *g;
        if (c < 32 || c > 126) c = '?';
        g = &s_glyph[c];
        if (c != ' ')
            quad(x + g->xoff * k, y, x + (g->xoff + g->w) * k, y + g->h * k,
                 (float)g->x / ATLAS_W, (float)g->y / ATLAS_H,
                 (float)(g->x + g->w) / ATLAS_W, (float)(g->y + g->h) / ATLAS_H, ct, cb);
        x += g->adv * k;
    }
    s_layer = sv;
}

/* align: 0 left, 1 centre, 2 right. The game's lettering has a dark edge
 * and a soft drop shadow. */
static void text(float x, float y, float px, const char *s, UINT32 ct, UINT32 cb, int align)
{
    float w = text_w(s, px), o = px * 0.035f + 0.8f;
    if (align == 1) x -= w * 0.5f; else if (align == 2) x -= w;
    text_pass(x + o * 1.6f, y + o * 2.0f, px, s, RGBA_(0, 0, 0, 140), RGBA_(0, 0, 0, 140));
    text_pass(x - o, y, px, s, C_SHADOW, C_SHADOW);
    text_pass(x + o, y, px, s, C_SHADOW, C_SHADOW);
    text_pass(x, y - o, px, s, C_SHADOW, C_SHADOW);
    text_pass(x, y + o, px, s, C_SHADOW, C_SHADOW);
    text_pass(x, y, px, s, ct, cb);
}

static void moon(float cx, float cy, float r, UINT32 c)
{
    quad(cx - r, cy - r, cx + r, cy + r, s_moon_uv[0], s_moon_uv[1], s_moon_uv[2], s_moon_uv[3], c, c);
}

/* Value text of a row. */
static const char *item_value(const Item *it, char *buf, size_t cap)
{
    if (it->type == IT_CHOICE) {
        int i = opt_index(it);
        if (i < 0) { snprintf(buf, cap, "%d", *it->field); return buf; }
        return it->opts[i].name;
    }
    if (it->type == IT_KEY || it->type == IT_PAD) {
        const char *v = pc_settings_get(it->sec, it->key);
        snprintf(buf, cap, "%s", v ? v : it->keydef);
        return buf;
    }
    if (it->type == IT_INFO && !strcmp(it->label, "Settings File")) {
        char full[MAX_PATH];
        if (GetFullPathNameA(pc_settings_path(), sizeof full, full, NULL)) snprintf(buf, cap, "%s", full);
        else snprintf(buf, cap, "%s", pc_settings_path());
        return buf;
    }
    return "";
}

static void draw_menu(UINT W, UINT H)
{
    int idx[64], n, i, vis = 7, first;   /* rows shown; more scroll (9 ran into the help line) */
    float px0 = 170, px1 = 1430, y;
    Item *sel;
    char vb[300];
    DWORD now = GetTickCount();

    /* Dim the paused game, then the panel: dark, with a faint cloudy band. */
    s_ox = 0; s_oy = 0; s_sc = 1;
    rect(0, 0, (float)W, (float)H, RGBA_(6, 4, 10, 170), RGBA_(6, 4, 10, 200));
    s_sc = (float)W / 1600.0f < (float)H / 900.0f ? (float)W / 1600.0f : (float)H / 900.0f;
    s_ox = ((float)W - 1600.0f * s_sc) * 0.5f;
    s_oy = ((float)H - 900.0f * s_sc) * 0.5f;
    rect(px0, 40, px1, 860, RGBA_(28, 24, 34, 236), RGBA_(14, 12, 18, 244));
    rect(px0, 40, px1, 300, RGBA_(70, 62, 84, 70), RGBA_(70, 62, 84, 0));
    moon(1290, 128, 82, RGBA_(232, 220, 160, 230));
    if (s_art_ok) {
        /* the game's own rough hand-drawn frame */
        art_frame(&ART_FRAME, px0 - 18, 24, px1 + 18, 876, 20, 2.6f, RGBA_(236, 226, 200, 255));
    } else {
    /* Riveted frame: a gold-brown edge with a dark inner line. */
    rect(px0, 40, px1, 44, RGBA_(150, 112, 50, 255), RGBA_(110, 80, 34, 255));
    rect(px0, 856, px1, 860, RGBA_(110, 80, 34, 255), RGBA_(150, 112, 50, 255));
    rect(px0, 40, px0 + 4, 860, RGBA_(150, 112, 50, 255), RGBA_(110, 80, 34, 255));
    rect(px1 - 4, 40, px1, 860, RGBA_(150, 112, 50, 255), RGBA_(110, 80, 34, 255));
    for (i = 0; i < 14; i++) {
        float rx = px0 + 30 + i * (px1 - px0 - 60) / 13.0f;
        rect(rx - 3, 39, rx + 3, 45, RGBA_(200, 170, 110, 255), RGBA_(90, 70, 40, 255));
        rect(rx - 3, 855, rx + 3, 861, RGBA_(200, 170, 110, 255), RGBA_(90, 70, 40, 255));
    }
    }

    text(px0 + 60, 62, 46, "Oogie's Revenge", C_ORNG_T, C_ORNG_B, 0);
    text(px0 + 60, 104, 92, "Recomp Settings", C_GOLD_T, C_GOLD_B, 0);

    /* Page tabs. */
    {
        float tx = px0 + 60, ty = 232, tp = 36;
        for (i = 0; i < P_COUNT; i++) {
            float w = text_w(s_page_names[i], tp);
            if (i == s_page) {
                text(tx, ty, tp, s_page_names[i], C_ORNG_T, C_ORNG_B, 0);
                rect(tx, ty + tp * 1.12f, tx + w, ty + tp * 1.12f + 3, C_ORNG_T, C_ORNG_B);
            } else {
                text(tx, ty, tp, s_page_names[i], C_OLIVE_T, C_OLIVE_B, 0);
            }
            tx += w + 34;
        }
        if (s_art_ok) art_divider(px0 + 40, px1 - 40, 292, RGBA_(200, 180, 140, 230));
        else rect(px0 + 40, 288, px1 - 40, 290, RGBA_(120, 96, 52, 200), RGBA_(120, 96, 52, 200));
    }

    /* Rows. */
    n = page_items(s_page, idx, 64);
    if (s_sel[s_page] < s_scroll[s_page]) s_scroll[s_page] = s_sel[s_page];
    if (s_sel[s_page] >= s_scroll[s_page] + vis) s_scroll[s_page] = s_sel[s_page] - vis + 1;
    first = s_scroll[s_page];
    y = 306;
    sel = cur_item();
    if (first > 0) text(px1 - 70, 286, 34, "^", C_OLIVE_T, C_OLIVE_B, 1);
    for (i = first; i < n && i < first + vis; i++) {
        Item *it = &s_items[idx[i]];
        int on = it == sel;
        const char *v;
        if (it->type == IT_INFO && !strcmp(it->label, "Connected Controllers")) {
            char pads[4][96];
            int np = pc_input_pad_list(pads, 4), k;
            text(px0 + 80, y + 6, 38, it->label, C_OLIVE_T, C_OLIVE_B, 0);
            if (!np) text(px0 + 700, y + 6, 34, "None (keyboard)", C_PALE, C_PALE, 0);
            for (k = 0; k < np; k++) {
                text(px0 + 700, y + 6, 34, pads[k], C_PALE, C_PALE, 0);
                if (k + 1 < np) y += 44;
            }
            y += 56;
            continue;
        }
        if (on) {
            rect(px0 + 50, y, px1 - 50, y + 54, RGBA_(96, 48, 14, 150), RGBA_(60, 26, 6, 150));
            if (s_art_ok) {
                /* the pause menu's hook cursor, mirrored to point at the row */
                float bob = (float)sin((double)(now % 100000) / 170.0) * 4.0f;
                art(&ART_HOOK, px0 + 16 + bob, y + 4, px0 + 16 + bob + 56, y + 4 + 47, RGBA_(252, 196, 110, 255), 1);
            } else {
                rect(px0 + 50, y, px0 + 54, y + 54, C_ORNG_T, C_ORNG_B);
                rect(px1 - 54, y, px1 - 50, y + 54, C_ORNG_T, C_ORNG_B);
                rect(px0 + 20, y + 26, px0 + 50, y + 28, C_ORNG_T, C_ORNG_T);
                rect(px1 - 50, y + 26, px1 - 20, y + 28, C_ORNG_T, C_ORNG_T);
            }
        }
        text(px0 + 80, y + 6, 40, it->label, on ? C_ORNG_T : C_OLIVE_T, on ? C_ORNG_B : C_OLIVE_B, 0);
        if (s_capture && on) {
            v = s_capture == 1 ? "Press a key...   (Esc cancels)" : "Press a button...";
            if ((now / 400) & 1) text(px0 + 700, y + 8, 36, v, C_GOLD_T, C_GOLD_B, 0);
        } else {
            v = item_value(it, vb, sizeof vb);
            if (it->type == IT_INFO) {
                text(px0 + 520, y + 10, 30, v, C_PALE, C_PALE, 0);
            } else if (it->type == IT_ACTION) {
                if (on) text(px0 + 700, y + 8, 36, "Press A / Enter", C_PALE, C_PALE, 0);
            } else {
                if (on && it->type == IT_CHOICE) {
                    float w = text_w(v, 38);
                    text(px0 + 680, y + 6, 38, "<", C_GOLD_T, C_GOLD_B, 0);
                    text(px0 + 720 + w + 14, y + 6, 38, ">", C_GOLD_T, C_GOLD_B, 0);
                }
                text(px0 + 720, y + 6, 38, v, on ? C_GOLD_T : C_OLIVE_T, on ? C_GOLD_B : C_OLIVE_B, 0);
                if (it->restart) text(px1 - 80, y + 14, 26, "(restart)", C_OLIVE_T, C_OLIVE_B, 2);
            }
        }
        y += 56;
    }
    if (first + vis < n) text(px1 - 70, 300 + vis * 56 - 18, 34, "v", C_OLIVE_T, C_OLIVE_B, 1);

    /* Help line for the selected row, a note, and the controls. */
    if (s_art_ok) art_divider(px0 + 40, px1 - 40, 812 - 70, RGBA_(200, 180, 140, 200));
    else rect(px0 + 40, 812 - 70, px1 - 40, 812 - 68, RGBA_(120, 96, 52, 160), RGBA_(120, 96, 52, 160));
    if (s_note[0] && now - s_note_t < 4000)
        text(px0 + 60, 812 - 60, 30, s_note, C_GOLD_T, C_GOLD_B, 0);
    else if (sel && sel->help)
        text(px0 + 60, 812 - 60, 30, sel->help, C_PALE, C_PALE, 0);
    if (s_art_ok) {
        /* the game's own button pictures in front of each control */
        static const struct { const Piece *b; const char *t; } c[4] = {
            { &ART_BTN_A, "/ Enter  Change" }, { &ART_BTN_B, "/ Esc  Back to the game" },
            { NULL, "LB RB / Q E  Page" }, { &ART_BTN_Y, "/ Del  Default" } };
        float x = px0 + 60, gh = 34;
        int k;
        for (k = 0; k < 4; k++) {
            if (c[k].b) {
                float gw = c[k].b->w * gh / c[k].b->h;
                art(c[k].b, x, 812 - 1, x + gw, 812 - 1 + gh, RGBA_(255, 255, 255, 255), 0);
                x += gw + 8;
            }
            text(x, 812, 28, c[k].t, C_OLIVE_T, C_OLIVE_B, 0);
            x += text_w(c[k].t, 28) + 44;
        }
    } else {
        text(px0 + 60, 812, 28, "A / Enter  Change      B / Esc  Back to the game      LB RB / Q E  Page      Y / Del  Default",
             C_OLIVE_T, C_OLIVE_B, 0);
    }
}

void pc_menu_render(void *devp, void *ctxp, void *rtvp, UINT W, UINT H, float fps)
{
    ID3D11Device *dev = (ID3D11Device *)devp;
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)ctxp;
    ID3D11RenderTargetView *rtv = (ID3D11RenderTargetView *)rtvp;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    UINT stride = sizeof(MV), off = 0;
    float inv[4];
    ID3D11ShaderResourceView *srv;

    if (!s_open && !g_pc.show_fps)
        return;
    if (!s_gfx_ok) {
        if (s_gfx_failed) return;
        if (!gfx_init(dev)) { s_gfx_failed = 1; fprintf(stderr, "[MENU] drawing set-up failed\n"); return; }
        s_gfx_ok = 1;
    }
    if (s_open) {
        s_preset = preset_match();      /* the Quality Preset row follows the five settings */
        pad_poll();
    }
    s_nv = 0;
    s_ln[0] = s_ln[1] = s_ln[2] = 0;
    s_layer = 0;
    if (g_pc.show_fps) {
        extern float g_oogie_worst_ms;   /* d3d11_backend.c: slowest frame, last 2 s */
        char b[48];
        s_ox = 0; s_oy = 0; s_sc = 1;
        if (g_oogie_worst_ms > 20.0f)
            snprintf(b, sizeof b, "%.0f fps  slowest %.0f ms", fps, g_oogie_worst_ms);
        else
            snprintf(b, sizeof b, "%.0f fps", fps);
        {
            float px = H > 1000 ? 40.0f : 28.0f;
            text(14, 8, px, b, C_GOLD_T, C_GOLD_B, 0);
        }
    }
    if (s_open)
        draw_menu(W, H);
    {
        int l;
        for (l = 0; l < 3; l++) {
            memcpy(s_v + s_nv, s_lv[l], (size_t)s_ln[l] * sizeof(MV));
            s_nv += s_ln[l];
        }
    }
    if (!s_nv)
        return;

    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    memcpy(m.pData, s_v, (size_t)s_nv * sizeof(MV));
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s_vb, 0);
    inv[0] = 1.0f / (float)W; inv[1] = 1.0f / (float)H; inv[2] = inv[3] = 0;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, inv, sizeof inv);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s_cb, 0);
    }
    vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)W; vp.Height = (float)H; vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, s_rs);
    ID3D11DeviceContext_OMSetBlendState(ctx, s_blend, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, s_ds, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, s_il);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &s_vb, &stride, &off);
    ID3D11DeviceContext_VSSetShader(ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShader(ctx, s_ps, NULL, 0);
    srv = s_atlas_srv;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &s_smp);
    if (s_ln[0]) ID3D11DeviceContext_Draw(ctx, (UINT)s_ln[0], 0);
    if (s_ln[1] && s_art_ok) {                       /* the game's art */
        srv = s_art_srv;
        ID3D11DeviceContext_PSSetShader(ctx, s_ps_art, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
        ID3D11DeviceContext_Draw(ctx, (UINT)s_ln[1], (UINT)s_ln[0]);
        srv = s_atlas_srv;
        ID3D11DeviceContext_PSSetShader(ctx, s_ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    }
    if (s_ln[2]) ID3D11DeviceContext_Draw(ctx, (UINT)s_ln[2], (UINT)(s_ln[0] + s_ln[1]));
    srv = NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
}
