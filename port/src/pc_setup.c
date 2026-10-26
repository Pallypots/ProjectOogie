/* Project Oogie: first-run setup (dev cycle 3, item 9).
 *
 * The port needs the files of the player's own disc. If game\default.xbe is
 * not next to the program (or one folder up, for bin\), the player picks a
 * disc image (.iso); the image is read, never written. Its default.xbe must
 * be the exact one the port was recompiled from (SHA-1 below: Xbox, USA);
 * then every file is copied to game\ with a progress window.
 *
 * Disc format (XDVDFS, after tools/xiso_tool.py): sectors of 2048 bytes; the
 * volume descriptor at sector 32 of the game partition holds
 * "MICROSOFT*XBOX*MEDIA", the root directory's sector and size. A directory
 * is a binary tree of entries: left and right offsets (in 4-byte units),
 * start sector, size, attributes (0x10 = directory), name length, name.
 *
 * Test without a window: oogie_recomp.exe --extract-iso <image> <folder>. */
#include <windows.h>
#include <commdlg.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

#include "pc_setup.h"

#define SECTOR 2048ull
static const uint64_t k_part[4] = { 0, 0x18300000ull, 0xFD90000ull, 0x2080000ull };
static const char k_xbe_sha1[] = "667c5bd8abc4a551ef6b7cbab3bf3a45e7244605";

typedef struct { char path[MAX_PATH]; uint32_t sec, size; int dir; } XEnt;
typedef struct {
    HANDLE f;
    uint64_t base;
    uint32_t root_sec, root_size;
    XEnt *e; int n, cap;
} XIso;

static int rd(XIso *x, uint64_t off, void *buf, DWORD n)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    li.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(x->f, li, NULL, FILE_BEGIN) && ReadFile(x->f, buf, n, &got, NULL) && got == n;
}

static int add(XIso *x, const char *path, uint32_t sec, uint32_t size, int dir)
{
    if (x->n == x->cap) {
        int cap = x->cap ? x->cap * 2 : 1024;
        XEnt *ne = (XEnt *)realloc(x->e, (size_t)cap * sizeof *ne);
        if (!ne) return 0;
        x->e = ne; x->cap = cap;
    }
    snprintf(x->e[x->n].path, MAX_PATH, "%s", path);
    x->e[x->n].sec = sec; x->e[x->n].size = size; x->e[x->n].dir = dir;
    x->n++;
    return 1;
}

static int walk(XIso *x, uint32_t sec, uint32_t size, const char *prefix, int depth)
{
    uint8_t *d, *seen;
    uint32_t *stack;
    int sp = 0, ok = 1;
    if (!size) return 1;
    if (depth > 32 || size > (64u << 20)) return 0;
    d = (uint8_t *)malloc(size);
    seen = (uint8_t *)calloc(size / 4 + 1, 1);
    stack = (uint32_t *)malloc((size / 4 + 2) * sizeof *stack);
    if (!d || !seen || !stack || !rd(x, x->base + sec * SECTOR, d, size)) { free(d); free(seen); free(stack); return 0; }
    stack[sp++] = 0;
    while (sp && ok) {
        uint32_t off = stack[--sp], st, sz;
        uint16_t l, r;
        uint8_t attr, nl;
        char name[256], path[MAX_PATH];
        if (off + 14 > size || seen[off / 4]) continue;
        seen[off / 4] = 1;
        memcpy(&l, d + off, 2); memcpy(&r, d + off + 2, 2);
        memcpy(&st, d + off + 4, 4); memcpy(&sz, d + off + 8, 4);
        attr = d[off + 12]; nl = d[off + 13];
        if (l == 0xFFFF || off + 14 + nl > size) continue;
        memcpy(name, d + off + 14, nl); name[nl] = 0;
        snprintf(path, sizeof path, "%s%s", prefix, name);
        if (attr & 0x10) {
            char sub[MAX_PATH];
            ok = add(x, path, st, sz, 1);
            snprintf(sub, sizeof sub, "%s\\", path);
            if (ok) ok = walk(x, st, sz, sub, depth + 1);
        } else {
            ok = add(x, path, st, sz, 0);
        }
        if (l) stack[sp++] = (uint32_t)l * 4u;
        if (r) stack[sp++] = (uint32_t)r * 4u;
    }
    free(d); free(seen); free(stack);
    return ok;
}

static int xiso_open(XIso *x, const char *path)
{
    int i;
    memset(x, 0, sizeof *x);
    x->f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (x->f == INVALID_HANDLE_VALUE) return 0;
    for (i = 0; i < 4; i++) {
        uint8_t m[28];
        if (rd(x, k_part[i] + 32 * SECTOR, m, 28) && !memcmp(m, "MICROSOFT*XBOX*MEDIA", 20)) {
            x->base = k_part[i];
            memcpy(&x->root_sec, m + 20, 4);
            memcpy(&x->root_size, m + 24, 4);
            return walk(x, x->root_sec, x->root_size, "", 0);
        }
    }
    return 0;
}

static void xiso_close(XIso *x)
{
    if (x->f && x->f != INVALID_HANDLE_VALUE) CloseHandle(x->f);
    free(x->e);
    memset(x, 0, sizeof *x);
}

/* SHA-1 of a file in the image, as 40 hex digits */
static int sha1_of(XIso *x, const XEnt *e, char hex[41])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    uint8_t dig[20], *buf = (uint8_t *)malloc(1 << 20);
    uint64_t left = e->size, pos = x->base + e->sec * SECTOR;
    int ok = buf && BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0))
             && BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0));
    while (ok && left) {
        DWORD n = (DWORD)(left < (1u << 20) ? left : (1u << 20));
        ok = rd(x, pos, buf, n) && BCRYPT_SUCCESS(BCryptHashData(h, buf, n, 0));
        pos += n; left -= n;
    }
    if (ok) ok = BCRYPT_SUCCESS(BCryptFinishHash(h, dig, 20, 0));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    free(buf);
    if (ok) { int i; for (i = 0; i < 20; i++) sprintf(hex + i * 2, "%02x", dig[i]); }
    return ok;
}

static volatile LONG64 s_done, s_total;
static volatile LONG s_state;           /* 0 running, 1 done, -1 failed */
static char s_err[512];

static int make_dirs(const char *path)
{
    char t[MAX_PATH];
    char *p;
    snprintf(t, sizeof t, "%s", path);
    for (p = t + 3; *p; p++)
        if (*p == '\\') { *p = 0; CreateDirectoryA(t, NULL); *p = '\\'; }
    return CreateDirectoryA(t, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int extract_all(XIso *x, const char *out)
{
    int i;
    uint8_t *buf = (uint8_t *)malloc(4u << 20);
    if (!buf) return 0;
    s_total = 0; s_done = 0;
    for (i = 0; i < x->n; i++) if (!x->e[i].dir) s_total += x->e[i].size;
    make_dirs(out);
    for (i = 0; i < x->n; i++) {
        char dst[MAX_PATH];
        snprintf(dst, sizeof dst, "%s\\%s", out, x->e[i].path);
        if (x->e[i].dir) { make_dirs(dst); continue; }
        {
            HANDLE o = CreateFileA(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            uint64_t left = x->e[i].size, pos = x->base + x->e[i].sec * SECTOR;
            if (o == INVALID_HANDLE_VALUE) {
                snprintf(s_err, sizeof s_err, "Could not create %s (error %lu).", dst, GetLastError());
                free(buf); return 0;
            }
            while (left) {
                DWORD n = (DWORD)(left < (4u << 20) ? left : (4u << 20)), w = 0;
                if (!rd(x, pos, buf, n) || !WriteFile(o, buf, n, &w, NULL) || w != n) {
                    snprintf(s_err, sizeof s_err, "Copying %s failed (error %lu). Is the disk full?", x->e[i].path, GetLastError());
                    CloseHandle(o); free(buf); return 0;
                }
                pos += n; left -= n;
                InterlockedAdd64(&s_done, n);
            }
            CloseHandle(o);
        }
    }
    free(buf);
    return 1;
}

/* Opens the image, checks it is the supported game and version; the reason
 * goes to s_err otherwise. */
static int check_image(XIso *x, const char *iso)
{
    int i;
    char hex[41];
    if (!xiso_open(x, iso)) {
        snprintf(s_err, sizeof s_err, "This file is not an Xbox disc image (XISO), or it could not be read:\n%s", iso);
        return 0;
    }
    for (i = 0; i < x->n; i++)
        if (!x->e[i].dir && !_stricmp(x->e[i].path, "default.xbe")) break;
    if (i == x->n) {
        snprintf(s_err, sizeof s_err, "This disc image has no default.xbe, so it is not a game disc.");
        return 0;
    }
    if (!sha1_of(x, &x->e[i], hex)) {
        snprintf(s_err, sizeof s_err, "Reading default.xbe from the disc image failed.");
        return 0;
    }
    if (strcmp(hex, k_xbe_sha1)) {
        snprintf(s_err, sizeof s_err,
                 "This disc image is a different game or a different version.\n\n"
                 "Project Oogie is built for The Nightmare Before Christmas: Oogie's Revenge, "
                 "Xbox, USA (default.xbe SHA-1 %s).\nThis image's default.xbe: %s", k_xbe_sha1, hex);
        return 0;
    }
    return 1;
}

int pc_setup_extract_cli(const char *iso, const char *out)
{
    XIso x;
    int ok = check_image(&x, iso);
    if (ok) {
        fprintf(stderr, "[SETUP] %s: %d entries, base 0x%llx; default.xbe matches; copying to %s\n",
                iso, x.n, (unsigned long long)x.base, out);
        ok = extract_all(&x, out);
    }
    if (ok) fprintf(stderr, "[SETUP] done: %d entries, %lld bytes\n", x.n, (long long)s_total);
    else fprintf(stderr, "[SETUP] failed: %s\n", s_err);
    xiso_close(&x);
    return ok;
}

typedef struct { XIso *x; char out[MAX_PATH]; } Job;

static DWORD WINAPI job_thread(LPVOID p)
{
    Job *j = (Job *)p;
    InterlockedExchange(&s_state, extract_all(j->x, j->out) ? 1 : -1);
    return 0;
}

static LRESULT CALLBACK prog_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) return 0;              /* not while copying */
    return DefWindowProcA(h, m, w, l);
}

/* Copies with a small progress window; returns 1 on success. */
static int extract_with_window(XIso *x, const char *out)
{
    WNDCLASSA wc;
    HWND win, txt;
    HANDLE th;
    Job j;
    MSG msg;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = prog_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_WAIT);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "OogieSetup";
    RegisterClassA(&wc);
    win = CreateWindowA("OogieSetup", "Project Oogie: setting up", WS_CAPTION | WS_VISIBLE,
                        CW_USEDEFAULT, CW_USEDEFAULT, 520, 140, NULL, NULL, wc.hInstance, NULL);
    txt = CreateWindowA("STATIC", "Copying the game files from your disc image...", WS_CHILD | WS_VISIBLE,
                        20, 25, 470, 60, win, NULL, wc.hInstance, NULL);
    j.x = x;
    snprintf(j.out, sizeof j.out, "%s", out);
    s_state = 0;
    th = CreateThread(NULL, 0, job_thread, &j, 0, NULL);
    if (!th) { DestroyWindow(win); return 0; }
    while (s_state == 0) {
        char t[200];
        double tot = (double)(s_total ? s_total : 1), done = (double)s_done;
        snprintf(t, sizeof t, "Copying the game files from your disc image...\n\n%.0f%%  (%.1f of %.1f GB)",
                 done * 100.0 / tot, done / 1e9, tot / 1e9);
        SetWindowTextA(txt, t);
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        MsgWaitForMultipleObjects(1, &th, FALSE, 150, QS_ALLINPUT);
    }
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    DestroyWindow(win);
    return s_state == 1;
}

static int has_game(const char *dir)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\game\\default.xbe", dir);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

int pc_setup_ensure_game(void)
{
    char exe[MAX_PATH], dir[MAX_PATH], up[MAX_PATH], iso[MAX_PATH] = "", *s, root[MAX_PATH];
    OPENFILENAMEA of;
    XIso x;
    int ok;
    if (GetFileAttributesA("game\\default.xbe") != INVALID_FILE_ATTRIBUTES) return 1;
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(dir, sizeof dir, "%s", exe);
    if ((s = strrchr(dir, '\\')) != NULL) *s = 0;
    snprintf(up, sizeof up, "%s", dir);
    if ((s = strrchr(up, '\\')) != NULL) *s = 0;
    if (has_game(dir)) { SetCurrentDirectoryA(dir); return 1; }
    if (has_game(up)) { SetCurrentDirectoryA(up); return 1; }
    /* the game goes next to the program, or next to bin\ */
    s = strrchr(dir, '\\');
    snprintf(root, sizeof root, "%s", (s && !_stricmp(s + 1, "bin")) ? up : dir);
    {
        /* Tests: OOGIE_SETUP_FROM=<image> runs the setup without dialogs
         * (the progress window still shows). */
        const char *from = getenv("OOGIE_SETUP_FROM");
        if (from && *from) {
            char out[MAX_PATH];
            if (!check_image(&x, from)) { xiso_close(&x); return 0; }
            snprintf(out, sizeof out, "%s\\game", root);
            ok = extract_with_window(&x, out);
            xiso_close(&x);
            if (ok) SetCurrentDirectoryA(root);
            return ok;
        }
    }
    if (MessageBoxA(NULL,
            "Welcome to Project Oogie.\n\n"
            "To play, the game files are copied once from your own copy of "
            "The Nightmare Before Christmas: Oogie's Revenge (Xbox, USA).\n\n"
            "Click OK and choose your disc image (.iso). The image is only read, never changed. "
            "About 3.5 GB of free space is needed.",
            "Project Oogie setup", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
        return 0;
    for (;;) {
        memset(&of, 0, sizeof of);
        of.lStructSize = sizeof of;
        of.lpstrFilter = "Xbox disc image (*.iso;*.xiso)\0*.iso;*.xiso\0All files\0*.*\0";
        of.lpstrFile = iso;
        of.nMaxFile = sizeof iso;
        of.lpstrTitle = "Choose your Oogie's Revenge disc image";
        of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameA(&of)) return 0;
        if (check_image(&x, iso)) break;
        xiso_close(&x);
        if (MessageBoxA(NULL, s_err, "Project Oogie setup", MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY)
            return 0;
    }
    {
        char out[MAX_PATH];
        snprintf(out, sizeof out, "%s\\game", root);
        ok = extract_with_window(&x, out);
    }
    xiso_close(&x);
    if (!ok) {
        MessageBoxA(NULL, s_err[0] ? s_err : "Copying the game files failed.", "Project Oogie setup", MB_ICONERROR);
        return 0;
    }
    SetCurrentDirectoryA(root);
    MessageBoxA(NULL, "The game files are in place. Enjoy the game!\n\nF1 opens the settings at any time.",
                "Project Oogie setup", MB_ICONINFORMATION);
    return 1;
}

/* ---- started on its own (release 1) -----------------------------------------
 * play.bat sets the runtime's switches, puts saves in saves\, backs them up,
 * and sends the log to logs\play.log. A player runs the program directly, so
 * when nothing set the environment (RECOMP_PB_EXEC unset), the program does
 * the same itself. play.bat and the test scripts set it, and are unaffected. */
static void copy_tree(const char *from, const char *to)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    CreateDirectoryA(to, NULL);
    snprintf(pat, sizeof pat, "%s\\*", from);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char a[MAX_PATH], b[MAX_PATH];
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        snprintf(a, sizeof a, "%s\\%s", from, fd.cFileName);
        snprintf(b, sizeof b, "%s\\%s", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copy_tree(a, b);
        else CopyFileA(a, b, FALSE);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void delete_tree(const char *dir)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char a[MAX_PATH];
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(a, sizeof a, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) delete_tree(a);
            else DeleteFileA(a);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

static int name_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* saves\UDATA to saves_backup\<date_time>\UDATA; the ten newest are kept. */
static void backup_saves(void)
{
    char dst[MAX_PATH], (*names)[64] = NULL;
    SYSTEMTIME t;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0, cap = 0, i;
    if (GetFileAttributesA("saves\\UDATA") == INVALID_FILE_ATTRIBUTES) return;
    CreateDirectoryA("saves_backup", NULL);
    GetLocalTime(&t);
    snprintf(dst, sizeof dst, "saves_backup\\%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    CreateDirectoryA(dst, NULL);
    strncat(dst, "\\UDATA", sizeof dst - strlen(dst) - 1);
    copy_tree("saves\\UDATA", dst);
    h = FindFirstFileA("saves_backup\\*", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        if (n == cap) {
            void *nn = realloc(names, (size_t)(cap = cap ? cap * 2 : 32) * 64);
            if (!nn) break;
            names = (char (*)[64])nn;
        }
        snprintf(names[n++], 64, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (names) {
        qsort(names, (size_t)n, 64, name_cmp);
        for (i = 0; i + 10 < n; i++) {
            char old[MAX_PATH];
            snprintf(old, sizeof old, "saves_backup\\%s", names[i]);
            delete_tree(old);
        }
        free(names);
    }
}

static int s_standalone;
int pc_setup_is_standalone(void) { return s_standalone; }

void pc_setup_standalone(void)
{
    static const char *const defaults[] = {
        "RECOMP_PB_EXEC=1", "RECOMP_AC97_READY=1", "RECOMP_MISSING_FILES=1", "RECOMP_USB=1",
        "RECOMP_KEYBOARD=1", "RECOMP_HEAP_RECLAIM=1", "OOGIE_D3D11=1", "OOGIE_QUIET_GPU=1",
        "RECOMP_VBLANK=1" };
    char full[MAX_PATH], env[MAX_PATH + 32];
    size_t i;
    if (getenv("RECOMP_PB_EXEC")) return;           /* play.bat or a test script set things up */
    s_standalone = 1;
    for (i = 0; i < sizeof defaults / sizeof defaults[0]; i++) _putenv(defaults[i]);
    CreateDirectoryA("saves", NULL);
    CreateDirectoryA("saves\\UDATA", NULL);
    if (GetFullPathNameA("saves", sizeof full, full, NULL)) {
        snprintf(env, sizeof env, "RECOMP_SAVE_DIR=%s", full);
        _putenv(env);
    }
    backup_saves();
    /* The log: logs\play.log, the last two sessions kept as play.1.log and
     * play.2.log. Only when nothing receives the program's output already. */
    {
        HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
        DWORD ty = (e && e != INVALID_HANDLE_VALUE) ? GetFileType(e) : FILE_TYPE_UNKNOWN;
        if (ty != FILE_TYPE_DISK && ty != FILE_TYPE_PIPE) {
            CreateDirectoryA("logs", NULL);
            MoveFileExA("logs\\play.1.log", "logs\\play.2.log", MOVEFILE_REPLACE_EXISTING);
            MoveFileExA("logs\\play.log", "logs\\play.1.log", MOVEFILE_REPLACE_EXISTING);
            if (freopen("logs\\play.log", "w", stderr)) {
                setvbuf(stderr, NULL, _IONBF, 0);
                SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
            }
            if (freopen("logs\\play_out.log", "w", stdout)) {
                setvbuf(stdout, NULL, _IONBF, 0);
                SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
            }
        }
    }
    fprintf(stderr, "[OOGIE] started on its own: saves in saves\\, log in logs\\play.log\n");
}
