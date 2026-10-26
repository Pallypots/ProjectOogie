/* Project Oogie: settings.ini, the PC port's own settings file.
 *
 * Owner's decision (2026-10-18): no launcher, and the game's original
 * Options menu is left alone. The port's settings live in settings.ini
 * beside the game (the working directory, else beside the exe). It is
 * written with every option and a comment when missing. A later in-game
 * overlay menu will edit the same file.
 *
 * An environment variable that is already set wins over the file, so test
 * scripts keep full control. */
#ifndef PC_SETTINGS_H
#define PC_SETTINGS_H

/* Reads settings.ini once (later calls do nothing) and copies the options
 * that are plain environment switches into the environment. */
void pc_settings_load(void);

/* The value of key in [section], or NULL. Names are case-insensitive. */
const char *pc_settings_get(const char *section, const char *key);
int pc_settings_int(const char *section, const char *key, int def);

/* Full path of the file that was read (or would be written). */
const char *pc_settings_path(void);

/* Change one value in memory and in settings.ini (comments and order kept). */
int pc_settings_set(const char *section, const char *key, const char *value);

/* The PC options as the running game uses them. Read from settings.ini at
 * start; the settings menu changes them and saves; the back end and input
 * code read them every frame and apply what changed. */
typedef struct {
    int fullscreen;      /* 0 window, 1 borderless fullscreen */
    int monitor;         /* 0 the window's monitor, n: the n-th monitor */
    int window_scale;    /* 0 auto, 1..8 x 640x480 */
    int aspect;          /* 0 4:3, 1 16:9 widescreen, 2 stretch to window */
    int fov;             /* field of view, percent of the original (100) */
    int hide_borders;    /* hide the game's own black edge masks / film bars */
    int side_fill;       /* 4:3 screens in a wide window: 0 black, 1 blur, 2 stretch */
    int vsync;           /* 0 off, 1 on */
    int fps_cap;         /* 0 none, else frames per second */
    int render_scale;    /* 0 auto, -1 native, 1..4 x 640x480, or a height 240..4320 (d3d11_backend.c) */
    int aniso;           /* 1 off, 2,4,8,16 */
    int aa;              /* anti-aliasing: 0 off, 1 low, 2 high (d3d11_backend.c post_process) */
    int sharpen;         /* 0 off, 1 low, 2 medium, 3 high */
    int mipmaps;         /* mip chains for textures the game mip-filters (d3d11_backend.c tex_upload) */
    int hud_edges;       /* widescreen: corner HUD pieces at the screen edges */
    int tex_dump;        /* Mods > Save Game Textures: textures to texture_dump\ */
    int audio_buffer;    /* 0 small (lowest delay), 1 large (no crackles); at start */
    int show_fps;        /* frame-rate counter in the corner */
    int rumble;          /* 0..2: off, normal, strong */
    int auto_repeat;     /* accessibility: 0 off, 1..4 = A, B, X, Y presses itself while held (pc_input.c) */
    int colour_filter;   /* accessibility: 0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia (d3d11_backend.c post_process) */
    int other_pads;      /* SDL2 pads (restart) */
    int volume;          /* 0..100 master volume */
    int mute_bg;         /* mute while the window is in the background */
    int pause_bg;        /* switching away opens the settings menu (pauses) */
    int skip_intro;      /* press Start through the company logos film */
    volatile long gen;   /* bumped on every change */
} PcConfig;

extern PcConfig g_pc;

/* Fill g_pc from settings.ini (called by pc_settings_load). */
void pc_config_load(void);
/* Store one g_pc field back to settings.ini by its menu key. */
void pc_config_save_int(const char *section, const char *key, int value);
#endif
