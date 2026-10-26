/* Project Oogie: the in-game settings menu ("RECOMP SETTINGS").
 *
 * Owner's request (2026-10-18): a settings menu like the BLACK recomp's,
 * themed as Oogie's Revenge, that controls every PC-port feature, so nobody
 * has to edit settings.ini by hand. Opened with F1, or Back+Start (or the
 * Guide / PS button) on a pad. The game is paused while it is open: the
 * back end keeps redrawing the last frame under it and does not return to
 * the GPU executor, so the game waits. Every change is saved at once. */
#ifndef PC_MENU_H
#define PC_MENU_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

int  pc_menu_is_open(void);
/* From any thread: open or close at the next frame. */
void pc_menu_request_toggle(void);
/* Applies a pending toggle; returns 1 when the menu just opened. Back end,
 * once per frame. */
int  pc_menu_update_state(void);
/* Keyboard from the game window. Returns 1 when the menu took the key (F1
 * always; every key while the menu is open). */
int  pc_menu_key(int vk, int down);
/* Any menu key still physically held (so it does not reach the game). */
int  pc_menu_keys_held(void);
/* The window lost focus: no key counts as held (their key-ups go elsewhere). */
void pc_menu_keys_reset(void);

/* Draw the menu (when open) and the frame-rate counter (when enabled) over
 * the finished picture. dev/ctx/rtv are ID3D11 pointers. */
void pc_menu_render(void *dev, void *ctx, void *rtv, UINT w, UINT h, float fps);

/* The game window, for focus checks. */
void pc_menu_set_window(HWND h);

/* 0..1 gain for the audio output (volume, mute in background, menu open). */
float oogie_audio_gain(void);

/* Implemented by the back end (d3d11_backend.c). */
void oogie_keymap_reload(void);

#ifdef __cplusplus
}
#endif
#endif
