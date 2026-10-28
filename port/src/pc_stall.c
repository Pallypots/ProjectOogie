/* Project Oogie: stall sampler.
 *
 * The owner sees short freezes while the frame counter reads 60 (play.1.log:
 * many [HITCH] lines of 510-530 ms) and one hang that Windows closed
 * (2026-10-19 09:55, during the title film; no [CRASH], AppHangB1). A frame
 * time says how long, not where. This thread watches the presents counter
 * (d3d11_backend.c); when no frame has been shown for 200 ms it captures the
 * flipping thread's call stack, and at 2 s, 6 s and 15 s every thread's, as
 * function names (sub_XXXXXXXX are guest functions). One [STALL] block per
 * stall. OOGIE_STALL_LOG=0 turns it off.
 *
 * The stack is copied with the thread suspended using only RtlVirtualUnwind
 * (no heap, no locks); names are looked up after it is resumed, so a thread
 * suspended while holding the heap lock cannot deadlock the sampler. */
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#pragma comment(lib, "dbghelp.lib")

extern volatile long g_oogie_presents;
int pc_menu_is_open(void);              /* the settings menu holds the game on purpose */

static DWORD s_flip_tid;
static DWORD s_game_tid;     /* the thread that runs the game (main.c) */
static int   s_sym_ok;

#define MAXF 24

void pc_stall_note_flip_thread(void)
{
    s_flip_tid = GetCurrentThreadId();
}

/* Wine / Proton (Steam Deck, SteamOS, Linux): ntdll exports wine_get_version. */
static int under_wine(void)
{
    static int w = -1;
    if (w < 0) {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        w = nt && GetProcAddress(nt, "wine_get_version") != NULL;
    }
    return w;
}

static int capture(DWORD tid, DWORD64 *pc, int max)
{
    HANDLE h;
    CONTEXT c;
    int n = 0;
    /* 0.2, Wine test boot3: walking a suspended thread's stack faulted inside
     * Wine's ntdll (null read at RtlVirtualUnwind's level) right after the
     * first "[STALL] no frame for 200 ms" of the boot, and closed the game.
     * Under Wine no stacks are taken (the stall lines still are), and the walk
     * is guarded everywhere: a fault ends it, not the program. */
    if (under_wine()) return 0;
    h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) return 0;
    if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); return 0; }
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(h, &c)) __try {
        while (n < max && c.Rip) {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f;
            pc[n++] = c.Rip;
            f = RtlLookupFunctionEntry(c.Rip, &base, NULL);
            if (f) {
                PVOID hd = NULL; DWORD64 ef = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &hd, &ef, NULL);
            } else {
                /* leaf function: the return address is at rsp */
                if (!c.Rsp || IsBadReadPtr((void *)c.Rsp, 8)) break;
                c.Rip = *(DWORD64 *)c.Rsp;
                c.Rsp += 8;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* an unwind that faulted: keep the frames found so far */
    }
    ResumeThread(h);
    CloseHandle(h);
    return n;
}

static void print_stack(DWORD tid, const char *what, int guest_only)
{
    DWORD64 pc[MAXF];
    int n = capture(tid, pc, MAXF), i;
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    char line[1600];
    size_t len;
    static SRWLOCK sym_lock = SRWLOCK_INIT;   /* dbghelp is single-threaded */

    AcquireSRWLockExclusive(&sym_lock);
    len = (size_t)snprintf(line, sizeof line, "[STALL]   thread %lu%s:", (unsigned long)tid, what);
    for (i = 0; i < n && len < sizeof line - 80; i++) {
        DWORD64 disp = 0;
        memset(buf, 0, sizeof buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        if (s_sym_ok && SymFromAddr(GetCurrentProcess(), pc[i], &disp, si))
            len += (size_t)snprintf(line + len, sizeof line - len, " %s+%llX", si->Name, (unsigned long long)disp);
        else
            len += (size_t)snprintf(line + len, sizeof line - len, " %llX", (unsigned long long)pc[i]);
    }
    ReleaseSRWLockExclusive(&sym_lock);
    if (guest_only && !strstr(line, " sub_"))
        return;                 /* host-only thread: not part of the game */
    fprintf(stderr, "%s\n", line);
}

static void print_all(int guest_only)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != me)
            print_stack(te.th32ThreadID, te.th32ThreadID == s_flip_tid ? " (renders)"
                        : te.th32ThreadID == s_game_tid ? " (game)" : "", guest_only);
        te.dwSize = sizeof te;
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}

static DWORD WINAPI stall_thread(LPVOID arg)
{
    long last = g_oogie_presents;
    DWORD since = GetTickCount();
    int stage = 0;          /* 0 none, 1 short sample done, 2..4 full dumps done */
    unsigned stalls = 0;
    static const DWORD full_at[] = { 2000, 6000, 15000 };
    (void)arg;

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_NO_PROMPTS);
    {
        /* The .pdb is next to the exe (bin\), not in the working directory. */
        char dir[MAX_PATH], *slash;
        DWORD n = GetModuleFileNameA(NULL, dir, MAX_PATH);
        if (n && n < MAX_PATH && (slash = strrchr(dir, '\\')) != NULL) *slash = 0;
        else dir[0] = 0;
        s_sym_ok = SymInitialize(GetCurrentProcess(), dir[0] ? dir : NULL, TRUE);
        /* 87: the crash handler loaded them already; they are usable. */
        if (!s_sym_ok && GetLastError() == ERROR_INVALID_PARAMETER) s_sym_ok = 1;
        if (!s_sym_ok) fprintf(stderr, "[STALL] symbols unavailable (%lu): stacks are addresses\n", GetLastError());
    }
    for (;;) {
        long p = g_oogie_presents;
        DWORD now = GetTickCount();
        Sleep(50);
        {
            extern int oogie_sem_watch(void);
            if (oogie_sem_watch() && s_game_tid)
                print_stack(s_game_tid, " (game, when the semaphore went back)", 0);
        }
        if (p != last || pc_menu_is_open()) {
            if (stage)
                fprintf(stderr, "[STALL] #%u ended after %lu ms\n", stalls, (unsigned long)(now - since));
            last = p; since = now; stage = 0;
            continue;
        }
        {
            extern int oogie_gpu_kick_check(void);
            if (p) oogie_gpu_kick_check();  /* #54: a kickoff that never came */
        }
        if (!s_flip_tid || p == 0)
            continue;                       /* boot, before the first frame */
        {
            /* #54: a fence wait the GPU can no longer answer (recomp_manual.c) */
            extern int oogie_gpu_unstick(unsigned long stalled_ms);
            static DWORD last_try;
            if (now - since >= 1000 && now - last_try >= 1000) {
                last_try = now;
                oogie_gpu_unstick((unsigned long)(now - since));
            }
        }
        if (stage == 0 && now - since >= 200) {
            stage = 1; stalls++;
            if (stalls <= 400) {
                fprintf(stderr, "[STALL] #%u no frame for %lu ms (frame %ld)\n", stalls, (unsigned long)(now - since), p);
                if (stalls <= 60) {
                    /* every thread running game code: which one the game waits for */
                    print_all(1);
                    print_stack(s_flip_tid, " (renders)", 0);
                } else {
                    if (s_game_tid && s_game_tid != s_flip_tid) print_stack(s_game_tid, " (game)", 0);
                    print_stack(s_flip_tid, " (renders)", 0);
                }
                fflush(stderr);
            }
        } else if (stage >= 1 && stage <= 3 && now - since >= full_at[stage - 1] && stalls <= 400) {
            fprintf(stderr, "[STALL] #%u still no frame after %lu ms, all threads:\n", stalls, (unsigned long)(now - since));
            print_all(0);
            {
                extern void oogie_wait_dump(void);   /* #61: what each thread waits on (recomp_manual.c) */
                oogie_wait_dump();
            }
            fflush(stderr);
            stage++;
        }
    }
    return 0;
}

/* KNOWN_ISSUES #55: D3D's KickOff (sub_001D76F0) sets 0x100410 bit 16 and
 * spins until the "GPU" clears it, holding the game's render lock. The
 * runtime clears it from the GPU thread only, between commands, so a GPU
 * thread busy with a heavy batch (texture uploads, shader compiles at a
 * cutscene start) held the game for 100-460 ms ([CS] sk60d). The bit means
 * "write caches flushed", which is always true here, so a thread answers it
 * within a millisecond. OOGIE_ACK_THREAD=0 turns it off. */
static DWORD WINAPI ack_thread(LPVOID arg)
{
    /* Only the kickoff bit. xbox_Nv2aAckBusyBits also clears interrupt
     * status registers, and doing that every millisecond from here could take
     * an interrupt away before the game saw it. */
    extern volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);
    volatile uint32_t *r = NULL;
    (void)arg;
    for (;;) {
        if (!r) r = xbox_Nv2aRegPtr(0x100410);
        if (r && (*r & 0x00010000u))
            InterlockedAnd((volatile LONG *)r, ~0x00010000);
        Sleep(1);
    }
    return 0;
}

void pc_stall_start(void)
{
    {
        static LONG ack_once;
        const char *a = getenv("OOGIE_ACK_THREAD");
        if (!(a && *a == '0') && !InterlockedCompareExchange(&ack_once, 1, 0))
            CloseHandle(CreateThread(NULL, 0, ack_thread, NULL, 0, NULL));
    }
    static LONG once;
    const char *e = getenv("OOGIE_STALL_LOG");
    if (e && *e == '0') return;
    if (InterlockedCompareExchange(&once, 1, 0)) return;
    s_game_tid = GetCurrentThreadId();   /* called on the game thread before it starts */
    CloseHandle(CreateThread(NULL, 0, stall_thread, NULL, 0, NULL));
}

/* One thread's call stack, for the kernel's lock-wait report (patch
 * cs_wait_report). Symbols are loaded by the sampler thread; before that the
 * stack is printed as addresses. */
void pc_stall_print_thread(unsigned long tid, const char *what)
{
    /* print_stack suspends the thread to walk it: a thread that asks for
     * its own stack would suspend itself for good (owner 2026-10-22 11:25:
     * the game froze on the first frame-order cap, which printed the main
     * thread's own stack). */
    if ((DWORD)tid == GetCurrentThreadId())
        return;
    print_stack((DWORD)tid, what, 0);
}
