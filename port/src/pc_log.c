/* Project Oogie: the log is written by its own thread.
 *
 * Every fprintf(stderr) went straight to logs\play.log, and the C runtime
 * serialises them on one lock. When a write was slow (a busy disk, a virus
 * scanner looking at a file that grows all the time), every thread that
 * logged waited behind it: [CS] sk58c caught DSOUND holding the XACT lock
 * inside fprintf for 234 ms while the game thread waited for that lock, the
 * owner's half-second freezes (KNOWN_ISSUES #55).
 *
 * Now stdout and stderr each write into a 4 MB pipe, and a thread per pipe
 * copies it to the original file. Writing into the pipe takes microseconds.
 * The crash handler calls pc_log_drain() so that nothing is lost when the
 * process dies; so does exit. OOGIE_SYNC_LOG=1 keeps the old direct writes. */
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    HANDLE rd, out;
    volatile LONG busy;          /* the writer holds bytes it has not written */
} LogPipe;

static LogPipe s_pipe[2];
static int s_on;

static DWORD WINAPI writer(LPVOID p)
{
    LogPipe *lp = (LogPipe *)p;
    static char buf[2][65536];
    char *b = buf[lp == &s_pipe[1]];
    for (;;) {
        DWORD n = 0, w;
        if (!ReadFile(lp->rd, b, 65536, &n, NULL) || n == 0) {
            InterlockedExchange(&lp->busy, 0);
            if (GetLastError() == ERROR_BROKEN_PIPE) return 0;
            Sleep(1);
            continue;
        }
        InterlockedExchange(&lp->busy, 1);
        {
            DWORD off = 0;
            while (off < n && WriteFile(lp->out, b + off, n - off, &w, NULL) && w)
                off += w;
        }
        InterlockedExchange(&lp->busy, 0);
    }
}

static int redirect(int fd, DWORD std_id, LogPipe *lp)
{
    HANDLE r, w;
    int nfd;
    lp->out = (HANDLE)_get_osfhandle(fd);
    if (lp->out == INVALID_HANDLE_VALUE || lp->out == NULL)
        return 0;
    if (GetFileType(lp->out) == FILE_TYPE_CHAR)
        return 0;                    /* a console: leave it alone */
    if (!CreatePipe(&r, &w, NULL, 4u << 20))
        return 0;
    /* keep the original file open for the writer, separate from fd */
    if (!DuplicateHandle(GetCurrentProcess(), lp->out, GetCurrentProcess(), &lp->out,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) {
        CloseHandle(r); CloseHandle(w);
        return 0;
    }
    nfd = _open_osfhandle((intptr_t)w, _O_WRONLY | _O_BINARY);
    if (nfd < 0 || _dup2(nfd, fd) != 0) {
        CloseHandle(r);
        return 0;
    }
    _close(nfd);
    SetStdHandle(std_id, (HANDLE)_get_osfhandle(fd));
    lp->rd = r;
    CloseHandle(CreateThread(NULL, 0, writer, lp, 0, NULL));
    return 1;
}

void pc_log_drain(DWORD max_ms)
{
    DWORD t0 = GetTickCount();
    int k;
    if (!s_on) return;
    fflush(stdout);
    fflush(stderr);
    for (k = 0; k < 2; k++) {
        while (s_pipe[k].rd && GetTickCount() - t0 < max_ms) {
            DWORD avail = 0;
            if (!PeekNamedPipe(s_pipe[k].rd, NULL, 0, NULL, &avail, NULL)) break;
            if (avail == 0 && !s_pipe[k].busy) break;
            Sleep(2);
        }
    }
}

static void drain_at_exit(void)
{
    pc_log_drain(2000);
}

void pc_log_start(void)
{
    /* Opt-in for now (KNOWN_ISSUES #61): with the log no longer holding every
     * thread behind one lock, the game's threads overlap more and a race in
     * the render lists froze both soaks within 8 minutes (bsC1, bsC2), where
     * the same build with direct writes ran clean (bsD1, bsD2).
     * OOGIE_FAST_LOG=1 turns the pipe on.
     *
     * Dev cycle 7 (2026-10-24): on by default again under lock mode
     * (RECOMP_GUEST_LOCK, the default; main.c sets it after this), which
     * runs one game thread at a time and so closes the race (#69). The
     * [STALL] stacks of every load pause showed the game's threads queued on
     * the log's lock (fputs -> RtlEnterCriticalSection, one in WriteFile):
     * opening cutscene with the owner's settings, bp1 / fl1: 99 -> 32 slow
     * frames, 19 -> 0 over 100 ms, 8 -> 0 stalls. OOGIE_FAST_LOG=0 (or
     * OOGIE_SYNC_LOG=1, or the lock off) keeps direct writes. */
    const char *f = getenv("OOGIE_FAST_LOG");
    const char *e = getenv("OOGIE_SYNC_LOG");
    const char *lk = getenv("RECOMP_GUEST_LOCK");
    int lock_on = !(lk && *lk == '0');
    if (f && *f == '0') return;
    if (!(f && *f == '1') && !lock_on) return;
    if (e && *e == '1') return;
    fflush(stdout);
    fflush(stderr);
    s_on = redirect(2, STD_ERROR_HANDLE, &s_pipe[0]);
    s_on |= redirect(1, STD_OUTPUT_HANDLE, &s_pipe[1]);
    if (s_on) atexit(drain_at_exit);
}
