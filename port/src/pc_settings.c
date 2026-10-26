/* Project Oogie: settings.ini (see pc_settings.h). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc_settings.h"

#define MAX_ENTRIES 256

typedef struct { char sec[32]; char key[48]; char val[160]; } Entry;

static Entry s_ent[MAX_ENTRIES];
static int s_n;
static int s_loaded;
static char s_path[MAX_PATH];

/* Written when settings.ini is missing. Every value here is the built-in
 * default, so deleting the file (or a line) never changes behaviour. */
static const char s_default_ini[] =
"; Project Oogie settings. Press F1 in the game (or Back+Start on a pad) to\n"
"; change them in the settings menu; this file is where they are kept.\n"
"; It can also be edited with Notepad while the game is closed. Delete it to\n"
"; get the defaults back (it is rewritten on the next start).\n"
"; A line starting with ; is a comment.\n"
"\n"
"[Display]\n"
"; Start in borderless fullscreen (1) or a window (0). F11 or Alt+Enter toggles.\n"
"fullscreen = 0\n"
"; Monitor for fullscreen: 0 = the one the window is on, 1, 2, ... = that monitor.\n"
"monitor = 0\n"
"; Window size as a multiple of the original 640x480 (1 to 8), or auto to fit\n"
"; the screen. The window can also be resized or maximised while playing.\n"
"window_scale = auto\n"
"; Picture shape: 4:3 (original), fill (widescreen: the 3D world is drawn for\n"
"; the shape of your screen and fills it edge to edge), or stretch.\n"
"aspect = 4:3\n"
"; Hide the black borders the game paints along the screen edges and the\n"
"; film bars in cutscenes (1), or keep them (0).\n"
"hide_borders = 1\n"
"; 4:3 screens (menus, films) in a wide window: 0 black sides, 1 blurred\n"
"; picture at the sides, 2 stretched.\n"
"side_fill = 1\n"
"; Widescreen: health and gauges in the screen corners (1) or in the 4:3 area (0).\n"
"hud_edges = 1\n"
"; Field of view in percent of the original camera (70 to 140).\n"
"field_of_view = 100\n"
"; Wait for the monitor's refresh before showing a frame (no tearing).\n"
"vsync = 1\n"
"; Frame-rate cap: 60 (the original speed) or 30. The game's logic, films and\n"
"; music run per frame, so other values are not offered.\n"
"fps_cap = 60\n"
"\n"
"[Graphics]\n"
"; Internal resolution: 0 = Auto (the largest the window holds, lowered if the\n"
"; game drops below 60 fps), -1 = Native (your screen's height), a height such\n"
"; as 720, 1080 or 2160, or 1 to 4 = that multiple of the game's 640x480.\n"
"render_scale = 0\n"
"; Anisotropic texture filtering: 1 (off), 2, 4, 8 or 16.\n"
"anisotropic = 1\n"
"; Anti-aliasing (smooths jagged edges): 0 off, 1 low, 2 high.\n"
"anti_aliasing = 2\n"
"; Sharpening: 0 off, 1 low, 2 medium, 3 high.\n"
"sharpening = 1\n"
"; Mipmaps (smoother distant textures, less shimmer): 1 on, 0 off (original,\n"
"; least video memory).\n"
"mipmaps = 1\n"
"; Accessibility: colour-blind filter, 0 off, 1 protanopia (red-weak),\n"
"; 2 deuteranopia (green-weak), 3 tritanopia (blue-weak).\n"
"colour_filter = 0\n"
"; Show the frame rate in the top-left corner.\n"
"show_fps = 0\n"
"\n"
"[Audio]\n"
"; Master volume, 0 to 100.\n"
"volume = 100\n"
"; Silence the game while its window is in the background.\n"
"mute_in_background = 0\n"
"\n"
"[Controller]\n"
"; Xbox 360, Xbox One and Xbox Series pads (wired or Bluetooth) always work.\n"
"; PlayStation pads (DualShock 4, DualSense), Switch Pro and most other pads\n"
"; are read through SDL2.dll beside the game. 0 turns that off.\n"
"other_pads = 1\n"
"; Light bar colour on PlayStation pads, as RRGGBB (blank = leave it alone).\n"
"lightbar = 30A040\n"
"; Rumble (vibration): 0 off, 1 normal, 2 strong.\n"
"rumble = 1\n"
"; Accessibility: a held face button presses itself about 10 times a second\n"
"; (0 off, 1 A, 2 B, 3 X, 4 Y).\n"
"auto_repeat = 0\n"
"\n"
"[ControllerButtons]\n"
"; Which button on your pad presses each original Xbox button.\n"
"; Pad buttons use Xbox names: A B X Y LB RB LT RT Back Start LS RS\n"
"; DUp DDown DLeft DRight, plus Touchpad and Misc (PS5 mic / Series share).\n"
"; On PlayStation pads: A = Cross, B = Circle, X = Square, Y = Triangle,\n"
"; LB/RB = L1/R1, LT/RT = L2/R2, Back = Share/Create, Start = Options.\n"
"; Several buttons may be listed, separated by commas; none = unbound.\n"
"A = A\n"
"B = B\n"
"X = X\n"
"Y = Y\n"
"Black = LB\n"
"White = RB\n"
"LeftTrigger = LT\n"
"RightTrigger = RT\n"
"Start = Start\n"
"Back = Back, Touchpad\n"
"LeftStickClick = LS\n"
"RightStickClick = RS\n"
"DpadUp = DUp\n"
"DpadDown = DDown\n"
"DpadLeft = DLeft\n"
"DpadRight = DRight\n"
"\n"
"[Keyboard]\n"
"; Which keys press each original Xbox button (the game window must have focus).\n"
"; Key names: A to Z, 0 to 9, Up Down Left Right, Enter Backspace Space Tab\n"
"; Shift Ctrl Alt Escape Insert Delete Home End PageUp PageDown, F1 to F12,\n"
"; Numpad0 to Numpad9. Several keys may be listed, separated by commas.\n"
"; The arrow keys are A B X Y in the pad's diamond (Down A, Right B, Left X, Up Y).\n"
"A = Down, Z\n"
"B = Right, X\n"
"X = Left, C\n"
"Y = Up, V\n"
"Black = E\n"
"White = Q\n"
"LeftTrigger = 1\n"
"RightTrigger = 3\n"
"Start = Enter\n"
"Back = Backspace\n"
"LeftStickClick = Shift\n"
"RightStickClick = Ctrl\n"
"DpadUp = T\n"
"DpadDown = G\n"
"DpadLeft = F\n"
"DpadRight = H\n"
"LeftStickUp = W, Numpad8\n"
"LeftStickDown = S, Numpad2\n"
"LeftStickLeft = A, Numpad4\n"
"LeftStickRight = D, Numpad6\n"
"RightStickUp = I\n"
"RightStickDown = K\n"
"RightStickLeft = J\n"
"RightStickRight = L\n";

static void trim(char *s)
{
    char *p = s, *e;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
}

static void parse(FILE *f)
{
    char line[256], sec[32] = "";
    while (fgets(line, sizeof line, f)) {
        char *eq, *c;
        trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#')
            continue;
        if (line[0] == '[') {
            c = strchr(line, ']');
            if (c) *c = 0;
            snprintf(sec, sizeof sec, "%s", line + 1);
            trim(sec);
            continue;
        }
        eq = strchr(line, '=');
        if (!eq || s_n >= MAX_ENTRIES)
            continue;
        *eq = 0;
        c = strchr(eq + 1, ';');               /* trailing comment */
        if (c) *c = 0;
        snprintf(s_ent[s_n].sec, sizeof s_ent[s_n].sec, "%s", sec);
        snprintf(s_ent[s_n].key, sizeof s_ent[s_n].key, "%s", line);
        snprintf(s_ent[s_n].val, sizeof s_ent[s_n].val, "%s", eq + 1);
        trim(s_ent[s_n].key);
        trim(s_ent[s_n].val);
        s_n++;
    }
}

const char *pc_settings_get(const char *section, const char *key)
{
    int i;
    pc_settings_load();
    /* The last line wins, as an editor would expect after appending. */
    for (i = s_n - 1; i >= 0; i--)
        if (!_stricmp(s_ent[i].sec, section) && !_stricmp(s_ent[i].key, key))
            return s_ent[i].val;
    return NULL;
}

int pc_settings_int(const char *section, const char *key, int def)
{
    const char *v = pc_settings_get(section, key);
    char *end;
    long l;
    if (!v || !*v) return def;
    l = strtol(v, &end, 0);
    return end == v ? def : (int)l;
}

const char *pc_settings_path(void)
{
    return s_path;
}

/* Options that the back end already reads as environment switches. */
static void to_env(const char *section, const char *key, const char *env)
{
    const char *v = pc_settings_get(section, key);
    const char *have = getenv(env);
    if (v && *v && !(have && *have))
        _putenv_s(env, v);
}

void pc_settings_load(void)
{
    const char *e;
    FILE *f;

    if (s_loaded)
        return;
    s_loaded = 1;

    e = getenv("OOGIE_SETTINGS");
    if (e && *e) {
        snprintf(s_path, sizeof s_path, "%s", e);
    } else {
        /* Beside the game: the working directory is the folder with game\
         * and play.bat. Else beside the exe. */
        snprintf(s_path, sizeof s_path, "settings.ini");
        if (GetFileAttributesA(s_path) == INVALID_FILE_ATTRIBUTES
            && GetFileAttributesA("game") == INVALID_FILE_ATTRIBUTES) {
            DWORD n = GetModuleFileNameA(NULL, s_path, sizeof s_path);
            char *slash = n ? strrchr(s_path, '\\') : NULL;
            if (slash) snprintf(slash + 1, sizeof s_path - (slash + 1 - s_path), "settings.ini");
            else snprintf(s_path, sizeof s_path, "settings.ini");
        }
    }

    f = fopen(s_path, "r");
    if (!f) {
        f = fopen(s_path, "w");
        if (f) {
            fputs(s_default_ini, f);
            fclose(f);
            fprintf(stderr, "[SETTINGS] wrote defaults to %s\n", s_path);
        }
        f = fopen(s_path, "r");
    }
    if (f) {
        parse(f);
        fclose(f);
        fprintf(stderr, "[SETTINGS] %s: %d values\n", s_path, s_n);
    } else {
        /* Unwritable folder: the built-in defaults, from the same text. */
        char tmp[MAX_PATH], name[MAX_PATH];
        GetTempPathA(sizeof tmp, tmp);
        GetTempFileNameA(tmp, "oog", 0, name);
        f = fopen(name, "w+");
        if (f) { fputs(s_default_ini, f); rewind(f); parse(f); fclose(f); }
        DeleteFileA(name);
        fprintf(stderr, "[SETTINGS] %s unreadable, using defaults\n", s_path);
    }

    to_env("Display", "window_scale", "OOGIE_WINDOW_SCALE");
    to_env("Display", "fps_cap", "OOGIE_FPS_CAP");
    pc_config_load();
}

/* Change one value and write settings.ini back, keeping every comment and
 * the order of the lines: the value of an existing "key = value" line in its
 * section is replaced, a missing key is added at the end of its section, and
 * a missing section at the end of the file. Used by the settings menu. */
int pc_settings_set(const char *section, const char *key, const char *value)
{
    char *buf = NULL, *out = NULL, line[512], cur[64] = "";
    size_t cap = 0, n = 0, on = 0, ocap;
    FILE *f;
    int i, done = 0, in_sec = 0;
    long last_in_sec = -1;           /* offset in out after the section's last line */

    pc_settings_load();
    /* In memory first. */
    for (i = s_n - 1; i >= 0; i--)
        if (!_stricmp(s_ent[i].sec, section) && !_stricmp(s_ent[i].key, key)) break;
    if (i < 0 && s_n < MAX_ENTRIES) {
        i = s_n++;
        snprintf(s_ent[i].sec, sizeof s_ent[i].sec, "%s", section);
        snprintf(s_ent[i].key, sizeof s_ent[i].key, "%s", key);
    }
    if (i >= 0)
        snprintf(s_ent[i].val, sizeof s_ent[i].val, "%s", value);

    f = fopen(s_path, "rb");
    if (f) {
        for (;;) {
            if (n + 4096 > cap) { cap = cap ? cap * 2 : 16384; buf = (char *)realloc(buf, cap); if (!buf) { fclose(f); return 0; } }
            size_t r = fread(buf + n, 1, cap - n - 1, f);
            if (!r) break;
            n += r;
        }
        fclose(f);
    }
    if (!buf) { buf = (char *)malloc(1); if (!buf) return 0; }
    buf[n] = 0;
    ocap = n + 1024;
    out = (char *)malloc(ocap);
    if (!out) { free(buf); return 0; }
    {
        char *p = buf;
        while (*p) {
            char *e = strchr(p, '\n');
            size_t len = e ? (size_t)(e - p + 1) : strlen(p);
            char t[512], *eq;
            if (len >= sizeof line) len = sizeof line - 1;
            memcpy(line, p, len); line[len] = 0;
            p += e ? (size_t)(e - p + 1) : strlen(p);
            snprintf(t, sizeof t, "%s", line); trim(t);
            if (t[0] == '[') {
                if (in_sec && !done && last_in_sec >= 0) {
                    /* Section ends without the key: insert it there. */
                    char add[256]; size_t al;
                    snprintf(add, sizeof add, "%s = %s\r\n", key, value);
                    al = strlen(add);
                    if (on + al + 1 >= ocap) { ocap = (on + al) * 2; out = (char *)realloc(out, ocap); }
                    memmove(out + last_in_sec + al, out + last_in_sec, on - (size_t)last_in_sec);
                    memcpy(out + last_in_sec, add, al);
                    on += al;
                    done = 1;
                }
                eq = strchr(t, ']'); if (eq) *eq = 0;
                snprintf(cur, sizeof cur, "%s", t + 1); trim(cur);
                in_sec = !_stricmp(cur, section);
            } else if (in_sec && !done && t[0] && t[0] != ';' && t[0] != '#' && (eq = strchr(t, '='))) {
                *eq = 0; trim(t);
                if (!_stricmp(t, key)) {
                    snprintf(line, sizeof line, "%s = %s\r\n", key, value);
                    len = strlen(line);
                    done = 1;
                }
            }
            if (on + len + 1 >= ocap) { ocap = (on + len) * 2 + 256; out = (char *)realloc(out, ocap); }
            memcpy(out + on, line, len); on += len;
            if (in_sec && t[0] && t[0] != ';' && t[0] != '[') last_in_sec = (long)on;
            else if (in_sec && t[0] == '[') last_in_sec = (long)on;
        }
    }
    if (!done) {
        char add[320];
        if (in_sec && last_in_sec >= 0 && last_in_sec == (long)on) {
            snprintf(add, sizeof add, "%s%s = %s\r\n", (on && out[on - 1] != '\n') ? "\r\n" : "", key, value);
        } else {
            snprintf(add, sizeof add, "%s\r\n[%s]\r\n%s = %s\r\n", (on && out[on - 1] != '\n') ? "\r\n" : "", section, key, value);
            if (in_sec && last_in_sec >= 0) {   /* the section is open but trailing comments followed */
                char a2[256]; size_t al;
                snprintf(a2, sizeof a2, "%s = %s\r\n", key, value);
                al = strlen(a2);
                if (on + al + 1 >= ocap) { ocap = (on + al) * 2; out = (char *)realloc(out, ocap); }
                memmove(out + last_in_sec + al, out + last_in_sec, on - (size_t)last_in_sec);
                memcpy(out + last_in_sec, a2, al);
                on += al; add[0] = 0;
            }
        }
        if (add[0]) {
            size_t al = strlen(add);
            if (on + al + 1 >= ocap) { ocap = (on + al) * 2; out = (char *)realloc(out, ocap); }
            memcpy(out + on, add, al); on += al;
        }
    }
    f = fopen(s_path, "wb");
    if (f) { fwrite(out, 1, on, f); fclose(f); }
    free(buf); free(out);
    return f != NULL;
}

/* ---- live configuration (g_pc) ------------------------------------------ */
PcConfig g_pc = { 0, 0, 0, 0, 100, 1, 1, 1, 60, 1, 1, 0, 1, 1, 100, 0, 0, 0 };

static int str_choice(const char *v, const char *const *names, int n, int def)
{
    int i;
    if (!v || !*v) return def;
    for (i = 0; i < n; i++) if (!_stricmp(v, names[i])) return i;
    return def;
}

void pc_config_load(void)
{
    static const char *const aspects[] = { "4:3", "fill", "stretch", "16:9" };
    const char *v;
    pc_settings_load();
    g_pc.fullscreen = pc_settings_int("Display", "fullscreen", 0) != 0;
    g_pc.monitor = pc_settings_int("Display", "monitor", 0);
    v = pc_settings_get("Display", "window_scale");
    g_pc.window_scale = (!v || !_stricmp(v, "auto")) ? 0 : pc_settings_int("Display", "window_scale", 0);
    if (g_pc.window_scale < 0 || g_pc.window_scale > 8) g_pc.window_scale = 0;
    v = pc_settings_get("Display", "aspect");
    if (v) { g_pc.aspect = str_choice(v, aspects, 4, 0); if (g_pc.aspect == 3) g_pc.aspect = 1; }
    else g_pc.aspect = pc_settings_int("Display", "keep_aspect", 1) ? 0 : 2;
    g_pc.hide_borders = pc_settings_int("Display", "hide_borders", 1) != 0;
    g_pc.side_fill = pc_settings_int("Display", "side_fill", 1);
    if (g_pc.side_fill < 0 || g_pc.side_fill > 2) g_pc.side_fill = 1;
    g_pc.fov = pc_settings_int("Display", "field_of_view", 100);
    if (g_pc.fov < 70 || g_pc.fov > 140) g_pc.fov = 100;
    g_pc.vsync = pc_settings_int("Display", "vsync", 1) != 0;
    v = getenv("OOGIE_FPS_CAP");                       /* a test's override wins */
    g_pc.fps_cap = (v && *v) ? atoi(v) : pc_settings_int("Display", "fps_cap", 60);
    /* The game's logic, films and music run per frame: above 60 they run
     * fast (owner 2026-10-18), so until a real high-frame-rate mode exists
     * the cap is 30 or 60. A test's OOGIE_FPS_CAP still wins. */
    if (!(v && *v) && g_pc.fps_cap != 30) g_pc.fps_cap = 60;
    if (g_pc.fps_cap < 0) g_pc.fps_cap = 0;
    g_pc.render_scale = pc_settings_int("Graphics", "render_scale", 0);
    if (g_pc.render_scale < -1 || (g_pc.render_scale > 4 && (g_pc.render_scale < 240 || g_pc.render_scale > 4320)))
        g_pc.render_scale = 0;
    g_pc.aniso = pc_settings_int("Graphics", "anisotropic", 1);
    if (g_pc.aniso != 2 && g_pc.aniso != 4 && g_pc.aniso != 8 && g_pc.aniso != 16) g_pc.aniso = 1;
    g_pc.aa = pc_settings_int("Graphics", "anti_aliasing", 2);
    if (g_pc.aa < 0 || g_pc.aa > 2) g_pc.aa = 2;
    g_pc.sharpen = pc_settings_int("Graphics", "sharpening", 1);
    if (g_pc.sharpen < 0 || g_pc.sharpen > 3) g_pc.sharpen = 1;
    g_pc.mipmaps = pc_settings_int("Graphics", "mipmaps", 1) != 0;
    g_pc.hud_edges = pc_settings_int("Display", "hud_edges", 1) != 0;
    g_pc.tex_dump = pc_settings_int("Mods", "save_textures", 0) != 0;
    g_pc.audio_buffer = pc_settings_int("Audio", "buffer", 1) != 0;
    g_pc.pause_bg = pc_settings_int("Audio", "pause_in_background", 0) != 0;
    g_pc.skip_intro = pc_settings_int("Display", "skip_intro", 0) != 0;
    if (!getenv("RECOMP_AUDIO_BUF_COUNT"))
        /* XAudio2 queue: 4 buffers of 512 samples (43 ms) ran dry when a few
         * frames in a row took 30-40 ms (owner 2026-10-23, menus while a save
         * loads: 27 underruns in 3 min); 6 hold 64 ms. 6 still ran dry 15
         * times in a session during films and save reads (owner 2026-10-24,
         * play.log); 8 (the most the queue takes) hold 85 ms. */
        _putenv(g_pc.audio_buffer ? "RECOMP_AUDIO_BUF_COUNT=8" : "RECOMP_AUDIO_BUF_COUNT=4");
    g_pc.show_fps = pc_settings_int("Graphics", "show_fps", 0) != 0;
    g_pc.rumble = pc_settings_int("Controller", "rumble", 1);
    if (g_pc.rumble < 0 || g_pc.rumble > 2) g_pc.rumble = 1;
    g_pc.auto_repeat = pc_settings_int("Controller", "auto_repeat", 0);
    if (g_pc.auto_repeat < 0 || g_pc.auto_repeat > 4) g_pc.auto_repeat = 0;
    g_pc.colour_filter = pc_settings_int("Graphics", "colour_filter", 0);
    if (g_pc.colour_filter < 0 || g_pc.colour_filter > 3) g_pc.colour_filter = 0;
    g_pc.other_pads = pc_settings_int("Controller", "other_pads", 1) != 0;
    g_pc.volume = pc_settings_int("Audio", "volume", 100);
    if (g_pc.volume < 0 || g_pc.volume > 100) g_pc.volume = 100;
    g_pc.mute_bg = pc_settings_int("Audio", "mute_in_background", 0) != 0;
}

void pc_config_save_int(const char *section, const char *key, int value)
{
    char b[32];
    snprintf(b, sizeof b, "%d", value);
    pc_settings_set(section, key, b);
    InterlockedIncrement(&g_pc.gen);
}
