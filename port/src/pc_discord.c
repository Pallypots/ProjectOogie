/* Project Oogie: Discord Rich Presence (GitHub request, 2026-10-31).
 *
 * Shows "Playing Oogie's Revenge" with the chapter on the player's Discord
 * profile. Talks to the Discord desktop app over its local pipe
 * (\\.\pipe\discord-ipc-0..9): a frame is opcode (u32), length (u32), then
 * JSON. Opcode 0 is the handshake, opcode 1 a command (SET_ACTIVITY).
 * Written from Discord's public RPC documentation; no Discord SDK or library.
 *
 * Nothing happens when Discord is not running: the thread tries again every
 * 15 s. Under Proton the pipe exists only with a bridge (for example
 * wine-discord-ipc-bridge); without one this stays idle.
 *
 * settings.ini [Discord] rich_presence = 1 / 0 (also F1 > Display),
 * app_id = the Discord application to show as (Discord developer portal). */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pc_settings.h"
#include "pc_version.h"

extern ptrdiff_t g_xbox_mem_offset;
#define DC_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* The Project Oogie application on Discord's developer portal; its name is
 * what Discord shows after "Playing". settings.ini app_id overrides it. */
#define OOGIE_DISCORD_APP_ID "1566296443105321020"

/* [0x413164]: current chapter 1..24 (docs/RE_CHAPTER_SELECT.md, Verified) */
#define DC_CHAPTER_VA 0x00413164u

static const char *const s_chapters[25] = {
    NULL,
    "Jack's Return", "The Witching Hour", "The Hanging Tree", "Grave Danger",
    "The Crypt Creeper", "The Doctor's Hunch", "The Lightning Round", "Changing Brains",
    "Pumpkin Patch Possession", "Barrel's Maze", "Peril in the Pumpkin Head", "Runaway Clown",
    "The Vampire Brothers", "The Mayor's Madhouse", "Rooftop Rumble", "The Grizzly Gauntlet",
    "Casino Clash", "Spiral Hill", "The Hinterlands, Part 1", "Fire and Ice Frenzy",
    "The Hinterlands, Part 2", "Christmas Town", "Saving Sandy", "Taking out the Trash",
};

/* [0x413164] holds 1 from the title screen on, so the chapter is shown only
 * after a level file (stm_data\NNNN.lz, 4 hex digits) opened; the title film
 * (mv_title.bik) means the menus again. Called from the runtime's path hook
 * (pc_input_path_watch). */
static volatile LONG s_in_level;
void pc_discord_path(const char *xbox_path)
{
    const char *s = strstr(xbox_path, "stm_data\\");
    if (s) {
        int k;
        s += 9;
        for (k = 0; k < 4; k++)
            if (!((s[k] >= '0' && s[k] <= '9') || (s[k] >= 'a' && s[k] <= 'f') || (s[k] >= 'A' && s[k] <= 'F')))
                break;
        if (k == 4 && (s[4] == '.') && (s[5] == 'l' || s[5] == 'L') && (s[6] == 'z' || s[6] == 'Z') && !s[7])
            InterlockedExchange(&s_in_level, 1);
    }
    if (strstr(xbox_path, "mv_title.bik") || strstr(xbox_path, "MV_TITLE.BIK"))
        InterlockedExchange(&s_in_level, 0);
}

static HANDLE s_pipe = INVALID_HANDLE_VALUE;
static unsigned s_nonce;

static void dc_close(void)
{
    if (s_pipe != INVALID_HANDLE_VALUE) CloseHandle(s_pipe);
    s_pipe = INVALID_HANDLE_VALUE;
}

static int dc_send(uint32_t op, const char *json)
{
    uint32_t len = (uint32_t)strlen(json);
    char buf[2048];
    DWORD wr;
    if (len + 8 > sizeof buf) return 0;
    memcpy(buf, &op, 4);
    memcpy(buf + 4, &len, 4);
    memcpy(buf + 8, json, len);
    if (!WriteFile(s_pipe, buf, len + 8, &wr, NULL) || wr != len + 8) return 0;
    return 1;
}

/* Reads and drops whatever Discord sent back (replies, the READY event);
 * 0 when the pipe broke or Discord closed it (opcode 2). */
static int dc_drain(int wait_ms)
{
    DWORD avail = 0, rd;
    char buf[4096];
    DWORD t0 = GetTickCount();
    for (;;) {
        if (!PeekNamedPipe(s_pipe, NULL, 0, NULL, &avail, NULL)) return 0;
        if (avail >= 8) {
            uint32_t hdr[2];
            if (!ReadFile(s_pipe, hdr, 8, &rd, NULL) || rd != 8) return 0;
            while (hdr[1] > 0) {
                DWORD n = hdr[1] > sizeof buf ? (DWORD)sizeof buf : hdr[1];
                if (!ReadFile(s_pipe, buf, n, &rd, NULL) || !rd) return 0;
                hdr[1] -= rd;
            }
            if (hdr[0] == 2) {
                fprintf(stderr, "[DISCORD] Discord closed the connection (check app_id)\n");
                return 0;
            }
            continue;
        }
        if ((int)(GetTickCount() - t0) >= wait_ms) return 1;
        Sleep(20);
    }
}

static int dc_connect(const char *app_id)
{
    int i;
    char hs[160];
    for (i = 0; i < 10; i++) {
        char name[64];
        snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
        s_pipe = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (s_pipe != INVALID_HANDLE_VALUE) break;
    }
    if (s_pipe == INVALID_HANDLE_VALUE) return 0;
    snprintf(hs, sizeof hs, "{\"v\":1,\"client_id\":\"%s\"}", app_id);
    if (!dc_send(0, hs) || !dc_drain(500)) {
        dc_close();
        return 0;
    }
    fprintf(stderr, "[DISCORD] connected to Discord (pipe %d)\n", i);
    fflush(stderr);
    return 1;
}

/* JSON string body: quotes and backslashes escaped (chapter names are ASCII) */
static void dc_esc(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 2 < cap; s++) {
        if (*s == '"' || *s == '\\') out[n++] = '\\';
        out[n++] = *s;
    }
    out[n] = 0;
}

static int dc_set_activity(int chapter, long long start)
{
    char details[96], state[96], ed[160], es[160], json[1400];
    if (chapter >= 1 && chapter <= 24) {
        snprintf(details, sizeof details, "Chapter %d: %s", chapter, s_chapters[chapter]);
        snprintf(state, sizeof state, "Playing as Jack Skellington");
    } else {
        snprintf(details, sizeof details, "In the menus");
        snprintf(state, sizeof state, "Halloween Town awaits");
    }
    dc_esc(ed, sizeof ed, details);
    dc_esc(es, sizeof es, state);
    snprintf(json, sizeof json,
             "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{"
             "\"details\":\"%s\",\"state\":\"%s\",\"timestamps\":{\"start\":%lld},"
             "\"assets\":{\"large_image\":\"logo\",\"large_text\":\"Project Oogie " OOGIE_VERSION "\"}}},"
             "\"nonce\":\"%u\"}",
             (unsigned long)GetCurrentProcessId(), ed, es, start, ++s_nonce);
    return dc_send(1, json) && dc_drain(200);
}

static int dc_clear(void)
{
    char json[160];
    snprintf(json, sizeof json,
             "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu},\"nonce\":\"%u\"}",
             (unsigned long)GetCurrentProcessId(), ++s_nonce);
    return dc_send(1, json) && dc_drain(200);
}

static DWORD WINAPI dc_thread(LPVOID arg)
{
    const char *app_id = (const char *)arg;
    long long start = (long long)time(NULL);
    int shown = -2, was_on = 1;
    ULONGLONG next_try = 0;
    (void)arg;
    for (;;) {
        int on = g_pc.discord;
        if (!on) {
            if (was_on && s_pipe != INVALID_HANDLE_VALUE) dc_clear();
            was_on = 0;
            shown = -2;
            Sleep(1000);
            continue;
        }
        was_on = 1;
        if (s_pipe == INVALID_HANDLE_VALUE) {
            if (GetTickCount64() < next_try) { Sleep(1000); continue; }
            if (!dc_connect(app_id)) {
                next_try = GetTickCount64() + 15000;
                Sleep(1000);
                continue;
            }
            shown = -2;
        }
        {
            uint32_t ch = DC_MEM32(DC_CHAPTER_VA);
            int c = s_in_level && ch >= 1 && ch <= 24 ? (int)ch : 0;
            if (c != shown) {
                if (!dc_set_activity(c, start)) { dc_close(); continue; }
                shown = c;
                if (c) fprintf(stderr, "[DISCORD] status: Chapter %d: %s\n", c, s_chapters[c]);
                else fprintf(stderr, "[DISCORD] status: in the menus\n");
                fflush(stderr);
            } else if (!dc_drain(0)) {
                dc_close();
                continue;
            }
        }
        Sleep(2000);
    }
    return 0;
}

void pc_discord_start(void)
{
    static char app_id[32];
    const char *id = getenv("OOGIE_DISCORD_APP_ID");   /* tests */
    const char *p;
    if (!id || !*id) id = pc_settings_get("Discord", "app_id");
    if (!id || !*id) id = OOGIE_DISCORD_APP_ID;
    for (p = id; *p; p++)
        if (*p < '0' || *p > '9') break;
    if (!*id || *p || strlen(id) >= sizeof app_id) {
        fprintf(stderr, "[DISCORD] no Discord application id set; Rich Presence off\n");
        return;
    }
    strcpy(app_id, id);
    fprintf(stderr, "[DISCORD] Rich Presence %s; it shows once the Discord app is running\n",
            g_pc.discord ? "on" : "off (F1 > Display > Discord Status)");
    CloseHandle(CreateThread(NULL, 0, dc_thread, app_id, 0, NULL));
}
