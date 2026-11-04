/* Project Oogie: install the PS2 texture packs from the archive the player
 * downloaded (0.2.7). The packs (RetroGenerica, with Solid Snack; see
 * docs/HD_ASSET_ENHANCEMENTS.md) are not shipped: they are the game's own
 * textures, redone. At start, an archive in Downloads, the game folder or
 * mods\ whose name says Oogie and "Texture" or "Button" is offered once;
 * Windows' own tar.exe (libarchive: .7z and .zip) unpacks it into mods\ on a
 * background thread, and the textures are matched again when it is done
 * (d3d11_backend.c finds packs anywhere under mods\). Without tar.exe (Wine,
 * Steam Deck) the player is told where to unpack it by hand. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "pc_settings.h"

extern void oogie_textures_rescan(void);   /* d3d11_backend.c */

typedef struct { char path[MAX_PATH], dest[MAX_PATH], label[64]; unsigned long long size; } PackJob;

static int has_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    for (; *s; s++) if (!_strnicmp(s, w, n)) return 1;
    return 0;
}

static int dds_count(const char *dir, int depth)
{
    WIN32_FIND_DATAA fd;
    char pat[MAX_PATH];
    HANDLE h;
    int n = 0;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == '.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char sub[MAX_PATH];
            if (depth < 5 && (size_t)snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName) < sizeof sub)
                n += dds_count(sub, depth + 1);
        } else {
            size_t L = strlen(fd.cFileName);
            if (L > 4 && !_stricmp(fd.cFileName + L - 4, ".dds")) n++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static DWORD WINAPI unpack_thread(LPVOID arg)
{
    PackJob *j = (PackJob *)arg;
    char cmd[3 * MAX_PATH], tar[MAX_PATH];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    GetSystemDirectoryA(tar, sizeof tar);
    strcat(tar, "\\tar.exe");
    CreateDirectoryA("mods", NULL);
    CreateDirectoryA(j->dest, NULL);
    snprintf(cmd, sizeof cmd, "\"%s\" -xf \"%s\" -C \"%s\"", tar, j->path, j->dest);
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    fprintf(stderr, "[TEXPACK] installing %s from %s into %s\n", j->label, j->path, j->dest);
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    {
        int n = dds_count(j->dest, 0);
        fprintf(stderr, "[TEXPACK] %s: tar exit %lu, %d texture files in %s\n", j->label, code, n, j->dest);
        if (code == 0 && n > 0) {
            char key[96], v[64];
            snprintf(key, sizeof key, "installed_%llu", j->size);
            snprintf(v, sizeof v, "%d", n);
            pc_settings_set("TexPacks", key, v);
            oogie_textures_rescan();
        } else {
            char msg[3 * MAX_PATH];
            snprintf(msg, sizeof msg, "The %s could not be unpacked.\n\nUnpack %s yourself into the \"mods\" folder next to Project Oogie.exe; the game finds it there.",
                     j->label, j->path);
            MessageBoxA(NULL, msg, "Project Oogie", MB_OK | MB_ICONWARNING);
        }
    }
    free(j);
    return 0;
}

static void offer(const char *path, unsigned long long size)
{
    const char *name = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    int hd = has_word(name, "texture") && !has_word(name, "button");
    const char *label = hd ? "PS2 HD Texture Pack" : "Modern Xbox Buttons pack";
    char key[96], msg[3 * MAX_PATH], tar[MAX_PATH];
    const char *seen;
    PackJob *j;
    snprintf(key, sizeof key, "installed_%llu", size);
    if (pc_settings_get("TexPacks", key)) return;          /* installed before */
    snprintf(key, sizeof key, "declined_%llu", size);
    if ((seen = pc_settings_get("TexPacks", key)) && *seen == '1') return;
    GetSystemDirectoryA(tar, sizeof tar);
    strcat(tar, "\\tar.exe");
    if (GetFileAttributesA(tar) == INVALID_FILE_ATTRIBUTES) {
        snprintf(msg, sizeof msg, "Found the %s by RetroGenerica (with Solid Snack):\n%s\n\n"
                 "To use it, unpack it into the \"mods\" folder next to Project Oogie.exe; the game finds it there.",
                 label, path);
        MessageBoxA(NULL, msg, "Project Oogie", MB_OK | MB_ICONINFORMATION);
        pc_settings_set("TexPacks", key, "1");
        return;
    }
    snprintf(msg, sizeof msg, "Found the %s by RetroGenerica (with Solid Snack):\n%s\n\n"
             "Install it now? It is unpacked into the \"mods\" folder while you play (%s) and switched on when done. "
             "F1 > Mods turns it off again.\n\nNo: never ask about this file again.",
             label, path, hd ? "about 800 MB, a few minutes" : "a few seconds");
    if (MessageBoxA(NULL, msg, "Project Oogie: texture pack", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        pc_settings_set("TexPacks", key, "1");
        return;
    }
    j = (PackJob *)calloc(1, sizeof *j);
    if (!j) return;
    snprintf(j->path, sizeof j->path, "%s", path);
    snprintf(j->dest, sizeof j->dest, "mods\\%s", hd ? "PS2 HD Textures" : "Modern Xbox Buttons");
    snprintf(j->label, sizeof j->label, "%s", label);
    j->size = size;
    CloseHandle(CreateThread(NULL, 0, unpack_thread, j, 0, NULL));
}

static void scan(const char *dir)
{
    WIN32_FIND_DATAA fd;
    char pat[MAX_PATH];
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        size_t L = strlen(fd.cFileName);
        char full[MAX_PATH];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || L < 5) continue;
        if (_stricmp(fd.cFileName + L - 3, ".7z") && _stricmp(fd.cFileName + L - 4, ".zip")) continue;
        if (!has_word(fd.cFileName, "oogie") || !(has_word(fd.cFileName, "texture") || has_word(fd.cFileName, "button")))
            continue;
        if ((size_t)snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName) >= sizeof full) continue;
        offer(full, ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void pc_packinstall_check(void)
{
    char dl[MAX_PATH];
    DWORD n;
    if (getenv("OOGIE_TEST_SETTINGS") || getenv("OOGIE_NO_PACK_OFFER")) return;   /* test runs */
    scan(".");
    scan("mods");
    n = GetEnvironmentVariableA("USERPROFILE", dl, sizeof dl);
    if (n && n < sizeof dl - 16) { strcat(dl, "\\Downloads"); scan(dl); }
}
