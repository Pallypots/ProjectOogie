/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>   /* getenv, abort */
#include <string.h>   /* memcpy */
#include <windows.h>

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

/* Guest registers are thread-local in the runtime (RECOMP_TLS in recomp_types.h).
 * The template declared g_eax without TLS; fixed here (Oogie). */
#if defined(_MSC_VER)
#  define OOGIE_TLS __declspec(thread)
#else
#  define OOGIE_TLS _Thread_local
#endif
extern OOGIE_TLS uint32_t g_eax, g_ecx, g_esp;
static int oogie_yield(void);   /* a yield that lets any priority run (#61), below */

/* The runtime's guest CPU lock (RECOMP_GUEST_LOCK=1, KNOWN_ISSUES #61): one
 * game thread runs game code at a time and lets go in every kernel call. Port
 * code that waits on its own lock or yields outside a kernel call must let go
 * too, or a thread holding that lock and back from a kernel call waits for the
 * CPU while the CPU's holder waits for the lock. Lock order: these locks, then
 * the CPU. Patch guest_cpu_held exports the query. */
extern int  guest_cpu_held(void);
extern void guest_cpu_part(void);
extern void guest_cpu_join(void);

static void oogie_cs_enter(CRITICAL_SECTION *cs)
{
    if (TryEnterCriticalSection(cs)) return;
    if (guest_cpu_held()) {
        guest_cpu_part();
        EnterCriticalSection(cs);
        guest_cpu_join();
    } else {
        EnterCriticalSection(cs);
    }
}
static BOOL oogie_host_yield(int sleep_ms)
{
    BOOL r = TRUE;
    int held = guest_cpu_held();
    if (held) guest_cpu_part();
    if (sleep_ms >= 0) Sleep((DWORD)sleep_ms); else r = SwitchToThread();
    if (held) guest_cpu_join();
    return r;
}
volatile int s_guests_pinned_flag;  /* game threads share one processor (#61) */
extern ptrdiff_t g_xbox_mem_offset;

#define OOGIE_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define OOGIE_MEM16(va) (*(volatile uint16_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* Dev cycle 4: the guest CPU lock's safepoint. RECOMP_CALL_PROFILE (CMake)
 * makes every direct guest call come here; while a thread of higher priority
 * waits for the CPU (g_gcpu_preempt, patch guest_cpu_priority) the running
 * thread steps aside at its next call, as the console preempts it, unless it
 * runs at DISPATCH_LEVEL or above (guest IRQL at fs:[0x24]). Without it a
 * low-priority thread computing between kernel calls held the frame threads
 * up (lock mode at 51 fps in fights with the CPU 60% idle). */
extern volatile LONG g_gcpu_preempt;
extern void guest_cpu_safepoint(void);
extern OOGIE_TLS uint32_t g_fs_base;
/* HLE renderer stage 0 (docs/D3D8_API_INVENTORY.md): OOGIE_D3D_CALLS=1 counts
 * every direct call into the D3D library section (0x1D1B80-0x1E4AE8) and logs
 * the 24 most called targets with calls per presented frame every 10 s. */
#define OOGIE_D3D_LO 0x001D1B80u
#define OOGIE_D3D_HI 0x001E4AE8u
static int s_d3dc_on = -1;
static volatile LONG s_d3dc_n[1024];
static volatile LONG s_d3dc_va[1024];
static volatile LONG s_d3dc_total;
extern volatile long g_oogie_presents;
static void d3d_calls_count(uint32_t va)
{
    uint32_t h = (va * 2654435761u) >> 22, i;
    for (i = 0; i < 1024; i++, h = (h + 1) & 1023) {
        LONG cur = s_d3dc_va[h];
        if (!cur) cur = InterlockedCompareExchange(&s_d3dc_va[h], (LONG)va, 0), cur = cur ? cur : (LONG)va;
        if ((uint32_t)cur == va) { InterlockedIncrement(&s_d3dc_n[h]); return; }
    }
}
static void d3d_calls_report(void)
{
    static volatile LONG busy;
    static DWORD last;
    static long last_presents;
    DWORD now = GetTickCount();
    long frames;
    LONG n[1024]; uint32_t va[1024];
    int i, j, k, idx[24];
    if (now - last < 10000 || InterlockedExchange(&busy, 1)) return;
    last = now;
    frames = g_oogie_presents - last_presents; last_presents = g_oogie_presents;
    for (i = 0; i < 1024; i++) { va[i] = (uint32_t)s_d3dc_va[i]; n[i] = InterlockedExchange(&s_d3dc_n[i], 0); }
    if (frames > 0) {
        for (k = 0; k < 24; k++) {                   /* the 24 most called, by selection */
            int best = -1;
            for (i = 0; i < 1024; i++) {
                if (!va[i] || !n[i]) continue;
                for (j = 0; j < k && idx[j] != i; j++) ;
                if (j < k) continue;
                if (best < 0 || n[i] > n[best]) best = i;
            }
            if (best < 0) break;
            idx[k] = best;
        }
        fprintf(stderr, "[D3DCALLS] %ld frames, calls per frame:", frames);
        for (j = 0; j < k; j++)
            fprintf(stderr, " %08X:%.1f", va[idx[j]], (double)n[idx[j]] / (double)frames);
        fprintf(stderr, "\n");
    }
    InterlockedExchange(&busy, 0);
}

void recomp_call_profile_hit(uint32_t va)
{
    if (s_d3dc_on) {
        if (s_d3dc_on < 0) { const char *e = getenv("OOGIE_D3D_CALLS"); s_d3dc_on = e && *e == '1'; }
        if (s_d3dc_on && va >= OOGIE_D3D_LO && va < OOGIE_D3D_HI) {
            d3d_calls_count(va);
            if ((InterlockedIncrement(&s_d3dc_total) & 4095) == 0) d3d_calls_report();
        }
    }
    if (g_gcpu_preempt && g_fs_base
            && *(volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + g_fs_base + 0x24) < 2)
        guest_cpu_safepoint();
}
#define OOGIE_MEM8(va)  (*(volatile uint8_t  *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
/* ── Oogie: KeStallExecutionProcessor statistics ───────────────
 *
 * tools/sampler (d3d5, Town Square): the game thread spent ~70% of its time
 * in the runtime's KeStallExecutionProcessor (a QPC busy-wait). All 8 call
 * sites are in DSOUND (AC'97 / APU register polls with 1-20 us stalls). With
 * OOGIE_STALL_STATS=1 the kernel slot (thunk 0x0025FA6C -> synthetic VA
 * 0xFE00018C, ordinal 151) is served here instead: same busy-wait, plus a
 * per-call-site tally printed every 10 s, to find which poll never sees its
 * answer. Called like a guest function (return address at [esp], the
 * microseconds at [esp+4]); stdcall with one argument, so it pops 8 bytes.
 */
#define OOGIE_KESTALL_VA 0xFE00018Cu
static struct { uint32_t ret, calls; uint64_t us; } s_stall[64];
static volatile LONG s_stall_lock;

static void oogie_ke_stall(void)
{
    uint32_t us = OOGIE_MEM32(g_esp + 4), ret = OOGIE_MEM32(g_esp);
    static LARGE_INTEGER freq;
    static DWORD last_report;
    LARGE_INTEGER t0, t;
    int i;

    while (InterlockedCompareExchange(&s_stall_lock, 1, 0)) SwitchToThread();
    for (i = 0; i < 64 && s_stall[i].ret && s_stall[i].ret != ret; i++)
        ;
    if (i < 64) { s_stall[i].ret = ret; s_stall[i].calls++; s_stall[i].us += us; }
    if (GetTickCount() - last_report > 10000) {
        last_report = GetTickCount();
        fprintf(stderr, "[STALL] KeStallExecutionProcessor by call site (calls, total us):\n");
        for (i = 0; i < 64 && s_stall[i].ret; i++)
            fprintf(stderr, "[STALL]   ret 0x%08X  %u calls  %llu us\n", s_stall[i].ret,
                    s_stall[i].calls, (unsigned long long)s_stall[i].us);
        fflush(stderr);
    }
    InterlockedExchange(&s_stall_lock, 0);

    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    do { QueryPerformanceCounter(&t); }
    while ((uint64_t)(t.QuadPart - t0.QuadPart) * 1000000u < (uint64_t)us * (uint64_t)freq.QuadPart);
    g_eax = 0;
    g_esp += 8;   /* ret + one stdcall argument */
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    static int s_stall_stats = -1;
    if (xbox_va == OOGIE_KESTALL_VA) {
        if (s_stall_stats < 0) {
            const char *e = getenv("OOGIE_STALL_STATS");
            s_stall_stats = e && *e && *e != '0';
        }
        if (s_stall_stats)
            return oogie_ke_stall;
    }
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── Oogie: D3D fence wait (D3D_BlockOnTime) ───────────────────
 *
 * sub_001D78D0 is the stock XDK 5849 D3D fence wait (verified by disassembly,
 * docs/REVERSE_ENGINEERING.md). It spins until the GPU semaphore at
 * *(device+0x30) reaches the fence; device pointer is at 0x001E25E8.
 * The pushbuffer executor (RECOMP_PB_EXEC) writes semaphore releases only
 * once it knows where the semaphore is, so tell it on entry. The original
 * body (including its kickoff) still runs unchanged.
 */
#define OOGIE_D3D_DEVICE_PTR 0x001E25E8u

extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);

static volatile uint32_t s_fence_wanted;   /* the fence the last wait asked for */

/* KNOWN_ISSUES #54: the title froze with the game thread spinning in this
 * function's last loop (0x1D7A07: "until *semaphore reaches the fence")
 * while the GPU had nothing left to do (sk58e at boot; the owner's 09:55
 * freeze in the title film), so the release for that fence never came.
 * Called by the stall sampler (pc_stall.c) once no frame has been shown for
 * a second. With the GPU idle (GET == PUT): if D3D's write pointer is past
 * PUT, the GPU is handed the rest (a kickoff); if everything was submitted
 * and executed, the fence has passed and its value is written, as the
 * release would have. Returns 1 if it acted. OOGIE_NO_UNSTICK=1: report only. */
extern volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);
extern int nv2a_pb_dma_walk(void);
int oogie_gpu_unstick(unsigned long stalled_ms)
{
    static int off = -1;
    static unsigned n_acts;
    uint32_t dev = *(volatile uint32_t *)((uintptr_t)OOGIE_D3D_DEVICE_PTR + g_xbox_mem_offset);
    uint32_t sem_va, sem, fence = s_fence_wanted, wp, wp_phys;
    volatile uint32_t *put = xbox_Nv2aRegPtr(0x800040), *get = xbox_Nv2aRegPtr(0x800044);
    if (off < 0) { const char *e = getenv("OOGIE_NO_UNSTICK"); off = e && *e == '1'; }
    if (!dev || !put || !get || !fence)
        return 0;
    sem_va = *(volatile uint32_t *)((uintptr_t)(dev + 0x30) + g_xbox_mem_offset);
    if (!sem_va)
        return 0;
    sem = *(volatile uint32_t *)((uintptr_t)sem_va + g_xbox_mem_offset);
    if ((int32_t)(sem - fence) >= 0)
        return 0;                                   /* not waiting on a fence */
    if (OOGIE_MEM32(dev + 8) & 0x2004u)
        return 0;                                   /* recording or another kick path: see oogie_gpu_kick_check */
    wp = *(volatile uint32_t *)((uintptr_t)dev + g_xbox_mem_offset);
    wp_phys = wp & 0x0FFFFFFFu;
    fprintf(stderr, "[GPU] stalled %lu ms: fence %08X, semaphore %08X, GET %08X PUT %08X, D3D writes at %08X%s\n",
            stalled_ms, fence, sem, *get, *put, wp, off ? " (OOGIE_NO_UNSTICK: no action)" : "");
    if (off || *get != *put || n_acts >= 1000)
        return 0;
    n_acts++;
    if (wp_phys > (*put & 0x0FFFFFFFu)) {
        fprintf(stderr, "[GPU]   handing the GPU the commands after PUT (kickoff to %08X)\n", wp_phys);
        *put = wp_phys;
    } else if (nv2a_pb_dma_walk() && wp_phys == (*put & 0x0FFFFFFFu) && stalled_ms >= 2000) {
        /* KNOWN_ISSUES #66, the owner's play.log of 2026-10-20 09:50 (title
         * screen): with the DMA walk the GPU's GET is exact, and here GET ==
         * PUT == D3D's write pointer, so every command D3D wrote has been
         * consumed. The walk lost this fence's release on a resync (a bad
         * jump into 0x11100), and the title would wait for good. On hardware
         * the fence has passed, and D3D reuses no ring space the GPU still
         * needs, so the write is safe in this state (rs2's replays came from
         * the segment scan, whose "consumed" was not GET). The newest fence
         * D3D issued ([dev+0x2C]) is written when it is not behind. */
        uint32_t issued = OOGIE_MEM32(dev + 0x2C), v = fence;
        if ((int32_t)(issued - fence) > 0 && issued - fence < 0x400u)
            v = issued;
        fprintf(stderr, "[GPU]   every command consumed (GET = PUT = D3D's write pointer), fence %08X lost: semaphore set to %08X\n",
                fence, v);
        *(volatile uint32_t *)((uintptr_t)sem_va + g_xbox_mem_offset) = v;
    } else {
        /* No longer writes the fence (KNOWN_ISSUES #66): a fence whose
         * release the GPU never ran is not done. Writing it told D3D the GPU
         * was further on than it was; D3D then reused ring space and the
         * emulator replayed old commands (the semaphore went back from 0xB89
         * to 0x96F right after each such write, rs2). Reported only. */
        fprintf(stderr, "[GPU]   fence %08X is not in anything submitted; left alone\n", fence);
        n_acts--;
        fflush(stderr);
        return 0;
    }
    fflush(stderr);
    return 1;
}

/* Called by the stall sampler every 50 ms while no frame is shown
 * (pc_stall.c). The common case of #54 (sk60c, menus): a game task thread
 * waits in this fence wait for a fence whose commands D3D wrote but never
 * handed over (D3D's write pointer past PUT, GPU idle), and the main thread
 * waits for that task. That never resolves by itself, and handing the GPU
 * what was written is what a kickoff does, so it is done after two samples
 * in that state (~100 ms) rather than after the 1 s of oogie_gpu_unstick. */
int oogie_gpu_kick_check(void)
{
    static int hits, idle_hits;
    static unsigned n, n_lost;
    static int off = -1;
    uint32_t dev = *(volatile uint32_t *)((uintptr_t)OOGIE_D3D_DEVICE_PTR + g_xbox_mem_offset);
    uint32_t sem_va, sem, fence = s_fence_wanted, wp_phys, p;
    volatile uint32_t *put = xbox_Nv2aRegPtr(0x800040), *get = xbox_Nv2aRegPtr(0x800044);
    if (off < 0) { const char *e = getenv("OOGIE_NO_UNSTICK"); off = e && *e == '1'; }
    if (off || !dev || !put || !get || !fence) return 0;
    sem_va = *(volatile uint32_t *)((uintptr_t)(dev + 0x30) + g_xbox_mem_offset);
    if (!sem_va) return 0;
    sem = *(volatile uint32_t *)((uintptr_t)sem_va + g_xbox_mem_offset);
    /* As KickOff (sub_001D76F0) chooses what to hand over: [dev+0] only in
     * D3D's normal state. With [dev+8] & 4 it hands over [dev+0x770] and
     * [dev+0] points into a command buffer being recorded; with & 0x2000 it
     * takes another path. Kicking [dev+0] then made the GPU replay old
     * commands for minutes (sk60e, sk712: GET cycling, D3D stuck at the
     * ring start). Only the normal state is touched. */
    if (OOGIE_MEM32(dev + 8) & 0x2004u) { hits = 0; return 0; }
    wp_phys = *(volatile uint32_t *)((uintptr_t)dev + g_xbox_mem_offset) & 0x0FFFFFFFu;
    p = *put & 0x0FFFFFFFu;
    if ((int32_t)(sem - fence) >= 0 || *get != *put) { hits = 0; idle_hits = 0; return 0; }
    if (wp_phys == p && nv2a_pb_dma_walk()) {
        /* Every command consumed and the fence still not reached: its
         * release was lost on a walk resync (#66, the owner's 2026-10-20
         * title-screen freeze). See oogie_gpu_unstick; after 300 ms in this
         * state, rather than its 2 s, so it costs a short pause. */
        hits = 0;
        if (++idle_hits < 6) return 0;
        idle_hits = 0;
        {
            uint32_t issued = OOGIE_MEM32(dev + 0x2C), v = fence;
            if ((int32_t)(issued - fence) > 0 && issued - fence < 0x400u)
                v = issued;
            if (n_lost++ < 40 || (n_lost & 255) == 0)
                fprintf(stderr, "[GPU] every command consumed, fence %08X lost: semaphore %08X set to %08X (%u so far)\n",
                        fence, sem, v, n_lost);
            *(volatile uint32_t *)((uintptr_t)sem_va + g_xbox_mem_offset) = v;
        }
        return 1;
    }
    idle_hits = 0;
    if (wp_phys <= p) { hits = 0; return 0; }
    if (++hits < 2) return 0;
    hits = 0;
    if (n++ < 40 || (n & 255) == 0)
        fprintf(stderr, "[GPU] fence %08X waited on with the GPU idle and commands not handed over: kickoff to %08X (%u so far)\n",
                fence, wp_phys, n);
    *put = wp_phys;
    return 1;
}

/* The GPU semaphore read 0 in the menus with fences in the thousands (bsA2:
 * "fence 00000B6B, semaphore 00000000"), so something wrote 0 over it. The
 * stall sampler calls this every 50 ms; a fall to 0 (or backwards by more
 * than a few fences) is logged, at most 10 times. */
int oogie_sem_watch(void)
{
    static uint32_t last;
    static unsigned n;
    uint32_t dev = *(volatile uint32_t *)((uintptr_t)OOGIE_D3D_DEVICE_PTR + g_xbox_mem_offset), sem_va, sem;
    if (!dev) return 0;
    sem_va = *(volatile uint32_t *)((uintptr_t)(dev + 0x30) + g_xbox_mem_offset);
    if (!sem_va) return 0;
    sem = *(volatile uint32_t *)((uintptr_t)sem_va + g_xbox_mem_offset);
    if (last > 0x100u && (int32_t)(sem - last) < -4) {
        if (n++ < 10) {
            extern void oogie_pb_history_dump(void);
            fprintf(stderr, "[GPU] semaphore at 0x%08X went back from %08X to %08X (fence wanted %08X)\n",
                    sem_va, last, sem, s_fence_wanted);
            if (n <= 2) oogie_pb_history_dump();
        }
        last = sem;
        return 1;
    }
    last = sem;
    return 0;
}

static OOGIE_TLS uint32_t t_fence_wanted;  /* this thread's fence wait (sub_001D7670) */
void sub_001D78D0_enter(void)
{
    static uint32_t s_sem;
    uint32_t dev = *(volatile uint32_t *)((uintptr_t)OOGIE_D3D_DEVICE_PTR + g_xbox_mem_offset);
    s_fence_wanted = *(volatile uint32_t *)((uintptr_t)(g_esp + 4) + g_xbox_mem_offset);
    t_fence_wanted = s_fence_wanted;
    if (!dev)
        return;
    uint32_t sem = *(volatile uint32_t *)((uintptr_t)(uint32_t)(dev + 0x30) + g_xbox_mem_offset);
    if (sem && sem != s_sem) {
        s_sem = sem;
        nv2a_pb_set_semaphore_target(sem);
        fprintf(stderr, "[D3D] GPU semaphore at 0x%08X (device 0x%08X)\n", sem, dev);
        fflush(stderr);
    }
}

/* ── Oogie: D3D GPU-position wait (sub_001D75F0) ───────────────
 *
 * sub_001D75F0(device, wait) is stock XDK 5849 D3D (verified by disassembly,
 * D3D.asm). With wait != 0 it spins on
 *     ((*(device+0x30)) << 2 ^ PGRAPH[0x400B10]) & 0x7C
 * i.e. until the fence bits the GPU latched from the last kickoff marker
 * (method 0x0310 on subchannel 5) match the low 5 bits of the semaphore.
 * The runtime only models that latch under RECOMP_PB_WALK=dma, so in the
 * default segment-scan mode it reads 0 and the spin ends only when the
 * semaphore is a multiple of 32. On real Windows (win_run1) the title froze
 * here on the Now Loading screen; under Wine it happened to get through.
 *
 * Fix: when asked to wait, let the executor consume everything submitted
 * (DMA_GET == DMA_PUT), then report what a GPU that had consumed everything
 * would latch: the semaphore's fence bits. The original body then runs and
 * its spin exits on the first pass.
 */
extern volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);

void sub_001D75F0_enter(void)
{
    static int s_logged;
    uint32_t dev  = *(volatile uint32_t *)((uintptr_t)(g_esp + 4) + g_xbox_mem_offset);
    uint32_t wait = *(volatile uint32_t *)((uintptr_t)(g_esp + 8) + g_xbox_mem_offset);
    volatile uint32_t *latch = xbox_Nv2aRegPtr(0x400B10);
    volatile uint32_t *put   = xbox_Nv2aRegPtr(0x800040);
    volatile uint32_t *get   = xbox_Nv2aRegPtr(0x800044);
    uint32_t sem_va, sem;
    DWORD t0;

    if (wait && s_logged == 0) {
        fprintf(stderr, "[D3D] GPU-position wait entered: dev=0x%08X latch=%p put=%p get=%p\n",
                dev, (void *)latch, (void *)put, (void *)get);
        fflush(stderr);
    }
    if (!wait || !dev || !latch || !put || !get)
        return;
    t0 = GetTickCount();
    while (*get != *put && GetTickCount() - t0 < 250)
        SwitchToThread();
    sem_va = *(volatile uint32_t *)((uintptr_t)(dev + 0x30) + g_xbox_mem_offset);
    if (!sem_va)
        return;
    sem = *(volatile uint32_t *)((uintptr_t)sem_va + g_xbox_mem_offset);
    *latch = (*latch & ~0x7Cu) | ((sem & 0x1Fu) << 2);
    if (s_logged < 3) {
        s_logged++;
        fprintf(stderr, "[D3D] GPU-position wait: sem=0x%08X GET=0x%08X PUT=0x%08X%s\n",
                sem, *get, *put, *get == *put ? "" : " (executor still behind)");
        fflush(stderr);
    }
}

/* ── Oogie: background job runner (sub_000584E0) ───────────────
 *
 * sub_00058540 starts a background job (with the Now Loading screen running
 * on the main thread meanwhile). Verified by disassembly:
 *   0x58587  job.state (0x3E3F64) = 0
 *   0x585C3  sub_001AD720 -> XAPI CreateThread(start 0x584E0, param 0x3E3F64,
 *            flags 4 = CREATE_SUSPENDED)
 *   0x585D1  job.func  (0x3E3F7C) = the job      <- after the thread exists
 *   0x585FC  sub_001AD3D0 -> ResumeThread
 *   0x58604  job.state = 2
 * The worker (sub_000584E0) runs job.func and then clears it; the loading loop
 * in sub_0003DF90 waits for job.func == 0. The runtime ignores
 * CREATE_SUSPENDED (kernel_bridge.c bridge_spawn_thread: CreateThread flags 0),
 * so on a fast PC the worker finished before 0x585D1, the main thread then
 * stored job.func, nobody cleared it, and the title sat on Now Loading for
 * ever (win_run2). Under Wine the main thread happened to win the race.
 *
 * Fix: the worker waits at entry until the creator has filled in job.func,
 * which CREATE_SUSPENDED guarantees on the Xbox. job.func is never 0 for a
 * started job (sub_00058540 returns early at 0x58552 when it would be).
 * Waiting on job.state instead (first version) was racy: sub_000586E0, run
 * every frame by other threads, sets state 1, which opened the gate before
 * job.func was stored; the worker then ran nothing and cleared the record,
 * and the title stayed on Now Loading (win_run11). Bounded so a wrong guess
 * cannot hang the worker.
 */
/* ── Oogie: a job loop that spins on "is it my turn" (#61) ──────
 *
 * sub_0004F4E0 loops while the state at 0x3E0DB4 is non-zero. The thread
 * named at 0x3E4698 runs the step; the one at 0x3E3F68 (sub_00057EA0 says
 * "that's me") loops straight back with no kernel call, waiting for the
 * other to finish. The console's scheduler would preempt it; under the guest
 * CPU lock (RECOMP_GUEST_LOCK=1) nothing could, and the other thread never
 * got the CPU (gy1: 15 s stall at boot, thread in sub_0004F4E0). Each call
 * now lets the CPU go for a moment, with a real sleep every 16th call so a
 * waiter on the lock wakes up. Without the lock this does nothing. */
void sub_00057EA0_enter(void)
{
    static OOGIE_TLS unsigned s_spin;
    if (!guest_cpu_held())
        return;
    oogie_host_yield((++s_spin & 15u) == 0 ? 1 : -1);
}

/* ── Oogie: the buffer on screen (KNOWN_ISSUES #51/#57) ───────
 * Verified (D3D.asm 0x1D8928..0x1D89A7): sub_001D8928(addr) is where D3D's
 * vblank handler (sub_001DB020 / sub_001DB140) points the CRTC at a new
 * frame buffer (`mov [regs + 0x600800], eax`, eax = addr in progressive
 * modes). The back end shows that buffer (d3d11_backend.c, be_flip). */
volatile uint32_t g_oogie_crtc_addr;
volatile LONG g_oogie_crtc_n;
void sub_001D8928_enter(void)
{
    g_oogie_crtc_addr = OOGIE_MEM32(g_esp + 4);
    InterlockedIncrement(&g_oogie_crtc_n);
}

/* ── Oogie: frame order between the task and the main thread (#57) ──
 * Per game thread: whether it is inside a task step (woken from its
 * semaphore, sub_001AA460, and not yet waiting on it again) and the last
 * frame it called D3D in. See sub_000586E0 below. */
#define OOGIE_FO_N 64
static struct { volatile DWORD tid; volatile LONG in_step; volatile long drew; volatile LONGLONG last_qpc; } s_fo[OOGIE_FO_N];
static LONGLONG s_fo_qpf;   /* QueryPerformanceFrequency */
static int oogie_fo_slot(void)
{
    static OOGIE_TLS int slot = -1;
    DWORD me = GetCurrentThreadId();
    int i;
    if (slot >= 0 && s_fo[slot].tid == me) return slot;
    for (i = 0; i < OOGIE_FO_N; i++)
        if (s_fo[i].tid == me || (s_fo[i].tid == 0 && InterlockedCompareExchange((volatile LONG *)&s_fo[i].tid, (LONG)me, 0) == 0))
            return slot = i;
    return -1;
}

/* BEGIN generated: D3D entry lock (KNOWN_ISSUES #66). Regenerate with
 * the scratchpad script d3dlock.py; do not edit by hand. */
/* ── Oogie: one game thread inside D3D at a time (#66) ──────────
 *
 * Verified (fn1, fd3, rc3 histories, 2026-10-21): the command ring went out
 * of step where two D3D writers overlapped: a complete SetTexture command
 * (0x00081B00 + 2 values) and then four floats (1.0, 1.0, 16777215.0, 1.0)
 * whose own header had been overwritten by the texture command's last value;
 * in fd3, D3D_SetScissors' 0x402E0 replaced by another command's words. D3D
 * reserves ring space from one write pointer ([dev+0]) and is not
 * thread-safe; on the console the game's scheduler and one CPU kept two
 * threads from being inside it at once (the render step, Swap from the job
 * steps, BlockOnResource from the loaders). Every D3D function the game or
 * D3DX calls directly (69, from build_out/disasm) is wrapped here: the first
 * entry on a game thread takes a recursive lock, the last exit lets it go.
 * Not on host threads (D3D's DPC and vblank handler), which must never wait
 * behind a game thread that may be waiting for them. A thread waits at most
 * 200 ms and then goes in without the lock (logged), so a wait cycle costs a
 * hitch, never a hang. OOGIE_D3D_LOCK=0 turns it off. */
static OOGIE_TLS int s_is_game_thread;   /* defined below (sub_001BD404_enter) */
static CRITICAL_SECTION s_d3d_cs;
static INIT_ONCE s_d3d_once = INIT_ONCE_STATIC_INIT;
static int s_d3d_off = -1;
static OOGIE_TLS int s_d3d_depth, s_d3d_locked;
static BOOL CALLBACK oogie_d3d_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    const char *e = getenv("OOGIE_D3D_LOCK");
    (void)o; (void)p; (void)c;
    InitializeCriticalSectionAndSpinCount(&s_d3d_cs, 2000);
    s_d3d_off = e && *e == '0';
    return TRUE;
}
/* Who calls D3D, frame by frame (#57 menus): every outermost D3D entry on a
 * game thread is kept in a ring (frame = flips so far, host thread, D3D
 * function, caller). The back end's flicker detector calls
 * oogie_d3d_trace_dump() for its first reports: the entries of the last three
 * frames, grouped per thread in order, so a menu task drawing into the next
 * frame shows up as such. */
extern volatile long g_oogie_presents;
#define OOGIE_DT_N 8192
static struct { long frame; DWORD tid; uint32_t fn, from; } s_dt[OOGIE_DT_N];
static volatile LONG s_dt_n;
static void oogie_d3d_trace(uint32_t fn)
{
    LONG i = InterlockedIncrement(&s_dt_n) - 1;
    i &= OOGIE_DT_N - 1;
    s_dt[i].frame = g_oogie_presents;
    s_dt[i].tid = GetCurrentThreadId();
    s_dt[i].fn = fn;
    s_dt[i].from = OOGIE_MEM32(g_esp);
}
void oogie_d3d_trace_dump(void)
{
    long last = g_oogie_presents, f;
    LONG end = s_dt_n, start = end > OOGIE_DT_N ? end - OOGIE_DT_N : 0, i;
    fprintf(stderr, "[D3DTRACE] D3D calls by game threads, frames %ld..%ld:\n", last - 2, last);
    for (f = last - 2; f <= last; f++) {
        DWORD tids[8]; int nt = 0, t;
        for (i = start; i < end; i++) {
            int k = i & (OOGIE_DT_N - 1), found = 0;
            if (s_dt[k].frame != f) continue;
            for (t = 0; t < nt; t++) if (tids[t] == s_dt[k].tid) found = 1;
            if (!found && nt < 8) tids[nt++] = s_dt[k].tid;
        }
        for (t = 0; t < nt; t++) {
            char line[1400]; size_t len = 0; uint32_t lastfn = 0, lastfrom = 0; int run = 0;
            len += (size_t)snprintf(line + len, sizeof line - len, "[D3DTRACE] f=%ld host %lu:", f, (unsigned long)tids[t]);
            for (i = start; i < end && len < sizeof line - 40; i++) {
                int k = i & (OOGIE_DT_N - 1);
                if (s_dt[k].frame != f || s_dt[k].tid != tids[t]) continue;
                if (s_dt[k].fn == lastfn && s_dt[k].from == lastfrom) { run++; continue; }
                if (run > 1) len += (size_t)snprintf(line + len, sizeof line - len, "*%d", run);
                len += (size_t)snprintf(line + len, sizeof line - len, " %X<%X", s_dt[k].fn, s_dt[k].from);
                lastfn = s_dt[k].fn; lastfrom = s_dt[k].from; run = 1;
            }
            if (run > 1) len += (size_t)snprintf(line + len, sizeof line - len, "*%d", run);
            fprintf(stderr, "%s\n", line);
        }
    }
    fflush(stderr);
}
static void oogie_d3d_enter(uint32_t fn)
{
    static volatile LONG n_late;
    DWORD t0;
    if (s_d3d_depth++ > 0) return;
    if (s_is_game_thread) {
        extern volatile long g_oogie_presents;
        int fo = oogie_fo_slot();
        oogie_d3d_trace(fn);
        if (fo >= 0) {
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            s_fo[fo].drew = g_oogie_presents;
            s_fo[fo].last_qpc = q.QuadPart;
        }
    }
    InitOnceExecuteOnce(&s_d3d_once, oogie_d3d_init, NULL, NULL);
    if (s_d3d_off || !s_is_game_thread) return;
    if (TryEnterCriticalSection(&s_d3d_cs)) { s_d3d_locked = 1; return; }
    t0 = GetTickCount();
    {
        /* Another game thread is inside D3D right now: the case the lock
         * exists for. The first few are logged. */
        static volatile LONG n_busy;
        LONG k = InterlockedIncrement(&n_busy);
        if (k <= 8 || (k & 4095) == 0) {
            fprintf(stderr, "[D3D] lock: another game thread is in D3D (host %lu holds it); this one waits (from 0x%08X, %ld so far)\n",
                    (unsigned long)(DWORD)(uintptr_t)s_d3d_cs.OwningThread, OOGIE_MEM32(g_esp), (long)k);
            fflush(stderr);
        }
    }
    while (GetTickCount() - t0 < 200) {
        oogie_host_yield(GetTickCount() - t0 < 2 ? -1 : 1);
        if (TryEnterCriticalSection(&s_d3d_cs)) { s_d3d_locked = 1; return; }
    }
    if (InterlockedIncrement(&n_late) <= 10 || (n_late & 1023) == 0) {
        fprintf(stderr, "[D3D] lock: waited 200 ms, going in without it (from 0x%08X, %ld so far)\n",
                OOGIE_MEM32(g_esp), (long)n_late);
        fflush(stderr);
    }
}
static void oogie_d3d_leave(void)
{
    if (--s_d3d_depth == 0 && s_d3d_locked) {
        s_d3d_locked = 0;
        LeaveCriticalSection(&s_d3d_cs);
    }
}
extern void sub_001D1B80_gen(void);
extern void sub_001D1BD0_gen(void);
extern void sub_001D1C10_gen(void);
extern void sub_001D1C30_gen(void);
extern void sub_001D2330_gen(void);
extern void sub_001D2470_gen(void);
extern void sub_001D2490_gen(void);
extern void sub_001D24F0_gen(void);
extern void sub_001D2680_gen(void);
extern void sub_001D26C0_gen(void);
extern void sub_001D2750_gen(void);
extern void sub_001D27B0_gen(void);
extern void sub_001D2850_gen(void);
extern void sub_001D2870_gen(void);
extern void sub_001D2AC0_gen(void);
extern void sub_001D2C30_gen(void);
extern void sub_001D2C40_gen(void);
extern void sub_001D2C60_gen(void);
extern void sub_001D2CF0_gen(void);
extern void sub_001D2E10_gen(void);
extern void sub_001D2E40_gen(void);
extern void sub_001D2E90_gen(void);
extern void sub_001D2EB0_gen(void);
extern void sub_001D2F00_gen(void);
extern void sub_001D2F40_gen(void);
extern void sub_001D3140_gen(void);
extern void sub_001D3210_gen(void);
extern void sub_001D3490_gen(void);
extern void sub_001D34E0_gen(void);
extern void sub_001D3810_gen(void);
extern void sub_001D3830_gen(void);
extern void sub_001D3850_gen(void);
extern void sub_001D3A80_gen(void);
extern void sub_001D3AB0_gen(void);
extern void sub_001D3D70_gen(void);
extern void sub_001D3D80_gen(void);
extern void sub_001D4080_gen(void);
extern void sub_001D4350_gen(void);
extern void sub_001D46D0_gen(void);
extern void sub_001D4720_gen(void);
extern void sub_001D4BC0_gen(void);
extern void sub_001D55A0_gen(void);
extern void sub_001D5630_gen(void);
extern void sub_001D56D0_gen(void);
extern void sub_001D5850_gen(void);
extern void sub_001D5910_gen(void);
extern void sub_001D5AC0_gen(void);
extern void sub_001D5C30_gen(void);
extern void sub_001D5F10_gen(void);
extern void sub_001D5F50_gen(void);
extern void sub_001D6030_gen(void);
extern void sub_001D6060_gen(void);
extern void sub_001D60E0_gen(void);
extern void sub_001D60F0_gen(void);
extern void sub_001D61A0_gen(void);
extern void sub_001D61B0_gen(void);
extern void sub_001D61C0_gen(void);
extern void sub_001D6210_gen(void);
extern void sub_001D6D20_gen(void);
extern void sub_001D6ED0_gen(void);
extern void sub_001D7310_gen(void);
extern void sub_001D76A0_gen(void);
extern void sub_001D7D50_gen(void);
extern void sub_001D7D70_gen(void);
extern void sub_001D8780_gen(void);
extern void sub_001D87A0_gen(void);
extern void sub_001D8850_gen(void);
extern void sub_001D88A0_gen(void);
extern void sub_001D8920_gen(void);
void sub_001D1B80(void) { oogie_d3d_enter(0x001D1B80u); sub_001D1B80_gen(); oogie_d3d_leave(); }
void sub_001D1BD0(void) { oogie_d3d_enter(0x001D1BD0u); sub_001D1BD0_gen(); oogie_d3d_leave(); }
void sub_001D1C10(void) { oogie_d3d_enter(0x001D1C10u); sub_001D1C10_gen(); oogie_d3d_leave(); }
void sub_001D1C30(void) { oogie_d3d_enter(0x001D1C30u); sub_001D1C30_gen(); oogie_d3d_leave(); }
void sub_001D2330(void) { oogie_d3d_enter(0x001D2330u); sub_001D2330_gen(); oogie_d3d_leave(); }
void sub_001D2470(void) { oogie_d3d_enter(0x001D2470u); sub_001D2470_gen(); oogie_d3d_leave(); }
void sub_001D2490(void) { oogie_d3d_enter(0x001D2490u); sub_001D2490_gen(); oogie_d3d_leave(); }
void sub_001D24F0(void) { oogie_d3d_enter(0x001D24F0u); sub_001D24F0_gen(); oogie_d3d_leave(); }
void sub_001D2680(void) { oogie_d3d_enter(0x001D2680u); sub_001D2680_gen(); oogie_d3d_leave(); }
void sub_001D26C0(void) { oogie_d3d_enter(0x001D26C0u); sub_001D26C0_gen(); oogie_d3d_leave(); }
void sub_001D2750(void) { oogie_d3d_enter(0x001D2750u); sub_001D2750_gen(); oogie_d3d_leave(); }
void sub_001D27B0(void) { oogie_d3d_enter(0x001D27B0u); sub_001D27B0_gen(); oogie_d3d_leave(); }
void sub_001D2850(void) { oogie_d3d_enter(0x001D2850u); sub_001D2850_gen(); oogie_d3d_leave(); }
void sub_001D2870(void) { oogie_d3d_enter(0x001D2870u); sub_001D2870_gen(); oogie_d3d_leave(); }
void sub_001D2AC0(void) { oogie_d3d_enter(0x001D2AC0u); sub_001D2AC0_gen(); oogie_d3d_leave(); }
void sub_001D2C30(void) { oogie_d3d_enter(0x001D2C30u); sub_001D2C30_gen(); oogie_d3d_leave(); }
void sub_001D2C40(void) { oogie_d3d_enter(0x001D2C40u); sub_001D2C40_gen(); oogie_d3d_leave(); }
void sub_001D2C60(void) { oogie_d3d_enter(0x001D2C60u); sub_001D2C60_gen(); oogie_d3d_leave(); }
void sub_001D2CF0(void) { oogie_d3d_enter(0x001D2CF0u); sub_001D2CF0_gen(); oogie_d3d_leave(); }
void sub_001D2E10(void) { oogie_d3d_enter(0x001D2E10u); sub_001D2E10_gen(); oogie_d3d_leave(); }
void sub_001D2E40(void) { oogie_d3d_enter(0x001D2E40u); sub_001D2E40_gen(); oogie_d3d_leave(); }
void sub_001D2E90(void) { oogie_d3d_enter(0x001D2E90u); sub_001D2E90_gen(); oogie_d3d_leave(); }
void sub_001D2EB0(void) { oogie_d3d_enter(0x001D2EB0u); sub_001D2EB0_gen(); oogie_d3d_leave(); }
void sub_001D2F00(void) { oogie_d3d_enter(0x001D2F00u); sub_001D2F00_gen(); oogie_d3d_leave(); }
void sub_001D2F40(void) { oogie_d3d_enter(0x001D2F40u); sub_001D2F40_gen(); oogie_d3d_leave(); }
void sub_001D3140(void) { oogie_d3d_enter(0x001D3140u); sub_001D3140_gen(); oogie_d3d_leave(); }
void sub_001D3210(void) { oogie_d3d_enter(0x001D3210u); sub_001D3210_gen(); oogie_d3d_leave(); }
void sub_001D3490(void) { oogie_d3d_enter(0x001D3490u); sub_001D3490_gen(); oogie_d3d_leave(); }
void sub_001D34E0(void) { oogie_d3d_enter(0x001D34E0u); sub_001D34E0_gen(); oogie_d3d_leave(); }
void sub_001D3810(void) { oogie_d3d_enter(0x001D3810u); sub_001D3810_gen(); oogie_d3d_leave(); }
void sub_001D3830(void) { oogie_d3d_enter(0x001D3830u); sub_001D3830_gen(); oogie_d3d_leave(); }
void sub_001D3850(void) { oogie_d3d_enter(0x001D3850u); sub_001D3850_gen(); oogie_d3d_leave(); }
void sub_001D3A80(void) { oogie_d3d_enter(0x001D3A80u); sub_001D3A80_gen(); oogie_d3d_leave(); }
void sub_001D3AB0(void) { oogie_d3d_enter(0x001D3AB0u); sub_001D3AB0_gen(); oogie_d3d_leave(); }
void sub_001D3D70(void) { oogie_d3d_enter(0x001D3D70u); sub_001D3D70_gen(); oogie_d3d_leave(); }
void sub_001D3D80(void) { oogie_d3d_enter(0x001D3D80u); sub_001D3D80_gen(); oogie_d3d_leave(); }
void sub_001D4080(void) { oogie_d3d_enter(0x001D4080u); sub_001D4080_gen(); oogie_d3d_leave(); }
void sub_001D4350(void) { oogie_d3d_enter(0x001D4350u); sub_001D4350_gen(); oogie_d3d_leave(); }
void sub_001D46D0(void) { oogie_d3d_enter(0x001D46D0u); sub_001D46D0_gen(); oogie_d3d_leave(); }
void sub_001D4720(void) { oogie_d3d_enter(0x001D4720u); sub_001D4720_gen(); oogie_d3d_leave(); }
void sub_001D4BC0(void) { oogie_d3d_enter(0x001D4BC0u); sub_001D4BC0_gen(); oogie_d3d_leave(); }
void sub_001D55A0(void) { oogie_d3d_enter(0x001D55A0u); sub_001D55A0_gen(); oogie_d3d_leave(); }
void sub_001D5630(void) { oogie_d3d_enter(0x001D5630u); sub_001D5630_gen(); oogie_d3d_leave(); }
void sub_001D56D0(void) { oogie_d3d_enter(0x001D56D0u); sub_001D56D0_gen(); oogie_d3d_leave(); }
void sub_001D5850(void) { oogie_d3d_enter(0x001D5850u); sub_001D5850_gen(); oogie_d3d_leave(); }
void sub_001D5910(void) { oogie_d3d_enter(0x001D5910u); sub_001D5910_gen(); oogie_d3d_leave(); }
void sub_001D5AC0(void) { oogie_d3d_enter(0x001D5AC0u); sub_001D5AC0_gen(); oogie_d3d_leave(); }
void sub_001D5C30(void) { oogie_d3d_enter(0x001D5C30u); sub_001D5C30_gen(); oogie_d3d_leave(); }
void sub_001D5F10(void) { oogie_d3d_enter(0x001D5F10u); sub_001D5F10_gen(); oogie_d3d_leave(); }
void sub_001D5F50(void) { oogie_d3d_enter(0x001D5F50u); sub_001D5F50_gen(); oogie_d3d_leave(); }
void sub_001D6030(void) { oogie_d3d_enter(0x001D6030u); sub_001D6030_gen(); oogie_d3d_leave(); }
void sub_001D6060(void) { oogie_d3d_enter(0x001D6060u); sub_001D6060_gen(); oogie_d3d_leave(); }
void sub_001D60E0(void) { oogie_d3d_enter(0x001D60E0u); sub_001D60E0_gen(); oogie_d3d_leave(); }
void sub_001D60F0(void) { oogie_d3d_enter(0x001D60F0u); sub_001D60F0_gen(); oogie_d3d_leave(); }
void sub_001D61A0(void) { oogie_d3d_enter(0x001D61A0u); sub_001D61A0_gen(); oogie_d3d_leave(); }
void sub_001D61B0(void) { oogie_d3d_enter(0x001D61B0u); sub_001D61B0_gen(); oogie_d3d_leave(); }
void sub_001D61C0(void) { oogie_d3d_enter(0x001D61C0u); sub_001D61C0_gen(); oogie_d3d_leave(); }
void sub_001D6210(void) { oogie_d3d_enter(0x001D6210u); sub_001D6210_gen(); oogie_d3d_leave(); }
void sub_001D6D20(void) { oogie_d3d_enter(0x001D6D20u); sub_001D6D20_gen(); oogie_d3d_leave(); }
void sub_001D6ED0(void) { oogie_d3d_enter(0x001D6ED0u); sub_001D6ED0_gen(); oogie_d3d_leave(); }
void sub_001D7310(void) { oogie_d3d_enter(0x001D7310u); sub_001D7310_gen(); oogie_d3d_leave(); }
void sub_001D76A0(void) { oogie_d3d_enter(0x001D76A0u); sub_001D76A0_gen(); oogie_d3d_leave(); }
void sub_001D7D50(void) { oogie_d3d_enter(0x001D7D50u); sub_001D7D50_gen(); oogie_d3d_leave(); }
void sub_001D7D70(void) { oogie_d3d_enter(0x001D7D70u); sub_001D7D70_gen(); oogie_d3d_leave(); }
void sub_001D8780(void) { oogie_d3d_enter(0x001D8780u); sub_001D8780_gen(); oogie_d3d_leave(); }
void sub_001D87A0(void) { oogie_d3d_enter(0x001D87A0u); sub_001D87A0_gen(); oogie_d3d_leave(); }
void sub_001D8850(void) { oogie_d3d_enter(0x001D8850u); sub_001D8850_gen(); oogie_d3d_leave(); }
void sub_001D88A0(void) { oogie_d3d_enter(0x001D88A0u); sub_001D88A0_gen(); oogie_d3d_leave(); }
void sub_001D8920(void) { oogie_d3d_enter(0x001D8920u); sub_001D8920_gen(); oogie_d3d_leave(); }
/* END generated: D3D entry lock */

/* ── Oogie: wait tracing (#61, diagnostics) ─────────────────────
 *
 * Under the guest CPU lock boot ended with every game thread in a kernel
 * wait (gz1), and the stall dump cannot say on what. XAPI's waits and
 * signals all pass through five functions (text.asm, verified):
 * WaitForSingleObjectEx sub_001BD012 (h, ms, alertable),
 * WaitForMultipleObjectsEx sub_001BD056 (n, handles, all, ms, alertable),
 * SetEvent sub_001BCEF9 (h), ReleaseSemaphore sub_001BCF74 (h, n, prev),
 * ReleaseMutex sub_001BCFF2 (h). Each thread's last wait and each handle's
 * last signal are kept; oogie_wait_dump() (pc_stall.c, at the long-stall
 * dumps) prints them. On with RECOMP_GUEST_LOCK=1 or OOGIE_WAIT_TRACE=1. */
#define OOGIE_WT_THREADS 64
#define OOGIE_WT_HANDLES 256
static struct { volatile DWORD tid; uint32_t h[4], n, ms, from, from2; DWORD tick; } s_wt[OOGIE_WT_THREADS];
static struct { volatile uint32_t h; char kind; uint32_t n, from, count; DWORD tid, tick; } s_ws[OOGIE_WT_HANDLES];
static int s_wt_on = -1;

static int oogie_wt_on(void)
{
    if (s_wt_on < 0) {
        const char *a = getenv("OOGIE_WAIT_TRACE"), *b = getenv("RECOMP_GUEST_LOCK");
        s_wt_on = (a && *a == '1') || (b && *b == '1');
    }
    return s_wt_on;
}
static int oogie_wt_slot(void)
{
    static OOGIE_TLS int slot = -1;
    DWORD me = GetCurrentThreadId();
    int i;
    if (slot >= 0 && s_wt[slot].tid == me) return slot;
    for (i = 0; i < OOGIE_WT_THREADS; i++)
        if (s_wt[i].tid == me || (s_wt[i].tid == 0 && InterlockedCompareExchange((volatile LONG *)&s_wt[i].tid, (LONG)me, 0) == 0))
            return slot = i;
    return -1;
}
static void oogie_wt_wait(uint32_t n, const uint32_t *h, uint32_t ms)
{
    int i = oogie_wt_slot(), k;
    if (i < 0) return;
    for (k = 0; k < 4; k++) s_wt[i].h[k] = k < (int)n ? h[k] : 0;
    s_wt[i].n = n; s_wt[i].ms = ms;
    s_wt[i].from = OOGIE_MEM32(g_esp);
    s_wt[i].from2 = 0;
    s_wt[i].tick = GetTickCount();
}
static void oogie_wt_signal(char kind, uint32_t h, uint32_t n)
{
    unsigned i = (h * 2654435761u) >> 24, k;
    for (k = 0; k < 16; k++, i = (i + 1) & (OOGIE_WT_HANDLES - 1)) {
        if (s_ws[i].h == h || (s_ws[i].h == 0 && InterlockedCompareExchange((volatile LONG *)&s_ws[i].h, (LONG)h, 0) == 0)) {
            s_ws[i].kind = kind; s_ws[i].n = n; s_ws[i].from = OOGIE_MEM32(g_esp);
            s_ws[i].tid = GetCurrentThreadId(); s_ws[i].tick = GetTickCount(); s_ws[i].count++;
            return;
        }
    }
}
void sub_001BD012_enter(void)
{
    uint32_t h;
    if (!oogie_wt_on()) return;
    h = OOGIE_MEM32(g_esp + 4);
    oogie_wt_wait(1, &h, OOGIE_MEM32(g_esp + 8));
}
void sub_001BD056_enter(void)
{
    uint32_t n, arr, h[4], k;
    if (!oogie_wt_on()) return;
    n = OOGIE_MEM32(g_esp + 4); arr = OOGIE_MEM32(g_esp + 8);
    for (k = 0; k < 4; k++) h[k] = (k < n && arr) ? OOGIE_MEM32(arr + k * 4) : 0;
    oogie_wt_wait(n, h, OOGIE_MEM32(g_esp + 0x10));
}
void sub_001BCEF9_enter(void) { if (oogie_wt_on()) oogie_wt_signal('E', OOGIE_MEM32(g_esp + 4), 1); }
void sub_001BCF74_enter(void) { if (oogie_wt_on()) oogie_wt_signal('S', OOGIE_MEM32(g_esp + 4), OOGIE_MEM32(g_esp + 8)); }
void sub_001BCFF2_enter(void) { if (oogie_wt_on()) oogie_wt_signal('M', OOGIE_MEM32(g_esp + 4), 1); }

void oogie_wait_dump(void)
{
    DWORD now = GetTickCount();
    int i, k;
    if (!oogie_wt_on()) return;
    for (i = 0; i < OOGIE_WT_THREADS; i++) {
        if (!s_wt[i].tid) continue;
        fprintf(stderr, "[WAIT] host %lu: last wait %lu ms ago from 0x%08X, %u handle(s), timeout %d ms:",
                (unsigned long)s_wt[i].tid, (unsigned long)(now - s_wt[i].tick), s_wt[i].from, s_wt[i].n, (int)s_wt[i].ms);
        for (k = 0; k < 4 && k < (int)s_wt[i].n; k++) {
            uint32_t h = s_wt[i].h[k];
            unsigned j = (h * 2654435761u) >> 24, m;
            fprintf(stderr, " 0x%08X", h);
            for (m = 0; m < 16; m++, j = (j + 1) & (OOGIE_WT_HANDLES - 1))
                if (s_ws[j].h == h) {
                    fprintf(stderr, " (last %c by host %lu %lu ms ago from 0x%08X, %u times)", s_ws[j].kind,
                            (unsigned long)s_ws[j].tid, (unsigned long)(now - s_ws[j].tick), s_ws[j].from, s_ws[j].count);
                    break;
                }
            if (m == 16) fprintf(stderr, " (never signalled)");
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

void sub_000584E0_enter(void)
{
    static volatile LONG s_waits;
    uint32_t job = *(volatile uint32_t *)((uintptr_t)(g_esp + 4) + g_xbox_mem_offset);
    volatile uint32_t *func;
    DWORD t0 = GetTickCount();
    int waited = 0;

    if (job < 0x00010000u || job >= 0x04000000u)
        return;
    func = (volatile uint32_t *)((uintptr_t)(job + 0x18) + g_xbox_mem_offset);
    while (*func == 0 && GetTickCount() - t0 < 5000) {
        waited = 1;
        oogie_host_yield(1);   /* lets the guest CPU go (#61) */
    }
    if (waited && InterlockedIncrement(&s_waits) <= 5) {
        fprintf(stderr, "[OOGIE] job worker waited %lu ms for its job (job 0x%08X func 0x%08X)\n",
                (unsigned long)(GetTickCount() - t0), job, *func);
        fflush(stderr);
    }
}

/* ── Oogie: XAPI DuplicateHandle (sub_001BC71B) ────────────────
 *
 * DuplicateHandle(srcProc, src, tgtProc, &dst, access, inherit, options)
 * forwards to NtDuplicateObject(src, &dst, options) (verified, text.asm).
 * The job scheduler's init (sub_001AD260, 0x1AD284) duplicates the current
 * thread pseudo-handle (-2) with options 0 into its thread table at 0x40EDD8,
 * then suspends and resumes threads through that table. Xbox handles carry no
 * access mask, so options 0 is a fully usable handle there. The runtime maps
 * options 0 to a Win32 DuplicateHandle with access 0 and no SAME_ACCESS, i.e.
 * a handle with no rights: every SuspendThread/ResumeThread on it failed with
 * ERROR_ACCESS_DENIED (xbox_kernel.log, win_run3: 184k failures), the
 * scheduler could not park its threads, and a worker crashed on a stack full
 * of 0xFFFFFFFF.
 *
 * Fix: set the SAME_ACCESS bit (0x2, which the runtime maps to Win32
 * DUPLICATE_SAME_ACCESS) in the options argument before the body forwards it.
 * Upstream bug, see KNOWN_ISSUES.md.
 */
void sub_001BC71B_enter(void)
{
    static int s_logged;
    volatile uint32_t *opts =
        (volatile uint32_t *)((uintptr_t)(g_esp + 0x1C) + g_xbox_mem_offset);
    if (!(*opts & 0x2u)) {
        if (s_logged++ < 3) {
            fprintf(stderr, "[OOGIE] DuplicateHandle(0x%08X) options 0x%X -> +SAME_ACCESS\n",
                    *(volatile uint32_t *)((uintptr_t)(g_esp + 8) + g_xbox_mem_offset), *opts);
            fflush(stderr);
        }
        *opts |= 0x2u;
    }
}

/* ── Oogie: thread IDs for guest threads ───────────────────────
 *
 * XAPI CreateThread (sub_001BD49C) returns the new thread's ID through
 * lpThreadId, and XAPI GetCurrentThreadId (sub_001BD50A) reads
 * [fs:[0x28] + 0x12C] (verified, text.asm). The job scheduler keys its thread
 * table at 0x40EDD8 on these IDs: sub_001AD720 stores the ID CreateThread gave
 * it, and sub_001AD4B0 / sub_001AD4F0 find the handle to suspend or resume by
 * ID. The runtime never writes PsCreateSystemThreadEx's ThreadId argument and
 * gives each worker a zero-filled thread object, so every worker's ID read 0
 * and the stored IDs were stack garbage: the scheduler suspended through the
 * wrong table slot (win_run4: 16 M failed NtSuspendThread calls).
 *
 * Fix: on CreateThread entry, pick a unique ID, write it to *lpThreadId (the
 * runtime leaves it alone) and queue it under (start, param). The new thread
 * enters XAPI's thread startup sub_001BD404(start, param) first; there it takes
 * its ID from the queue and stores it at [fs:[0x28] + 0x12C].
 */
extern OOGIE_TLS uint32_t g_fs_base;

#define OOGIE_TID_PENDING 64
static struct { uint32_t start, param, id; } s_tid_pend[OOGIE_TID_PENDING];
static volatile LONG s_tid_lock;
static volatile LONG s_tid_next = 0x7E000000;

#define OOGIE_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* This host thread's guest ID, set in sub_001BD404_enter. Writing it to
 * [fs:[0x28]+0x12C] alone was not enough: win_run7 showed workers reading 0
 * there later (the runtime gives a worker its own thread object only in some
 * cases), so the game-logic thread 0x0019D8E0 was not found in the scheduler
 * table and stopped itself. GetCurrentThreadId is overridden below to return
 * this value. */
static OOGIE_TLS uint32_t s_my_guest_tid;
static OOGIE_TLS int s_is_game_thread;   /* set at sub_001BD404 entry: every game thread, main included */

static void tid_lock(void)   { while (InterlockedCompareExchange(&s_tid_lock, 1, 0)) SwitchToThread(); }
static void tid_unlock(void) { InterlockedExchange(&s_tid_lock, 0); }

void sub_001BD49C_enter(void)
{
    uint32_t start = OOGIE_MEM32(g_esp + 0x0C);
    uint32_t param = OOGIE_MEM32(g_esp + 0x10);
    uint32_t lptid = OOGIE_MEM32(g_esp + 0x18);
    uint32_t id = (uint32_t)InterlockedIncrement(&s_tid_next);
    int i;

    if (lptid >= 0x00010000u && lptid < 0x04000000u)
        OOGIE_MEM32(lptid) = id;
    tid_lock();
    for (i = 0; i < OOGIE_TID_PENDING; i++)
        if (!s_tid_pend[i].id) {
            s_tid_pend[i].start = start;
            s_tid_pend[i].param = param;
            s_tid_pend[i].id = id;
            break;
        }
    tid_unlock();
}

/* ── Oogie diagnostics: per-thread stack sampler ───────────────
 *
 * The runtime watchdog samples only the main thread. With OOGIE_SAMPLE_SECS=N,
 * every guest thread registers here when it enters XAPI's thread startup
 * (sub_001BD404, the main thread included), and a host thread prints each live
 * thread's guest esp and the return-address-like words on its stack every N
 * seconds. Reads another thread's TLS through a pointer taken on that thread;
 * the host thread handle is checked first so exited threads are skipped.
 */
#define OOGIE_SAMPLE_MAX 64
static struct {
    volatile uint32_t *esp;
    HANDLE th;
    uint32_t start, param;
    DWORD tid;
} s_smp[OOGIE_SAMPLE_MAX];
static volatile LONG s_smp_n;

static DWORD WINAPI oogie_sampler(LPVOID p)
{
    DWORD ms = (DWORD)(uintptr_t)p;
    for (;;) {
        LONG n, i;
        Sleep(ms);
        n = s_smp_n;
        fprintf(stderr, "[SAMPLE] %ld guest threads\n", (long)n);
        for (i = 0; i < n && i < OOGIE_SAMPLE_MAX; i++) {
            uint32_t sp;
            int k, shown = 0;
            if (!s_smp[i].esp || !s_smp[i].th || WaitForSingleObject(s_smp[i].th, 0) == WAIT_OBJECT_0)
                continue;
            sp = *s_smp[i].esp;
            fprintf(stderr, "[SAMPLE] #%ld host %lu start 0x%08X param 0x%08X esp 0x%08X:",
                    (long)i, (unsigned long)s_smp[i].tid, s_smp[i].start, s_smp[i].param, sp);
            for (k = 0; k < 1024 && shown < 24; k++) {
                uint32_t a = sp + (uint32_t)k * 4, w;
                if (a < 0x00010000u || a >= 0x04000000u)
                    break;
                w = OOGIE_MEM32(a);
                if (w >= 0x00011000u && w < 0x0025F8E0u) {
                    fprintf(stderr, " %08X", w);
                    shown++;
                }
            }
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }
    return 0;
}

static void oogie_sample_register(uint32_t start, uint32_t param)
{
    static volatile LONG s_started;
    const char *e = getenv("OOGIE_SAMPLE_SECS");
    LONG i;
    if (!e || atoi(e) <= 0)
        return;
    i = InterlockedIncrement(&s_smp_n) - 1;
    if (i >= OOGIE_SAMPLE_MAX)
        return;
    s_smp[i].start = start;
    s_smp[i].param = param;
    s_smp[i].tid = GetCurrentThreadId();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &s_smp[i].th, 0, FALSE, DUPLICATE_SAME_ACCESS);
    s_smp[i].esp = &g_esp;
    if (!InterlockedExchange(&s_started, 1)) {
        HANDLE h = CreateThread(NULL, 0, oogie_sampler,
                                (LPVOID)(uintptr_t)(atoi(e) * 1000), 0, NULL);
        if (h) CloseHandle(h);
    }
}

/* ── Oogie: one CPU for the game's threads (KNOWN_ISSUES #61) ──────────
 * The Xbox has a single CPU: the game's threads take turns and never run at
 * the same instant. Here they ran in parallel on several cores, and the races
 * a single CPU never shows came out one by one (#48 DirectSound voices, #50
 * the semaphore table, and #61: a game task looping for minutes in
 * sub_001B24E0, drawing without end, Hypothesis a list damaged by two threads
 * editing it at once; sk58d, sk60b, sk60e). Every game thread starts here, so
 * each is pinned to the same logical processor (the highest-numbered one);
 * Windows then time-slices them as the console did. The emulator's own
 * threads (GPU, audio, renderer, log) stay free. OOGIE_GUEST_CORES=<n> allows
 * n processors, 0 turns this off. */
static void oogie_pin_guest_thread(void)
{
    static DWORD_PTR mask = (DWORD_PTR)-1;
    if (mask == (DWORD_PTR)-1) {
        const char *e = getenv("OOGIE_GUEST_CORES");
        int n = e && *e ? atoi(e) : 0;      /* off by default: see sk621/sk622 */
        DWORD_PTR proc = 0, sys = 0;
        mask = 0;
        if (n > 0 && GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc) {
            int b;
            for (b = (int)(sizeof(DWORD_PTR) * 8) - 1; b >= 0 && n > 0; b--)
                if (proc & ((DWORD_PTR)1 << b)) { mask |= (DWORD_PTR)1 << b; n--; }
            if (mask == proc) mask = 0;          /* that is every processor: nothing to do */
        }
        fprintf(stderr, "[OOGIE] game threads on processor mask 0x%llX%s\n",
                (unsigned long long)mask, mask ? "" : " (not pinned)");
    }
    if (mask) {
        s_guests_pinned_flag = 1;
        SetThreadAffinityMask(GetCurrentThread(), mask);
    }
}

void sub_001BD404_enter(void)
{
    uint32_t start = OOGIE_MEM32(g_esp + 4);
    uint32_t param = OOGIE_MEM32(g_esp + 8);
    uint32_t id = 0, kthread;
    int i;

    s_is_game_thread = 1;
    oogie_pin_guest_thread();

    oogie_sample_register(start, param);

    tid_lock();
    for (i = 0; i < OOGIE_TID_PENDING; i++)
        if (s_tid_pend[i].id && s_tid_pend[i].start == start && s_tid_pend[i].param == param) {
            id = s_tid_pend[i].id;
            s_tid_pend[i].id = 0;
            break;
        }
    tid_unlock();
    fprintf(stderr, "[OOGIE] thread start 0x%08X param 0x%08X host %lu -> guest tid 0x%08X%s\n",
            start, param, (unsigned long)GetCurrentThreadId(), id, id ? "" : " (no CreateThread match)");
    fflush(stderr);
    if (!id)
        return;
    s_my_guest_tid = id;
    if (!g_fs_base)
        return;
    kthread = OOGIE_MEM32(g_fs_base + 0x28);
    if (kthread >= 0x00010000u && kthread < 0x04000000u)
        OOGIE_MEM32(kthread + 0x12C) = id;
}

/* ── Oogie: one render step at a time (KNOWN_ISSUES #61) ───────────────
 *
 * Verified (text.asm 0x7D1B0-0x7D227, straight-line code): sub_0007D1B0 runs
 * one render step: sub_000849D0 (which rebuilds the render lists through
 * sub_001B2490 and walks them in sub_001B24E0), then sub_00039920 (return
 * address 0x0007D212) and sub_00012300. It is called from two game tasks
 * (sub_0019CE60, sub_0019E990) and sub_0004AC90. When two threads ran it at
 * once, one rebuilt the global lists at 0x413060 while the other walked
 * them, and the walk could go on for minutes (sk58d, sk60b/e, bsC1/bsC2;
 * Hypothesis until a soak with the fast log shows no such freeze). On the
 * single-CPU console that overlap does not happen in practice. So the step
 * takes a lock at its entry and gives it back at the entry of its call to
 * sub_00039920 (recognised by its return address). OOGIE_RENDER_LOCK=0
 * turns this off. */
static CRITICAL_SECTION s_rstep_cs;
static volatile LONG s_rstep_state;            /* 0 none, 1 initialising, 2 ready */
static OOGIE_TLS int s_rstep_held;
static int s_rstep_on = -1;

void sub_0007D1B0_enter(void)
{
    if (s_rstep_on < 0) { const char *e = getenv("OOGIE_RENDER_LOCK"); s_rstep_on = !(e && *e == '0'); }
    if (!s_rstep_on || s_rstep_held) return;
    if (s_rstep_state != 2) {
        if (InterlockedCompareExchange(&s_rstep_state, 1, 0) == 0) {
            InitializeCriticalSectionAndSpinCount(&s_rstep_cs, 2000);
            InterlockedExchange(&s_rstep_state, 2);
        }
        while (s_rstep_state != 2) SwitchToThread();
    }
    oogie_cs_enter(&s_rstep_cs);
    s_rstep_held = 1;
}

void sub_00039920_enter(void)
{
    if (s_rstep_held && OOGIE_MEM32(g_esp) == 0x0007D212u) {
        s_rstep_held = 0;
        LeaveCriticalSection(&s_rstep_cs);
    }
}

/* ── Oogie: the render-list walk (sub_001B24E0), entry hook (#61) ──
 *
 * Verified (text.asm 0x1B24E0-0x1B25AB): the walk goes through 51 buckets
 * [0x413060 + 4*i]; each is a list of objects (next at +0xA4), and each
 * object has two lists of draw callbacks (+0xA8 and +0xAC, next at +0). If
 * one of these lists links back on itself the walk never ends, drawing as it
 * goes: the long no-frame stalls in sk58d, sk60b, sk60e (game thread
 * waiting for the task, the task in sub_001B24E0). sub_001B2270 appends
 * objects without clearing their own next pointer, so a list rebuilt while
 * another thread walks it can do that (Hypothesis).
 * The walk itself stays the original; before it runs, this hook follows the
 * lists (reads only, no calls) and cuts the link that closes a loop, if any.
 * A replacement of the whole walk crashed in its callbacks (sk641-sk681) for
 * a reason not found, so the original is kept. OOGIE_RLIST_CHECK=0 turns
 * this off. */
#define RLIST_SEEN_MAX 4096

/* Follow a list from head (next at +next_off); if it loops, cut the link
 * that closes the loop. Returns the entries seen (bounded). */
static int rlist_ok(uint32_t p)
{
    return !(p < 0x10000u || (p >= 0x04000000u && (p < 0x80000000u || p >= 0x84000000u)));
}

static uint32_t rlist_check(uint32_t head, uint32_t next_off, const char *what)
{
    static uint32_t seen[RLIST_SEEN_MAX];
    static unsigned n_cut;
    uint32_t n = 0, p = head, q, i, slow = head, fast = head;
    /* Floyd: a loop is found in linear time with no memory; only then is the
     * closing link searched for. */
    for (;;) {
        if (!fast || !rlist_ok(fast)) return 0;
        fast = OOGIE_MEM32(fast + next_off);
        if (!fast || !rlist_ok(fast)) return 0;
        fast = OOGIE_MEM32(fast + next_off);
        slow = OOGIE_MEM32(slow + next_off);
        if (fast == slow) break;            /* a loop */
        if (++n > 1000000u) return 0;
    }
    n = 0;
    while (p && n < RLIST_SEEN_MAX) {
        if (p < 0x10000u || (p >= 0x04000000u && (p < 0x80000000u || p >= 0x84000000u)))
            return n;                           /* not guest memory: leave it to the game */
        seen[n++] = p;
        q = OOGIE_MEM32(p + next_off);
        for (i = 0; i < n; i++)
            if (seen[i] == q) {
                OOGIE_MEM32(p + next_off) = 0;
                if (n_cut++ < 20)
                    fprintf(stderr, "[OOGIE] render list (%s) looped back at 0x%08X after %u entries: link cut (%u so far)\n",
                            what, p, n, n_cut);
                return n;
            }
        p = q;
    }
    return n;
}

void sub_001B24E0_enter(void)
{
    static int on = -1;
    uint32_t slot, obj, k;
    if (on < 0) { const char *e = getenv("OOGIE_RLIST_CHECK"); on = e && *e == '1'; }   /* off by default: it cut lists still being built (sk69) */
    if (!on) return;
    for (slot = 0; slot < 0xCC; slot += 4) {
        uint32_t head = OOGIE_MEM32(0x00413060u + slot);
        if (!head) continue;
        rlist_check(head, 0xA4, "objects");
        for (obj = head, k = 0; obj && k < RLIST_SEEN_MAX; obj = OOGIE_MEM32(obj + 0xA4), k++) {
            if (obj < 0x10000u || (obj >= 0x04000000u && (obj < 0x80000000u || obj >= 0x84000000u)))
                break;
            if (OOGIE_MEM32(obj + 0xA8)) rlist_check(OOGIE_MEM32(obj + 0xA8), 0, "+A8");
            if (OOGIE_MEM32(obj + 0xAC)) rlist_check(OOGIE_MEM32(obj + 0xAC), 0, "+AC");
        }
    }
}

/* ── Oogie: XAPI GetCurrentThreadId (sub_001BD50A), full override ──
 * Original: mov eax, fs:[0x28]; mov eax, [eax+0x12C]; ret. Returns the ID
 * assigned at thread start when there is one, else the original read. */
void sub_001BD50A(void)
{
    uint32_t id = s_my_guest_tid;
    if (!id && g_fs_base) {
        uint32_t kt = OOGIE_MEM32(g_fs_base + 0x28);
        id = (kt >= 0x00010000u && kt < 0x04000000u) ? OOGIE_MEM32(kt + 0x12C) : 0;
    }
    g_eax = id;
    g_esp += 4; /* ret */
}

/* ── Oogie diagnostics: Bink movie frame step (sub_001A7DC0) ───
 * Verified (text.asm): if [0x40B144] (playing): bink = [0x40B148];
 * sub_0022E730(bink) (decode); if bink+0xC (frame) == bink+8 (frames) and not
 * looping, stop; else sub_0022E980(bink) (next frame). win_run9: the opening
 * movie mv_00.bik (6775 frames, 29.97 fps, 1 audio track) stalled after 0.8 MB
 * of 156 MB was read. Log how far it gets. */
void sub_001A7DC0_enter(void)
{
    static volatile LONG s_calls;
    LONG n = InterlockedIncrement(&s_calls);
    if (n <= 5 || (n & (n - 1)) == 0 || n % 300 == 0) {
        uint32_t bink = OOGIE_MEM32(0x0040B148u);
        fprintf(stderr, "[OOGIE] movie step #%ld playing=%u bink=0x%08X frame %u/%u\n",
                (long)n, OOGIE_MEM32(0x0040B144u) & 0xFF, bink,
                bink ? OOGIE_MEM32(bink + 0xC) : 0, bink ? OOGIE_MEM32(bink + 8) : 0);
        fflush(stderr);
    }
}

/* ── Oogie diagnostics: Bink plane decoder capture (sub_00232C40) ─
 * KNOWN_ISSUES #35: movie frames show speckled 8x8 blocks. sub_00232C40 is
 * the Bink plane decoder (11 cdecl args, called 3x per frame by sub_00234AF0,
 * reaches only BINK-section code). With OOGIE_BINK_CAPTURE=N, save guest RAM
 * plus registers on entry to its Nth and (N+1)th call, so tools/binkdiff can
 * run the original machine code on the first state and compare the result
 * with the second (what the recompiled code produced). Off by default. */
extern OOGIE_TLS uint32_t g_edx, g_ebx, g_ebp, g_esi, g_edi;

static void oogie_bink_dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    uint32_t regs[8];
    if (!f)
        return;
    regs[0] = g_eax; regs[1] = g_ecx; regs[2] = g_edx; regs[3] = g_ebx;
    regs[4] = g_esp; regs[5] = g_ebp; regs[6] = g_esi; regs[7] = g_edi;
    fwrite(regs, 4, 8, f);
    fwrite((const void *)(uintptr_t)g_xbox_mem_offset, 1, 0x04000000u, f);
    fclose(f);
    fprintf(stderr, "[OOGIE] bink capture %s esp=0x%08X\n", path, g_esp);
    fflush(stderr);
}

void sub_00232C40_enter(void)
{
    static int target = -1;
    static volatile LONG s_calls;
    LONG n;
    if (target < 0) {
        const char *e = getenv("OOGIE_BINK_CAPTURE");
        target = e ? atoi(e) : 0;
    }
    if (!target)
        return;
    n = InterlockedIncrement(&s_calls);
    if (n == target)
        oogie_bink_dump("logs\\bink_in.bin");
    else if (n == target + 1)
        oogie_bink_dump("logs\\bink_out.bin");
}

/* ── Oogie diagnostics: coloured-rectangle draws (sub_001A5990) ─
 * sub_001A5990(rect*) draws a 2D quad; verified (text.asm 0x1A59C4..0x1A5A9D):
 * colour channels are floats on a 0..128 scale, R [+0], G [+4], B [+8],
 * A [+0xC]; corner x/y at [+0x10]/[+0x14] etc. KNOWN_ISSUES #25: every Town
 * Square frame ends with a full-screen black quad at full opacity. With
 * OOGIE_RECT_TRACE=1, log rectangles with A >= 64 and the call chain, to find
 * the fade and what it waits for. Rate-limited per caller. */
static float oogie_f32(uint32_t va)
{
    uint32_t w = OOGIE_MEM32(va);
    float f;
    memcpy(&f, &w, 4);
    return f;
}

void sub_001A5990_enter(void)
{
    static int on = -1;
    static uint32_t seen[32]; static uint32_t hits[32]; static int nseen;
    uint32_t r, caller, caller2;
    float a, x0, y0, x1, y1;
    int i;

    if (on < 0) { const char *e = getenv("OOGIE_RECT_TRACE"); on = e && *e && *e != '0'; }
    if (!on)
        return;
    r = OOGIE_MEM32(g_esp + 4);
    if (r < 0x00010000u || r >= 0x04000000u)
        return;
    { uint32_t w = OOGIE_MEM32(r + 0xC); memcpy(&a, &w, 4); }
    if (!(a >= 64.0f))
        return;
    { uint32_t w;
      w = OOGIE_MEM32(r + 0x10); memcpy(&x0, &w, 4);
      w = OOGIE_MEM32(r + 0x14); memcpy(&y0, &w, 4);
      w = OOGIE_MEM32(r + 0x1C); memcpy(&x1, &w, 4);
      w = OOGIE_MEM32(r + 0x20); memcpy(&y1, &w, 4); }
    caller = OOGIE_MEM32(g_esp);
    caller2 = 0;
    for (i = 1; i < 64; i++) {           /* next code-looking word up the stack */
        uint32_t w = OOGIE_MEM32(g_esp + 4 * (uint32_t)i);
        if (w >= 0x00012000u && w < 0x001D1B68u) { caller2 = w; break; }
    }
    for (i = 0; i < nseen && seen[i] != caller; i++)
        ;
    if (i == nseen && nseen < 32) { seen[nseen] = caller; hits[nseen] = 0; nseen++; }
    if (i < 32 && hits[i]++ % 200 != 0)
        return;
    fprintf(stderr, "[RECT] from 0x%08X (<- 0x%08X) rect 0x%08X rgba %.1f %.1f %.1f %.1f"
                    " xy %.0f,%.0f %.0f,%.0f (call %u)\n",
            caller, caller2, r,
            (double)oogie_f32(r), (double)oogie_f32(r + 4), (double)oogie_f32(r + 8),
            (double)a, (double)x0, (double)y0, (double)x1, (double)y1,
            i < 32 ? hits[i] : 0);
    fflush(stderr);
}

/* ── Oogie: DirectSound DSP watchdog (sub_0020856F) ────────────
 *
 * Verified (DSOUND.asm 0x2085FE..0x20860E): DirectSound's periodic work ends
 * with `if (*[0x222100] != 0xCCCCCC) sub_0020832F()`, and sub_0020832F resets
 * the audio DSP (GP/EP control registers, reset counter [0x221AD4]) and then
 * busy-waits 10 ms (KeStallExecutionProcessor(10000) at 0x2083F0). The DSP
 * firmware writes 0xCCCCCC as its alive marker; the runtime's DSP is a stub
 * that runs no firmware, so DirectSound reset it ~8.5 times a second and the
 * game thread spent ~85% of its time in those 10 ms waits (OOGIE_STALL_STATS,
 * d3d6: 34,708 calls from 0x002083F6, nothing else significant).
 *
 * Fix: write the alive marker before the check, so the stub DSP counts as
 * running. Audio that needs the DSP's own effects is not affected either way:
 * the stub never ran them.
 */
void sub_0020856F_enter(void)
{
    static int logged;
    uint32_t p = OOGIE_MEM32(0x00222100u);
    if (!logged) {
        logged = 1;
        fprintf(stderr, "[OOGIE] DSP alive marker at 0x%08X (was 0x%08X)\n",
                p, (p >= 0x00010000u) ? OOGIE_MEM32(p) : 0);
        fflush(stderr);
    }
    if (p >= 0x00010000u && (p < 0x04000000u || p >= 0xFE830000u))
        OOGIE_MEM32(p) = 0x00CCCCCCu;
}

/* ── Oogie diagnostics: who sets per-speaker volumes (KNOWN_ISSUES #48) ──
 *
 * sub_00204155 (DSOUND.asm) is the common body of
 * IDirectSound{Buffer,Stream}::SetMixBinVolumes: [esp+4] the buffer or
 * stream, [esp+8] a DSMIXBINS {count, pairs of {mixbin, volume}}; it stores
 * each volume at [voice + 0x30 + 4 * mixbin] (sub_00203FE4) and reprograms
 * the hardware voice. Au5 caught the Town Square music voice going to
 * "front left and centre, front right muted" mid-level in one run of two.
 * RECOMP_APU_BINS=1 logs each call whose values differ from that object's
 * last call, with the code addresses found up the guest stack (game and
 * XACT), so the caller that computes them can be read.
 */
void sub_00204155_enter(void)
{
    static struct { uint32_t obj, hash; } last[64];
    static int on = -1;
    static unsigned lines;
    uint32_t obj, mb, n, i, h, chain[4], nc = 0;
    if (on < 0) { const char *e = getenv("RECOMP_APU_BINS"); on = e && *e && *e != '0'; }
    if (!on || lines >= 4000) return;
    obj = OOGIE_MEM32(g_esp + 4); mb = OOGIE_MEM32(g_esp + 8);
    if (mb < 0x10000u || mb >= 0x04000000u) return;
    n = OOGIE_MEM32(mb); if (n > 8) n = 8;
    h = 2166136261u ^ n;
    for (i = 0; i < n; i++) {
        uint32_t p = OOGIE_MEM32(mb + 4) + 8 * i;
        h = (h ^ OOGIE_MEM32(p)) * 16777619u; h = (h ^ OOGIE_MEM32(p + 4)) * 16777619u;
    }
    for (i = 0; i < 64 && last[i].obj && last[i].obj != obj; i++)
        ;
    if (i < 64) { if (last[i].obj == obj && last[i].hash == h) return; last[i].obj = obj; last[i].hash = h; }
    for (i = 0; i < 128 && nc < 4; i++) {
        uint32_t w = OOGIE_MEM32(g_esp + 4 * i);
        if ((w >= 0x00012000u && w < 0x001D1B68u) || (w >= 0x00222380u && w < 0x0022CE90u)) chain[nc++] = w;
    }
    lines++;
    fprintf(stderr, "[MIXVOL] obj 0x%08X n %u:", obj, n);
    for (i = 0; i < n; i++) {
        uint32_t p = OOGIE_MEM32(mb + 4) + 8 * i;
        fprintf(stderr, " %u=%d", OOGIE_MEM32(p), (int32_t)OOGIE_MEM32(p + 4));
    }
    fprintf(stderr, " | from");
    for (i = 0; i < nc; i++) fprintf(stderr, " 0x%08X", chain[i]);
    fprintf(stderr, "\n");
}

/* sub_0020A3F2 (DSOUND.asm) turns a voice's DirectSound state into the
 * VOLA/VOLB/VOLC words: ecx = the voice, [ecx+0x70] = its mixbin state
 * (count at +0x24, mixbin numbers at +0x28, per-mixbin volume at
 * +0x30 + 4*bin, buffer volume at +0x1C, 3D block at +0xB8), [ecx+0x64]
 * the number of hardware voices, [ecx+0xC] their indices. Logged under
 * RECOMP_APU_BINS=1 when that state changes. */
void sub_0020A3F2_enter(void)
{
    static struct { uint32_t obj, hash; } last[128];
    static int on = -1;
    static unsigned lines;
    uint32_t o = g_ecx, m, n, i, h;
    if (on < 0) { const char *e = getenv("RECOMP_APU_BINS"); on = e && *e && *e != '0'; }
    if (!on || lines >= 6000 || o < 0x10000u) return;
    m = OOGIE_MEM32(o + 0x70);
    if (m < 0x10000u || (m >= 0x04000000u && m < 0x80000000u)) return;
    n = OOGIE_MEM32(m + 0x24); if (n > 8) n = 8;
    h = 2166136261u ^ n ^ (OOGIE_MEM32(m + 0x1C) << 8) ^ (OOGIE_MEM32(m + 8) << 3);
    for (i = 0; i < 8; i++) h = (h ^ OOGIE_MEM32(m + 0x28 + i / 4 * 4)) * 16777619u;
    for (i = 0; i < 32; i++) h = (h ^ OOGIE_MEM32(m + 0x30 + 4 * i)) * 16777619u;
    for (i = 0; i < 128 && last[i].obj && last[i].obj != o; i++)
        ;
    if (i < 128) { if (last[i].obj == o && last[i].hash == h) return; last[i].obj = o; last[i].hash = h; }
    lines++;
    fprintf(stderr, "[VOLCALC] t=%lu voice 0x%08X hw %u x%u st 0x%08X n %u vol %d 3d %u bins",
            (unsigned long)GetTickCount(), o, OOGIE_MEM32(o + 0xC) & 0xFFFF,
            OOGIE_MEM32(o + 0x64) & 0xFF, m, n, (int32_t)OOGIE_MEM32(m + 0x1C),
            (OOGIE_MEM32(m + 0xB8) != 0) | ((OOGIE_MEM32(m + 8) >> 23) & 2));
    for (i = 0; i < n; i++) {
        uint32_t b = (OOGIE_MEM32(m + 0x28 + i / 4 * 4) >> (8 * (i % 4))) & 0xFF;
        fprintf(stderr, " %u:%d", b, b < 32 ? (int32_t)OOGIE_MEM32(m + 0x30 + 4 * b) : 0);
    }
    fprintf(stderr, " | from 0x%08X\n", OOGIE_MEM32(g_esp));
}

/* ── Oogie diagnostics: scheduler thread lifetime ──────────────
 *
 * sub_001AD3A0 ("should this thread stop?") answers yes when the caller's ID
 * is not in the table at 0x40EDD8 or its entry state is 3; sub_001AD440(tid)
 * sets state 3; sub_001AD520 / sub_001AD540 are the scheduler's thread exits
 * (verified, text.asm). win_run6: the game-logic thread 0x0019D8E0 was gone
 * within 20 s. Log who exits and who is killed.
 */
static uint32_t oogie_guest_tid(void)
{
    uint32_t kt;
    if (s_my_guest_tid)
        return s_my_guest_tid;
    if (!g_fs_base)
        return 0;
    kt = OOGIE_MEM32(g_fs_base + 0x28);
    return (kt >= 0x00010000u && kt < 0x04000000u) ? OOGIE_MEM32(kt + 0x12C) : 0;
}

static void oogie_dump_sched_table(void)
{
    uint32_t e;
    for (e = 0x0040EDD8u; e < 0x0040F0D8u; e += 0x18)
        if (OOGIE_MEM32(e) != 0xFFFFFFFFu && OOGIE_MEM32(e) != 0)
            fprintf(stderr, "    slot 0x%08X: handle 0x%08X tid 0x%08X state %u\n",
                    e, OOGIE_MEM32(e), OOGIE_MEM32(e + 4), OOGIE_MEM32(e + 0xC));
}

static void oogie_log_exit(const char *which)
{
    fprintf(stderr, "[OOGIE] %s: guest tid 0x%08X host %lu, called from 0x%08X\n",
            which, oogie_guest_tid(), (unsigned long)GetCurrentThreadId(),
            OOGIE_MEM32(g_esp));
    oogie_dump_sched_table();
    fflush(stderr);
}

static void oogie_park_mark(uint32_t tid, LONG awake);
void sub_001AD520_enter(void) { oogie_log_exit("sched thread exit (AD520)"); oogie_park_mark(oogie_guest_tid(), 0); }
void sub_001AD540_enter(void) { oogie_log_exit("sched thread exit (AD540)"); oogie_park_mark(oogie_guest_tid(), 0); }

void sub_001AD440_enter(void)
{
    fprintf(stderr, "[OOGIE] sched kill tid 0x%08X by guest tid 0x%08X, called from 0x%08X\n",
            OOGIE_MEM32(g_esp + 4), oogie_guest_tid(), OOGIE_MEM32(g_esp));
    fflush(stderr);
    oogie_park_mark(OOGIE_MEM32(g_esp + 4), 0);   /* a killed task never parks */
}

/* ── Oogie: scheduler park / wake without lost wake-ups ────────
 *
 * The game's coroutine-style scheduler hands control between threads with
 *   sub_001AD4B0(tid)  park:  SuspendThread(table[tid].handle)   (always the
 *                      caller's own tid: call sites 0x58120 and 0x586C7 pass
 *                      sub_001AD560() = GetCurrentThreadId)
 *   sub_001AD4F0(tid)  wake:  ResumeThread(table[tid].handle)    (one caller,
 *                      sub_00057E00)
 * Both return tid in eax and are cdecl (verified, text.asm). On the Xbox's
 * single CPU the waker cannot run before the parker has really stopped (the
 * waker's drain loop sub_001A7BA0 yields until nothing else is runnable). On a
 * PC it can: a ResumeThread that arrives before the SuspendThread does
 * nothing, the thread then parks for good, and the game sits on Now Loading
 * (KNOWN_ISSUES #29; live18 samples: game-logic thread parked at 0x001AD4E8
 * while the main loop ran on).
 *
 * Fix: park and wake use a per-thread auto-reset event instead of thread
 * suspension. An event remembers a wake that arrives first, so the pair is
 * race-free. Everything else that suspends or resumes threads (Bink, XAPI
 * CreateThread's resume) still uses the real calls.
 */
#define OOGIE_PARK_SLOTS 256
static HANDLE s_park_evt[OOGIE_PARK_SLOTS];
static uint32_t s_park_tid[OOGIE_PARK_SLOTS];
/* 1 while a woken thread has not parked (or exited) again: see sub_001BD332. */
static volatile LONG s_park_awake[OOGIE_PARK_SLOTS];
static volatile LONG s_park_awake_n;
static volatile LONG s_park_lock;

static HANDLE oogie_park_event(uint32_t tid)
{
    int i;
    HANDLE h = NULL;
    while (InterlockedCompareExchange(&s_park_lock, 1, 0)) SwitchToThread();
    for (i = 0; i < OOGIE_PARK_SLOTS; i++) {
        if (s_park_evt[i] && s_park_tid[i] == tid) { h = s_park_evt[i]; break; }
        if (!s_park_evt[i]) {
            s_park_evt[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
            s_park_tid[i] = tid;
            h = s_park_evt[i];
            break;
        }
    }
    InterlockedExchange(&s_park_lock, 0);
    return h;
}

static int oogie_park_slot(uint32_t tid)
{
    int i;
    for (i = 0; i < OOGIE_PARK_SLOTS && s_park_evt[i]; i++)
        if (s_park_tid[i] == tid) return i;
    return -1;
}
static void oogie_park_mark(uint32_t tid, LONG awake)
{
    int i = oogie_park_slot(tid);
    if (i >= 0 && InterlockedExchange(&s_park_awake[i], awake) != awake)
        InterlockedAdd(&s_park_awake_n, awake ? 1 : -1);
}

void sub_001AD4B0(void)
{
    uint32_t tid = OOGIE_MEM32(g_esp + 4);
    HANDLE h = oogie_park_event(tid);
    oogie_park_mark(tid, 0);                 /* parked: the waker may go on */
    {
        int fo = oogie_fo_slot();            /* #57: a parked task's step ends here */
        if (fo >= 0) InterlockedExchange(&s_fo[fo].in_step, 0);
    }
    if (h)
        { int held = guest_cpu_held(); if (held) guest_cpu_part();   /* #61 */
          WaitForSingleObject(h, INFINITE); if (held) guest_cpu_join(); }
    {
        int fo = oogie_fo_slot();            /* and the next one starts when it is woken */
        if (fo >= 0) InterlockedExchange(&s_fo[fo].in_step, 1);
    }
    g_eax = tid;
    g_esp += 4; /* ret */
}

void sub_001AD4F0(void)
{
    uint32_t tid = OOGIE_MEM32(g_esp + 4);
    HANDLE h = oogie_park_event(tid);
    oogie_park_mark(tid, 1);                 /* runs until it parks again */
    if (h)
        SetEvent(h);
    g_eax = tid;
    g_esp += 4; /* ret */
}

/* ── Oogie: the frame's overlay waits for the tasks that draw (#57) ──
 *
 * Verified (D3D traces, 2026-10-21): every frame two game threads draw. A
 * task thread (started at 0x00057E60, run by the main thread's task runner
 * sub_000583C0 through its semaphore) begins its frame with D3D Clear
 * (sub_001A5310: Clear(0, 0, 0xF3, colour, 1.0, 0), from sub_0019D390,
 * sub_000781F0, sub_001A0540 ...) and draws the scene; the main thread then
 * draws the overlay and swaps in sub_000586E0 (0x1A85DE ..., Swap at
 * 0x1A7BB8). In a flickering session the main thread's overlay went into the
 * buffer before the task's Clear wiped it ("D35C*11 C35C D35C*4": the
 * owner's pause menu, trophy shelf, figure viewer, chapter select), and the
 * D3D lock was contended 12 288 times against 7-8 in clean sessions. On the
 * console the task always finished its step first. So when a thread takes
 * sub_000586E0's drawing path (it is not the job worker at [0x3E3F68]), it
 * first waits until no other game thread is drawing (no D3D call from it in
 * the last 3 ms), at most OOGIE_FRAME_ORDER_MS (default 30; 0 = off).
 * Threads that sleep, load or wait inside a step are not waited for. */
extern void sub_000586E0_gen(void);
void sub_000586E0(void)
{
    static int cap = -2;
    static volatile LONG n_wait, n_cap;
    if (cap == -2) { const char *e = getenv("OOGIE_FRAME_ORDER_MS"); cap = e && *e ? atoi(e) : 30; }
    /* Not under the guest CPU lock: there the drawing task cannot run while
     * this thread holds the CPU, and the scheduler's priority order already
     * lets it finish first. */
    if (cap > 0 && s_is_game_thread && !guest_cpu_held() && OOGIE_MEM32(0x003E3F68u) != oogie_guest_tid()) {
        extern volatile long g_oogie_presents;
        int me = oogie_fo_slot(), i, busy;
        DWORD t0 = GetTickCount();
        int waited = 0;
        if (!s_fo_qpf) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_fo_qpf = f.QuadPart; }
        for (;;) {
            /* Busy: another game thread is drawing right now, i.e. called
             * D3D in the last 3 ms. A task sleeping or loading inside its
             * step is not (with the owner's saves, waiting for those held
             * every frame 30 ms and boot never reached the intro film). */
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            busy = 0;
            for (i = 0; i < OOGIE_FO_N; i++)
                if (i != me && s_fo[i].tid && s_fo[i].last_qpc
                        && q.QuadPart - s_fo[i].last_qpc < s_fo_qpf * 3 / 1000)
                    busy = 1;
            if (!busy) break;
            if (GetTickCount() - t0 >= (DWORD)cap) {
                LONG k = InterlockedIncrement(&n_cap);
                if (k <= 10 || (k & 1023) == 0) {
                    fprintf(stderr, "[SCHED] frame overlay: a drawing task still in its step after %d ms; going on (%ld of %ld waits)\n",
                            cap, (long)k, (long)n_wait);
                    fflush(stderr);
                }
                /* No stack dumps from here: suspending a thread from a game
                 * thread can deadlock (owner 2026-10-22: the main thread
                 * suspended itself and the game froze). */
                break;
            }
            if (!waited) { waited = 1; InterlockedIncrement(&n_wait); }
            oogie_host_yield(GetTickCount() - t0 < 1 ? -1 : 0);
        }
    }
    sub_000586E0_gen();
}

/* ── Oogie: woken tasks run before the main thread goes on (#57) ──
 *
 * Verified (ts21 D3D trace, 2026-10-21): in a flickering session two game
 * threads draw whole frames at once: the main thread (0x7E000001: its render
 * path ending in Swap, sub_001A7BA0) and a scheduler task thread (started at
 * 0x00057E60, drawing its menu sprites through 0x001A6A31 / 0x001A60EE and
 * the game's graphics lock 0x80102000). The D3D lock was contended 12 288+
 * times in the owner's flickering session and 7-8 times in clean ones.
 * sub_000583C0, the main thread's task runner, starts every frame by waking
 * the parked task threads (sub_00057E00 -> sub_001AD4F0) and then goes on
 * with its own frame; on the console a woken task runs its step at once and
 * parks again before the main thread continues, so its drawing always comes
 * at the same point of the frame. Here the main thread went on at once and
 * the task's sprites landed before or after the main thread's clear by
 * chance: the menus that flicker in some sessions and not in others. After
 * sub_00057E00 the main thread now waits until every woken task has parked
 * again (or exited, or been killed), at most OOGIE_TASK_RUN_MS (default 30;
 * 0 = off), letting the CPU go meanwhile. */
extern void sub_00057E00_gen(void);
void sub_00057E00(void)
{
    static int cap = -2;
    static volatile LONG n_wait, n_cap;
    DWORD t0;
    sub_00057E00_gen();
    if (cap == -2) { const char *e = getenv("OOGIE_TASK_RUN_MS"); cap = e && *e ? atoi(e) : 0; }   /* opt-in: 30 ms made every frame a hitch (tn1-tn3) */
    if (cap <= 0 || s_park_awake_n <= 0)
        return;
    t0 = GetTickCount();
    InterlockedIncrement(&n_wait);
    while (s_park_awake_n > 0) {
        DWORD el = GetTickCount() - t0;
        if (el >= (DWORD)cap) {
            LONG k = InterlockedIncrement(&n_cap);
            if (k <= 10 || (k & 1023) == 0) {
                fprintf(stderr, "[SCHED] woken task(s) still running after %d ms; main thread goes on (%ld of %ld waits)\n",
                        cap, (long)k, (long)n_wait);
                fflush(stderr);
            }
            break;
        }
        oogie_host_yield(el < 2 ? -1 : 0);
    }
}

/* ── Oogie diagnostics: XAPI SuspendThread / ResumeThread ───────
 *
 * sub_001BD290 = SuspendThread(h), sub_001BD2B6 = ResumeThread(h): each calls
 * NtSuspendThread / NtResumeThread and takes its error path (SetLastError,
 * return -1) when the status is negative. win_run3 showed that error path on
 * nearly every call (RtlNtStatusToDosError after each). Log the handle the
 * title passes and which host thread asks, for the first calls.
 */
static void oogie_log_thread_op(const char *op)
{
    static volatile LONG s_n;
    uint32_t h = *(volatile uint32_t *)((uintptr_t)(g_esp + 4) + g_xbox_mem_offset);
    uint32_t ret = *(volatile uint32_t *)((uintptr_t)g_esp + g_xbox_mem_offset);
    LONG n = InterlockedIncrement(&s_n);
    if (n <= 40) {
        fprintf(stderr, "[OOGIE] %s(handle 0x%08X) from 0x%08X on host thread %lu\n",
                op, h, ret, (unsigned long)GetCurrentThreadId());
        fflush(stderr);
    }
}

void sub_001BD290_enter(void) { oogie_log_thread_op("SuspendThread"); }
void sub_001BD2B6_enter(void) { oogie_log_thread_op("ResumeThread"); }

/* ── Oogie: DirectSound / XACT "raise IRQL" locks (KNOWN_ISSUES #48) ──
 *
 * Verified (DSOUND.asm 0x203A90 / 0x203AB2, XACTENG.asm 0x2227D2 / 0x2227F4,
 * identical pairs): the audio libraries guard their state with a stack
 * object {old irql (byte), raised (dword at +4)}: if KPCR.Irql (fs:[0x24])
 * is below DISPATCH_LEVEL, KfRaiseIrql(2) and remember to lower. On the
 * single-CPU Xbox nothing else runs at that point; here the runtime keeps
 * the level per thread, so two guest threads were inside at once.
 * DirectSound programs a voice by selecting it (PIO 0x2F8) and then writing
 * VOLA/VOLB/VOLC (0x360..0x368); with two threads interleaved one voice got
 * another's volumes. Seen (au5, au7, au8, au9, RECOMP_APU_BINS=1): the
 * Town Square music voice (hw 75, mixbins 0,1) received the volumes of an
 * effect routed to mixbins 10,3,31 -- "front left and centre, front right
 * muted" -- so the music lost its right channel and its level followed the
 * effect's. XACT also calls DirectSound while raised, which makes
 * DirectSound skip its own critical section (sub_00203C95), so the two
 * libraries' raised sections share one host lock here. Taken only on the
 * PASSIVE -> DISPATCH edge, as the original raises only then; the
 * interrupt gate in kernel_hal.c still keeps ISRs out while it is held.
 * OOGIE_NO_DS_LOCK=1 restores the original behaviour (for comparison).
 */
extern unsigned char __fastcall xbox_KfRaiseIrql(unsigned char new_irql);
extern void __fastcall xbox_KfLowerIrql(unsigned char new_irql);
static CRITICAL_SECTION s_dispatch_cs;
static volatile LONG s_dispatch_cs_state;   /* 0 none, 1 initialising, 2 ready */
static int s_dispatch_lock_on = -1;

static void oogie_dispatch_lock(int take)
{
    if (s_dispatch_lock_on < 0) {
        const char *e = getenv("OOGIE_NO_DS_LOCK");
        s_dispatch_lock_on = !(e && *e && *e != '0');
    }
    if (!s_dispatch_lock_on) return;
    if (s_dispatch_cs_state != 2) {
        if (InterlockedCompareExchange(&s_dispatch_cs_state, 1, 0) == 0) {
            InitializeCriticalSectionAndSpinCount(&s_dispatch_cs, 4000);
            InterlockedExchange(&s_dispatch_cs_state, 2);
        }
        while (s_dispatch_cs_state != 2) SwitchToThread();
    }
    if (take) oogie_cs_enter(&s_dispatch_cs);
    else LeaveCriticalSection(&s_dispatch_cs);
}

/* The thread polling in sub_0020B669 while it holds the lock (0: none). An
 * interrupt service routine (IRQL above DISPATCH_LEVEL) may then run
 * DirectSound without the lock, as the console's ISR cuts into that loop;
 * nothing else may. */
static volatile DWORD s_ds_poll_tid;
/* Passes of DirectSound's DPC (sub_002088F1, the APU interrupt's follow-up:
 * it completes stopped voices from the queue at [+0x4A0], sub_0020821C).
 * A forced voice stop waits for two (#78). */
static volatile LONG s_ds_dpc_passes;
void sub_002088F1_enter(void) { InterlockedIncrement(&s_ds_dpc_passes); }

static void oogie_raise_enter(int allow_isr)
{
    uint32_t o = g_ecx;
    unsigned char irql = g_fs_base ? *(volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + g_fs_base + 0x24) : 0;
    uint32_t raise = irql < 2;
    if (allow_isr && irql >= 2 && !s_is_game_thread && s_ds_poll_tid && s_dispatch_cs_state == 2   /* ISR or DPC (#55: the stop is finished by the DPC, sub_0020821C) */
            && (DWORD)(uintptr_t)s_dispatch_cs.OwningThread == s_ds_poll_tid) {
        OOGIE_MEM32(o + 4) = 3u;                /* entered without the lock */
        g_eax = 0;
        g_esp += 4;
        return;
    }
    /* [o+4]: 1 = raised (and locked), as the original; 2 = already at
     * DISPATCH_LEVEL or above, locked only. A DPC or ISR runs DirectSound
     * at raised IRQL on the runtime's own threads (au13: 7 voice
     * programs interleaved with the game thread's even with the PASSIVE
     * edge locked), which a single CPU would never run alongside a raised
     * thread, so it takes the lock too. Nothing but sub_00203AB2 /
     * sub_002227F4 reads the flag (checked: the other fs:[0x24] readers in
     * DSOUND/XACTENG only choose whether to call these). */
    oogie_dispatch_lock(1);
    OOGIE_MEM32(o + 4) = raise ? 1u : 2u;
    g_eax = raise;
    if (raise) {
        g_eax = xbox_KfRaiseIrql(2);
        *(volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + o) = (uint8_t)g_eax;
    }
    g_esp += 4; /* ret */
}

static void oogie_raise_leave(void)
{
    uint32_t o = g_ecx, f = OOGIE_MEM32(o + 4);
    if (f == 3) {
        OOGIE_MEM32(o + 4) = 0;                 /* the ISR way in: no lock held */
    } else if (f) {
        if (f == 1)
            xbox_KfLowerIrql(*(volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + o));
        OOGIE_MEM32(o + 4) = 0;
        oogie_dispatch_lock(0);
    }
    g_esp += 4; /* ret */
}


/* ── Oogie: DirectSound "wait for the voice to stop" (KNOWN_ISSUES #55) ──
 *
 * Verified (DSOUND.asm 0x20B669): if voice[+0x12] & 1, loop
 * "while (voice[+0x12] & 0x8000) sub_0020AA01(voice)"; sub_0020AA01 forces
 * the stop once 500 ms (0x4C4B40 x 100 ns) have passed since [+0x78]. The
 * 0x8000 bit is cleared by the APU's interrupt, which on the console cuts
 * into this loop. Here the loop holds the raised-IRQL lock above, the
 * interrupt (apu_deliver_irq, on the APU frame thread) runs DirectSound
 * code that takes the same lock, and waits: every voice stop cost the full
 * 500 ms with the game thread blocked behind XACT ([CS] sk58d: holder in
 * sub_0020AA01 polling KeQuerySystemTime, waiter got the lock after 485-500
 * ms; the owner's half-second freezes in play). Same loop; while it polls,
 * an ISR or DPC of the emulator (a thread that is not a game thread, at
 * DISPATCH_LEVEL or above) may enter DirectSound without the lock: the
 * stop is finished by DirectSound's DPC (sub_0020821C, owner 2026-10-19 Ch3
 * at 1-2 fps with threads in this loop).
 * Releasing the lock instead let any thread in, and XACT crashed on a
 * chapter load (lt3, sub_002246F8). OOGIE_DS_STOP_OLD=1 runs the original. */
extern void sub_0020AA01(void);
static int s_ds_stop_ms = -1;
/* Voices whose stop was forced once (2026-10-22, owner play.log 12:21: the same
 * five voices, one stop every ~25 frames near Jack's house, each forced after
 * the full 50 ms: 109 hitches of 66-120 ms, "more slowdowns"). Their "stopped"
 * report does not come, so later stops of them are forced after 2 ms. */
#define OOGIE_DS_DEAF_N 32
static volatile uint32_t s_ds_deaf[OOGIE_DS_DEAF_N];
static int ds_deaf(uint32_t v)
{
    int k;
    for (k = 0; k < OOGIE_DS_DEAF_N; k++) if (s_ds_deaf[k] == v) return 1;
    return 0;
}
static void ds_deaf_add(uint32_t v)
{
    static volatile LONG n;
    if (!ds_deaf(v)) s_ds_deaf[(InterlockedIncrement(&n) - 1) % OOGIE_DS_DEAF_N] = v;
}
void sub_0020B669(void)
{
    static int old = -1;
    uint32_t v = g_ecx;
    if (old < 0) { const char *e = getenv("OOGIE_DS_STOP_OLD"); old = e && *e == '1'; }
    if (OOGIE_MEM8(v + 0x12) & 1) {
        DWORD me = GetCurrentThreadId();
        LARGE_INTEGER q0, qf;
        int mine = !old && s_ds_poll_tid == 0, cut = 0, lim;
        if (mine) s_ds_poll_tid = me;           /* admit the APU's ISR meanwhile */
        if (s_ds_stop_ms < 0) {
            const char *e = getenv("OOGIE_DS_STOP_MS");
            s_ds_stop_ms = e && *e ? atoi(e) : 50;
        }
        /* #72 (2026-10-24): a deaf voice is forced at once, and the wait is
         * timed with the performance counter. GetTickCount moves in 15.6 ms
         * steps, so the "2 ms" wait lasted up to 16 ms; Casino Clash restarts
         * two deaf voices every 40 frames, and each restart was a 34-75 ms
         * frame (ds1_ch17: 40 such frames in 3 min). */
        /* #78 (2026-10-25): back to about the old wait for deaf voices
         * (GetTickCount's 2 ms was up to one 15.6 ms tick): forced at once,
         * the DPC freed-voice crash below came within the day. */
        lim = ds_deaf(v) && s_ds_stop_ms > 16 ? 16 : s_ds_stop_ms;
        QueryPerformanceFrequency(&qf);
        QueryPerformanceCounter(&q0);
        while (OOGIE_MEM16(v + 0x12) & 0x8000) {
            /* The owner's 2026-10-20 13:50 session: after walking Town
             * Square <-> Hinterland a few times, every voice stop waited
             * the full 500 ms (19 freezes in a row, then an XACT crash):
             * the "voice has stopped" report never came. Once a stop has
             * waited OOGIE_DS_STOP_MS (default 50; 0 = the original
             * 500 ms), its start time [+0x78] is moved back so
             * sub_0020AA01 takes its own timeout path (sub_0020A918, the
             * forced stop that DirectSound counts at [0x222108]). */
            LARGE_INTEGER qn;
            QueryPerformanceCounter(&qn);
            /* #78 (2026-10-25, owner, Chapter 5 spider boss: R6025 pure virtual
             * call from the DPC at 0x20828E, then a DPC list walk through freed
             * memory: frame drop, controls locked). A stop forced before the
             * DPC had run let the destructor free a voice still in the DPC's
             * completion queue; with deaf voices forced at once (#72) that
             * happened in fights with many short sounds. Waiting for two DPC
             * passes instead (s_ds_dpc_passes) was tried: the DPC seldom runs
             * during the wait, so stops sat out a 100 ms backstop (dpc1_ch17:
             * 16 stalls, 55 fps). Deaf voices wait 16 ms again (lim above). */
            if (!old && !cut && s_ds_stop_ms > 0
                && (qn.QuadPart - q0.QuadPart) * 1000 >= (LONGLONG)lim * qf.QuadPart) {
                static volatile LONG n_cut;
                cut = 1;
                ds_deaf_add(v);
                OOGIE_MEM32(v + 0x78) = 0;
                OOGIE_MEM32(v + 0x7C) = 0;
                if (InterlockedIncrement(&n_cut) <= 20 || (n_cut & 255) == 0) {
                    fprintf(stderr, "[DSOUND] voice 0x%08X still stopping after %d ms: forced stop (%ld so far)\n",
                            v, lim, (long)n_cut);
                    fflush(stderr);
                }
            }
            g_ecx = v;
            g_esp -= 4;
            OOGIE_MEM32(g_esp) = 0x0020B67Bu;   /* return address of the original call */
            sub_0020AA01();                     /* thiscall; its ret pops it */
            if (!old && (OOGIE_MEM16(v + 0x12) & 0x8000))
                oogie_yield();
        }
        if (mine) s_ds_poll_tid = 0;
    }
    g_ecx = v;
    g_esp += 4;                                 /* ret */
}

void sub_00203A90(void) { oogie_raise_enter(1); }
void sub_00203AB2(void) { oogie_raise_leave(); }
void sub_002227D2(void) { oogie_raise_enter(0); }   /* XACT: never without the lock (owner 13:50 crash in sub_002246F8) */
void sub_002227F4(void) { oogie_raise_leave(); }

/* ── Oogie: the game's semaphore wrapper (KNOWN_ISSUES #50) ────
 *
 * Verified (text.asm 0x1AA3E0..0x1AA4C0): the game keeps 64 semaphores in a
 * table at 0x40C058, 32 bytes each: [+0] handle, [+4] its own count of the
 * semaphore's value, [+8] the maximum, [+0x10] waiters. sub_001AA3E0(i, n)
 * (release; called with the game mutex [0x40C858] held) skips the real
 * ReleaseSemaphore when [+4] >= [+8], else adds n to [+4] and takes n off
 * [+0x10]. sub_001AA460(i) (wait) does `inc [+0x10]`, waits, then
 * `dec [+4]` -- plain read-modify-write, no mutex. On the single-CPU Xbox a
 * thread switch inside those three instructions is rare; here the waiting
 * and releasing threads run at the same time, updates are lost, [+4] drifts
 * up to the maximum, releases are skipped, and the waiter sleeps for good.
 * Seen (cc3, cc5): pause menu EXIT twice -> the main thread waits in
 * sub_000583C0 for a task that the logic thread (sub_0019D8E0) never runs
 * again, because that thread is blocked in sub_001AA460 on its semaphore;
 * the pad stops being read and the picture freezes.
 * Fix: both functions as the original, with the bookkeeping under one host
 * lock (the wait itself outside it). The real semaphore calls go through the
 * recompiled XAPI functions. OOGIE_NO_SEM_LOCK=1: same code without the
 * lock (the original's race, for comparison). */
extern void sub_001BD012(void);     /* WaitForSingleObjectEx(h, ms, alertable), stdcall */
extern void sub_001BCF74(void);     /* ReleaseSemaphore(h, n, prev), stdcall */
extern void sub_001BC741(void);     /* called when the release fails, no arguments */
static CRITICAL_SECTION s_sem_cs;
static volatile LONG s_sem_cs_state;
static int s_sem_lock_off = -1;

static void sem_lock(int take)
{
    if (s_sem_lock_off < 0) {
        const char *v = getenv("OOGIE_NO_SEM_LOCK");
        s_sem_lock_off = v && *v && *v != '0';
    }
    if (s_sem_lock_off) return;
    if (s_sem_cs_state != 2) {
        if (InterlockedCompareExchange(&s_sem_cs_state, 1, 0) == 0) {
            InitializeCriticalSectionAndSpinCount(&s_sem_cs, 4000);
            InterlockedExchange(&s_sem_cs_state, 2);
        }
        while (s_sem_cs_state != 2) SwitchToThread();
    }
    if (take) oogie_cs_enter(&s_sem_cs);
    else LeaveCriticalSection(&s_sem_cs);
}

/* Call a recompiled stdcall function of three arguments from C: build the
 * frame the way `push c; push b; push a; call` would. */
static uint32_t guest_stdcall3(void (*fn)(void), uint32_t a, uint32_t b, uint32_t c, uint32_t ret)
{
    uint32_t esp0 = g_esp;
    g_esp -= 16;
    OOGIE_MEM32(g_esp) = ret;
    OOGIE_MEM32(g_esp + 4) = a;
    OOGIE_MEM32(g_esp + 8) = b;
    OOGIE_MEM32(g_esp + 12) = c;
    fn();
    g_esp = esp0;
    return g_eax;
}

/* The same for n arguments (args[0] is the first, pushed last). */
static uint32_t guest_stdcalln(void (*fn)(void), uint32_t ret, int n, const uint32_t *args)
{
    uint32_t esp0 = g_esp;
    int k;
    g_esp -= 4u * (uint32_t)(n + 1);
    OOGIE_MEM32(g_esp) = ret;
    for (k = 0; k < n; k++)
        OOGIE_MEM32(g_esp + 4u + 4u * (uint32_t)k) = args[k];
    fn();
    g_esp = esp0;
    return g_eax;
}

/* ── Oogie: Bink's worker thread, created in a safe order (#42, #61) ──
 *
 * Verified (BINK.asm 0x22F3D0..0x22F477): sub_0022F3D0(rate) allocates a
 * 0x24-byte block, fills [+0] [+4] [+0x18] [+0x1C] [+0x20], creates the worker
 * (CreateThread(0, 0x8000, 0x22F360, block, CREATE_SUSPENDED, &block[+0x10]),
 * handle in [+0xC]), optionally lowers its priority, and only then creates the
 * mutex [+0x14] and the event [+8] the worker waits on. The console's
 * CREATE_SUSPENDED kept the worker off until the caller resumed it; the
 * runtime starts it at once (#11; honouring it in general made soaks worse),
 * so the worker could wait on an unmade handle: handle 0xFFFFFFFF for good
 * (gs1), and in a normal session a title film that never streams (#42,
 * hypothesis). Same steps, with the mutex and event made before the thread.
 * OOGIE_BINK_ORDER_OLD=1: the original. */
extern void sub_001C2FF0(void);   /* malloc(n), stdcall */
extern void sub_001BD49C(void);   /* CreateThread, 6 args, stdcall */
extern void sub_001BD23E(void);   /* (h, -1): thread priority, stdcall */
extern void sub_001C3060(void);   /* free(p), stdcall */
extern void sub_001BCF9A(void);   /* CreateMutex(0, 0, 0), stdcall */
extern void sub_001BCE98(void);   /* CreateEvent(0, 0, 0, 0), stdcall */
void sub_0022F3D0(void)
{
    static int old = -1;
    uint32_t rate = OOGIE_MEM32(g_esp + 4), blk, h, a[6];
    int32_t r = (int32_t)rate;
    int low = 0;
    if (old < 0) { const char *e = getenv("OOGIE_BINK_ORDER_OLD"); old = e && *e == '1'; }
    a[0] = 0x24;
    blk = guest_stdcalln(sub_001C2FF0, 0x0022F3D8u, 1, a);
    if (!blk) { g_eax = 0; g_esp += 8; return; }
    if (r < 0) { r = -r; low = 1; }
    OOGIE_MEM32(blk + 0x00) = 0;
    OOGIE_MEM32(blk + 0x18) = 0;
    OOGIE_MEM32(blk + 0x1C) = 1;
    OOGIE_MEM32(blk + 0x20) = 0;
    OOGIE_MEM32(blk + 0x04) = r ? (uint32_t)(1000 / r) : 1000u;
    if (!old) {
        uint32_t z[4] = { 0, 0, 0, 0 };
        OOGIE_MEM32(blk + 0x14) = guest_stdcalln(sub_001BCF9A, 0x0022F45Eu, 3, z);
        OOGIE_MEM32(blk + 0x08) = guest_stdcalln(sub_001BCE98, 0x0022F46Eu, 4, z);
    }
    a[0] = 0; a[1] = 0x8000; a[2] = 0x0022F360u; a[3] = blk; a[4] = 4; a[5] = blk + 0x10;
    h = guest_stdcalln(sub_001BD49C, 0x0022F430u, 6, a);
    OOGIE_MEM32(blk + 0x0C) = h;
    if (low) { a[0] = h; a[1] = 0xFFFFFFFFu; guest_stdcalln(sub_001BD23E, 0x0022F440u, 2, a); }
    if (!OOGIE_MEM32(blk + 0x0C)) {
        a[0] = blk;
        guest_stdcalln(sub_001C3060, 0x0022F44Du, 1, a);
        g_eax = 0; g_esp += 8; return;
    }
    if (old) {
        uint32_t z[4] = { 0, 0, 0, 0 };
        OOGIE_MEM32(blk + 0x14) = guest_stdcalln(sub_001BCF9A, 0x0022F45Eu, 3, z);
        OOGIE_MEM32(blk + 0x08) = guest_stdcalln(sub_001BCE98, 0x0022F46Eu, 4, z);
    }
    g_eax = blk;
    g_esp += 8;                      /* ret 4 */
}

/* ── Oogie: streamed reads on one worker thread (dev cycle 3, item 4) ──
 *
 * Verified (text 0x1A8410..0x1A8489, 0x1A83A0..0x1A840F): sub_001A8410(i,
 * buf, size) starts an asynchronous read of the game's open file i: it puts
 * the request in [0x40B864] (busy = 1), [0x40B868] i, [0x40B86C] buf,
 * [0x40B870] size, closes the previous reader's handle [0x40B874], creates a
 * thread at 0x1A83A0 and sets it to THREAD_PRIORITY_HIGHEST. That thread does
 * ReadFile(handle[i], buf, size, 0x40B320 + i*20, 0) up to 5 times (stopping
 * on success, EOF 0x26 or pending 0x3E5; sub_001A7A70 after 5 failures),
 * clears the busy flag and exits. Callers (about 30 in the loader,
 * 0x4FB50..0x52070) only poll the flag (sub_001A8500); nothing waits on the
 * handle. The owner's 13:30 session created 981 such threads; the long frames
 * left (55-85 ms, 23 in ~9 min) came after bursts of them while a sound bank
 * streamed. Here the first request creates the thread as the game does; that
 * thread then stays and serves every later request (same read code), woken by
 * an event. OOGIE_STREAM_WORKER=0: a thread per read, as the original. */
extern void sub_001A8410_gen(void);
extern void sub_001A83A0_gen(void);
extern void sub_001BCC81(void);     /* ReadFile-style, 5 args, stdcall */
extern void sub_001A7A70(void);     /* after 5 failed reads, no arguments */
static int s_sw_on = -1;
static HANDLE s_sw_ev;
static CRITICAL_SECTION s_sw_cs;
static volatile LONG s_sw_seq, s_sw_started, s_sw_n;

static int sw_on(void)
{
    if (s_sw_on < 0) {
        const char *e = getenv("OOGIE_STREAM_WORKER");
        InitializeCriticalSection(&s_sw_cs);
        s_sw_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        s_sw_on = !(e && *e == '0') && s_sw_ev != NULL;
    }
    return s_sw_on;
}

/* the read thread (0x1A83A0) */
void sub_001A83A0(void)
{
    if (!sw_on()) { sub_001A83A0_gen(); return; }
    fprintf(stderr, "[STREAM] read worker started: one thread serves every streamed read\n");
    fflush(stderr);
    for (;;) {
        uint32_t idx, buf, size, a[5], e;
        LONG seq;
        int k, busy;
        oogie_cs_enter(&s_sw_cs);
        busy = OOGIE_MEM32(0x0040B864u) != 0;
        seq = s_sw_seq;
        idx = OOGIE_MEM32(0x0040B868u); buf = OOGIE_MEM32(0x0040B86Cu); size = OOGIE_MEM32(0x0040B870u);
        LeaveCriticalSection(&s_sw_cs);
        if (!busy) {                     /* let other guest threads run meanwhile */
            int held = guest_cpu_held();
            if (held) guest_cpu_part();
            WaitForSingleObject(s_sw_ev, 100);
            if (held) guest_cpu_join();
            continue;
        }
        for (k = 0; k < 5; k++) {
            a[0] = OOGIE_MEM32(0x0040B210u + idx * 4u); a[1] = buf; a[2] = size;
            a[3] = 0x0040B320u + idx * 20u; a[4] = 0;
            if (guest_stdcalln(sub_001BCC81, 0x001A83DDu, 5, a)) break;
            e = guest_stdcalln(sub_001BC741, 0x001A83E6u, 0, NULL);
            if (e == 0x26 || e == 0x3E5) break;
        }
        if (k == 5) guest_stdcalln(sub_001A7A70, 0x001A83FDu, 0, NULL);
        InterlockedIncrement(&s_sw_n);
        oogie_cs_enter(&s_sw_cs);
        if (s_sw_seq == seq) OOGIE_MEM32(0x0040B864u) = 0;   /* else a newer request is waiting */
        LeaveCriticalSection(&s_sw_cs);
    }
}

/* int start_read(int i, void *buf, int size), cdecl */
void sub_001A8410(void)
{
    uint32_t idx = OOGIE_MEM32(g_esp + 4), buf = OOGIE_MEM32(g_esp + 8), size = OOGIE_MEM32(g_esp + 12);
    if (!sw_on()) { sub_001A8410_gen(); return; }
    if ((int32_t)idx < 0 || (int32_t)idx > 0x40 || !OOGIE_MEM32(0x0040B210u + idx * 4u)) {
        g_eax = 0xFFFFFFFFu; g_esp += 4; return;
    }
    if (InterlockedCompareExchange(&s_sw_started, 1, 0) == 0) {
        InterlockedIncrement(&s_sw_seq);
        sub_001A8410_gen();             /* creates the worker, as the game does */
        return;
    }
    oogie_cs_enter(&s_sw_cs);
    OOGIE_MEM32(0x0040B868u) = idx;
    OOGIE_MEM32(0x0040B86Cu) = buf;
    OOGIE_MEM32(0x0040B870u) = size;
    InterlockedIncrement(&s_sw_seq);
    OOGIE_MEM32(0x0040B864u) = 1;
    LeaveCriticalSection(&s_sw_cs);
    SetEvent(s_sw_ev);
    g_eax = 0;
    g_esp += 4;                         /* ret */
}

static int sem_entry(uint32_t i, uint32_t *e)
{
    if ((int32_t)i < 0 || i >= 0x40) return 0;
    *e = 0x0040C058u + i * 32u;
    return OOGIE_MEM32(*e) != 0;
}

/* #57 (2026-10-22, owner video 12:24, play.log 12:21): the waiter count
 * [+0x10] goes wrong for good when a release comes while no thread waits.
 * Release takes n off the waiters only if there are some; the wait adds one
 * and, finding the semaphore already signalled, passes at once without taking
 * it off again. The task is then running with [+0x10] = 1. The task runner
 * (sub_000583C0) releases a task and polls sub_001AA550 until the count [+4]
 * is 0 and the waiters [+0x10] > 0 ("it is waiting again"), so from then on it
 * takes every frame as finished at once, and the main thread draws its
 * overlay and swaps while the task is still drawing: the menu frame is shown
 * half drawn (trace f=9005..9067: 137 task draws after the Swap, then the
 * task's Clear, then the Swap; 200 reports). The state lasts until the menu's
 * task ends; it starts when the runner releases a just-created task thread
 * before that thread first reaches its wait, a race at thread start, so each
 * opening of a menu (which creates the task thread) could go either way:
 * "sometimes good, sometimes bad". Fix: a release that finds no waiter is
 * remembered (credit); the wait that consumes it takes itself off the waiters.
 * OOGIE_SEM_CREDIT=0: the original bookkeeping. OOGIE_TASK_WAIT_LATE_MS=N
 * (test only): a task thread's first wait comes N ms late, to force the race. */
static volatile LONG s_sem_credit[0x40];
static int s_sem_credit_on = -1, s_task_late_ms = -1;

/* Dev cycle 3 (2026-10-23, Verified wp1): the task runner (sub_000583C0)
 * polls sub_001AA550 in a loop with only a yield between polls until the task
 * it released waits again; each poll takes the game mutex [0x40C858]
 * (sub_001BD1CC -> NtWaitForSingleObjectEx, token 0x48000002): about 500 000
 * polls a second, one CPU core busy, and the same mutex the task's own
 * semaphore calls need (the owner's 15-hour session wrapped a log counter with
 * it). On the console the yield gave the CPU to the task. Here a poll that
 * finds the task running (count [+4] <= 0, waiters [+0x10] <= 0) sleeps until
 * a task reaches its wait (s_task_wait_seq, bumped in sub_001AA460) or 1 ms
 * passes; the result is then the same, only later. OOGIE_TASK_POLL_SPIN=1:
 * the original spin. */
static volatile LONG s_task_wait_seq;
extern void sub_001AA550_gen(void);
void sub_001AA550(void)
{
    static int spin = -1;
    uint32_t ret = OOGIE_MEM32(g_esp), outp = OOGIE_MEM32(g_esp + 8);
    LONG seq = s_task_wait_seq;
    if (spin < 0) { const char *e = getenv("OOGIE_TASK_POLL_SPIN"); spin = e && *e == '1'; }
    sub_001AA550_gen();
    /* Under the guest CPU lock the poll's own mutex calls already hand the
     * CPU to the (higher-priority) task, and a 1 ms sleep that missed its
     * wake cost every frame (lk_wait 52 fps, lk_spin 60): no sleep there. */
    if (!spin && !guest_cpu_held() && ret == 0x00058443u && g_eax != 0xFFFFFFFFu && outp
            && (int32_t)OOGIE_MEM32(outp) <= 0 && (int32_t)OOGIE_MEM32(outp + 12) <= 0) {
        uint32_t eax = g_eax;
        WaitOnAddress((volatile VOID *)&s_task_wait_seq, &seq, sizeof seq, 1);
        g_eax = eax;
    }
}
static OOGIE_TLS int s_sem_waited;
static void sem_credit_init(void)
{
    if (s_sem_credit_on < 0) {
        const char *v = getenv("OOGIE_SEM_CREDIT");
        const char *t = getenv("OOGIE_TASK_WAIT_LATE_MS");
        s_task_late_ms = t ? atoi(t) : 0;
        s_sem_credit_on = !(v && *v == '0');
    }
}

/* int wait(int i), cdecl */
void sub_001AA460(void)
{
    uint32_t i = OOGIE_MEM32(g_esp + 4), e, r;
    if (!sem_entry(i, &e)) { g_eax = 0xFFFFFFFFu; g_esp += 4; return; }
    sem_credit_init();
    if (!s_sem_waited) {
        s_sem_waited = 1;
        if (s_task_late_ms > 0 && OOGIE_MEM32(g_esp) == 0x00057E6Eu)
            Sleep((DWORD)s_task_late_ms);
    }
    {
        int fo = oogie_fo_slot();               /* #57: the step ends here */
        if (fo >= 0) InterlockedExchange(&s_fo[fo].in_step, 0);
    }
    sem_lock(1);
    OOGIE_MEM32(e + 0x10) += 1;
    sem_lock(0);
    InterlockedIncrement(&s_task_wait_seq);     /* wakes the runner's poll (sub_001AA550) */
    WakeByAddressAll((PVOID)&s_task_wait_seq);
    do {
        r = guest_stdcall3(sub_001BD012, OOGIE_MEM32(e), 0xFFFFFFFFu, 1, 0x001AA494u);
    } while (r == 0x102 || r == 0xC0);
    {
        int fo = oogie_fo_slot();               /* and the next one starts */
        if (fo >= 0) InterlockedExchange(&s_fo[fo].in_step, 1);
    }
    sem_lock(1);
    OOGIE_MEM32(e + 4) -= 1;
    if (s_sem_credit_on && s_sem_credit[i] > 0) {
        /* woken by a release that came before this thread waited */
        static volatile LONG n_cr;
        s_sem_credit[i]--;
        if ((int32_t)OOGIE_MEM32(e + 0x10) > 0) OOGIE_MEM32(e + 0x10) -= 1;
        if (InterlockedIncrement(&n_cr) <= 20) {
            fprintf(stderr, "[SEM] #%u released before its thread waited: waiter count kept right (#57)\n", i);
            fflush(stderr);
        }
    }
    sem_lock(0);
    while ((int32_t)OOGIE_MEM32(e + 4) < 0)
        oogie_yield();
    g_eax = i;
    g_esp += 4;                      /* ret */
}

/* int release(int i, int n), cdecl */
void sub_001AA3E0(void)
{
    uint32_t i = OOGIE_MEM32(g_esp + 4), n = OOGIE_MEM32(g_esp + 8), e, cnt, max, w, r;
    if (!sem_entry(i, &e)) { g_eax = 0xFFFFFFFFu; g_esp += 4; return; }
    sem_credit_init();
    sem_lock(1);
    cnt = OOGIE_MEM32(e + 4);
    max = OOGIE_MEM32(e + 8);
    if ((int32_t)cnt >= (int32_t)max) {
        static volatile LONG n_skip;
        sem_lock(0);
        if (oogie_wt_on() && InterlockedIncrement(&n_skip) <= 40) {
            fprintf(stderr, "[SEM] release of #%u (handle 0x%08X) skipped: count %d >= max %d, waiters %d, from 0x%08X\n",
                    i, OOGIE_MEM32(e), (int)cnt, (int)max, (int)OOGIE_MEM32(e + 0x10), OOGIE_MEM32(g_esp));
            fflush(stderr);
        }
        g_eax = 0xFFFFFFFFu; g_esp += 4; return;
    }
    w = OOGIE_MEM32(e + 0x10);
    {
        /* releases that no waiter took off [+0x10] (see sub_001AA460) */
        int32_t got = (int32_t)w > 0 ? ((int32_t)w < (int32_t)n ? (int32_t)w : (int32_t)n) : 0;
        int32_t room = (int32_t)max - (int32_t)cnt;
        int32_t add = ((int32_t)n < room ? (int32_t)n : room) - got;
        if (s_sem_credit_on > 0 && add > 0) s_sem_credit[i] += add;
    }
    if ((int32_t)w > 0) {
        w -= n;
        OOGIE_MEM32(e + 0x10) = (int32_t)w < (int32_t)n ? 0 : w;
    }
    cnt += n;
    OOGIE_MEM32(e + 4) = (int32_t)cnt > (int32_t)max ? max : cnt;
    r = guest_stdcall3(sub_001BCF74, OOGIE_MEM32(e), n, 0, 0x001AA440u);
    sem_lock(0);
    if (!r) {
        uint32_t esp0 = g_esp;
        g_esp -= 4;
        OOGIE_MEM32(g_esp) = 0x001AA449u;
        sub_001BC741();
        g_esp = esp0;
        g_eax = 0xFFFFFFFFu;
    } else {
        g_eax = i;
    }
    g_esp += 4;                      /* ret */
}

/* ── Oogie: XAPI SwitchToThread (sub_001BD332) ─────────────────
 *
 * Original: call NtYieldExecution; return (status != STATUS_NO_YIELD_PERFORMED).
 * sub_001A7BA0 loops "while (SwitchToThread()) alertable-wait 0 ms" to let
 * other guest threads and I/O APCs run. On a PC (and in the runtime, whose
 * NtYieldExecution always returns STATUS_SUCCESS) there is always another
 * host thread ready, so the loop never ends (run7c: 300k+ iterations).
 * Keep the yield and the alertable waits, but report "no yield" after 64
 * consecutive yields on this thread, as an idle Xbox would.
 */
/* A yield that lets a thread of any priority run. With the game's threads on
 * one processor (#61), SwitchToThread only gives way to threads of equal or
 * higher priority, so a polling thread starved a lower-priority task until
 * Windows' anti-starvation boost (1-2 s hitches, sk611/sk612). Every 8th
 * consecutive yield on a thread sleeps 1 ms instead. */
static OOGIE_TLS unsigned s_yield_run;
static BOOL oogie_yield(void)
{
    if (s_guests_pinned_flag && (++s_yield_run & 7u) == 0)
        return oogie_host_yield(1);
    return oogie_host_yield(-1);
}

/* KNOWN_ISSUES #66/#61 (2026-10-20): the scheduler's waker (sub_00057E00,
 * then the drain loop sub_001A7BA0, which also runs D3D's Swap) wakes a
 * worker and yields "until nothing else is runnable" -- on the console, until
 * the worker has parked again, so the two never ran at once. Here the drain
 * gave up after 64 yields and the waker went on with D3D while the woken
 * worker still ran its step (which also calls D3D: BlockOnResource, Swap),
 * and two threads wrote the command ring at once. While another woken
 * scheduler thread has not parked, a yield now reports "yielded" without
 * counting, for at most OOGIE_SCHED_YIELD_MS (opt-in, e.g. 30; default 0 = the old way),
 * since a worker blocked in a kernel wait would let the waker run on the
 * console too. */
static int s_sched_yield_ms = -1;
void sub_001BD332(void)
{
    static OOGIE_TLS unsigned s_run;
    static OOGIE_TLS DWORD s_wait_t0;
    BOOL yielded = oogie_yield();
    if (s_sched_yield_ms < 0) {
        const char *e = getenv("OOGIE_SCHED_YIELD_MS");
        s_sched_yield_ms = e && *e ? atoi(e) : 0;   /* opt-in: soaks with 30 went worse (sy1-sy4) */
    }
    if (s_sched_yield_ms > 0 && s_park_awake_n > 0) {
        int mine = oogie_park_slot(oogie_guest_tid());
        LONG others = s_park_awake_n - (mine >= 0 && s_park_awake[mine] ? 1 : 0);
        DWORD now = GetTickCount();
        if (others > 0) {
            if (!s_wait_t0) s_wait_t0 = now ? now : 1;
            if (now - s_wait_t0 < (DWORD)s_sched_yield_ms) {
                g_eax = 1; g_ecx = 1; g_esp += 4;
                return;
            }
            /* waited the full time: carry on as before until it parks */
        } else {
            s_wait_t0 = 0;
        }
    } else {
        s_wait_t0 = 0;
    }
    if (yielded && ++s_run < 64) {
        g_eax = 1;
    } else {
        s_run = 0;
        g_eax = 0;
    }
    g_ecx = g_eax;
    g_esp += 4; /* ret */
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
/* Oogie: print the guest call chain, i.e. stack words that look like return
 * addresses into game code (0x00011000 up to the kernel thunk table at
 * 0x0025F8E0, which covers .text, D3D, XAPI and the other code sections).
 * Same idea as the runtime watchdog's stack dump, filtered to code only. */
static void oogie_dump_call_chain(const char *why)
{
    uint32_t sp = g_esp;
    int shown = 0;
    fprintf(stderr, "[OOGIE] %s: guest esp=0x%08X eax=0x%08X ecx=0x%08X\n",
            why, sp, g_eax, g_ecx);
    for (int i = 0; i < 512 && shown < 32; i++) {
        uint32_t a = sp + (uint32_t)i * 4;
        if (a < 0x00010000u || a >= 0x04000000u)
            break;
        uint32_t w = *(volatile uint32_t *)((uintptr_t)a + g_xbox_mem_offset);
        if (w >= 0x00011000u && w < 0x0025F8E0u) {
            fprintf(stderr, "    [esp+0x%03X] 0x%08X\n", i * 4, w);
            shown++;
        }
    }
    fflush(stderr);
}

void recomp_icall_fail_log(uint32_t va)
{
    static int chain_dumped;
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);
    if (!chain_dumped && (va == 0xFFFFFFFFu || getenv("OOGIE_ICALL_CHAIN_ALL"))) {
        const char *stop = getenv("OOGIE_ICALL_STOP");
        chain_dumped = 1;
        oogie_dump_call_chain("first bad indirect call");
        if (stop && *stop && *stop != '0')
            abort();
    }

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ── Oogie: 16:9 widescreen (settings menu, Display > Aspect Ratio) ────────
 *
 * sub_001E4E86(out, fovy, aspect, zn, zf) builds the game's perspective
 * matrix (the shape of D3DXMatrixPerspectiveFovLH; Hypothesis from its
 * arguments). The camera code calls it with a fixed aspect of 4/3
 * (0x3FAAAAAB, sub_001A6D70 and sub_001AE4A0) and 1.2698 for a second
 * matrix. With 16:9 chosen, an aspect between 1.2 and 1.4 is widened by
 * (16/9)/(4/3): the 3D world is drawn for a wider view into the same 640x480
 * frame, and the back end shows that frame at 16:9 -- more of the world at
 * the sides, nothing stretched. Matrices with other aspects (1.0 for
 * shadows or effects) are left alone. The 2D overlay is drawn by other code
 * and is widened with the picture (Hypothesis; HUD correction to come). */
#include "pc_settings.h"
static volatile DWORD s_ws_tick;
float oogie_wide_aspect(void);       /* d3d11_backend.c: the window's shape */

int oogie_widescreen_active(void)
{
    return g_pc.aspect == 1 && GetTickCount() - s_ws_tick < 500;
}

void sub_001E4E86_enter(void)
{
    static int logged;
    uint32_t raw;
    float a;
    if (g_pc.aspect != 1 && g_pc.fov == 100)
        return;
    raw = OOGIE_MEM32(g_esp + 12);
    memcpy(&a, &raw, 4);
    if (a > 1.2f && a < 1.4f && g_pc.fov != 100) {
        /* Field of view (settings menu): the vertical angle, in radians at
         * [esp+8], scaled; the game's 4:3 cameras only. */
        float f;
        uint32_t rf = OOGIE_MEM32(g_esp + 8);
        memcpy(&f, &rf, 4);
        if (f > 0.05f && f < 3.0f) {
            f *= (float)g_pc.fov / 100.0f;
            if (f > 2.8f) f = 2.8f;
            memcpy(&rf, &f, 4);
            OOGIE_MEM32(g_esp + 8) = rf;
        }
    }
    if (g_pc.aspect != 1)
        return;
    if (a > 1.2f && a < 1.4f) {
        a *= oogie_wide_aspect() / (4.0f / 3.0f);
        memcpy(&raw, &a, 4);
        OOGIE_MEM32(g_esp + 12) = raw;
        s_ws_tick = GetTickCount();
        if (!logged) {
            logged = 1;
            fprintf(stderr, "[WIDE] camera aspect widened to %.4f (caller 0x%08X)\n", a, OOGIE_MEM32(g_esp));
        }
    }
}

/* ── Oogie: Swap path counters (KNOWN_ISSUES #51, menu flicker) ────────────
 * sub_001D69E0: D3D Swap that flips (FLIP_INCREMENT_WRITE + FLIP_STALL).
 * sub_001D6BD0: Swap's finish-only path (a lone FLIP_STALL, or one deferred to
 * the next SetRenderTarget). The back end prints both counts with
 * OOGIE_FRAME_LOG=1, to see whether flickering screens use the second. */
volatile long g_oogie_swap_flip, g_oogie_swap_finish;

void sub_001D69E0_enter(void)
{
    InterlockedIncrement(&g_oogie_swap_flip);
}

void sub_001D6BD0_enter(void)
{
    /* KNOWN_ISSUES #51: the GPU thread may run up to a frame behind the
     * game. The game then starts its next frame (clear and first layers)
     * in the buffer of the frame still waiting to be shown, and switches
     * buffers only when that flip arrives: menus showed half their layers
     * or black (ss3, sw5). The console's GPU keeps up, so at the end of
     * each frame the game waits here (at most 100 ms) until the executor
     * has taken everything submitted. OOGIE_NO_SWAP_SYNC=1: no wait. */
    static int off = -1;
    volatile uint32_t *put, *get;
    DWORD t0;
    InterlockedIncrement(&g_oogie_swap_finish);
    if (off < 0) { const char *e = getenv("OOGIE_SWAP_SYNC"); off = !(e && *e && *e != '0'); }  /* opt-in: did not help (sx1) */
    if (off) return;
    put = xbox_Nv2aRegPtr(0x800040);
    get = xbox_Nv2aRegPtr(0x800044);
    if (!put || !get) return;
    t0 = GetTickCount();
    while (*get != *put && GetTickCount() - t0 < 100)
        SwitchToThread();
}

/* ── Oogie: present interval at Swap (KNOWN_ISSUES #51) ───────────────────
 * 0x001E254C is D3D's presentation interval (1, 2, 4 = vblanks to wait,
 * 0x80000000 = immediate; set at sub_001D... from the present parameters).
 * With an interval, D3D's Swap (sub_001D6D20 -> sub_001D6BD0) defers the
 * FLIP_STALL to the next SetRenderTarget when the bound target is not its
 * back buffer, i.e. into the next frame. OOGIE_FORCE_IMMEDIATE=1 makes every
 * Swap use the immediate path: on by default since 2026-10-19 (im1-im6:
 * no deferred flips, no flashing menus); OOGIE_FORCE_IMMEDIATE=0 turns it
 * off. The back end paces to 60 Hz, so the game's speed is unchanged. */
void sub_001D6D20_enter(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("OOGIE_FORCE_IMMEDIATE"); on = !(e && *e == '0'); }
    if (on)
        OOGIE_MEM32(0x001E254Cu) = 0x80000000u;
}

/* ── Oogie: test switch, start at any chapter (dev cycle 4) ──────
 *
 * Verified (docs/RE_CHAPTER_SELECT.md, side session; text 0x40950):
 * sub_00040950(chapter) starts chapter 1..24 (argument at [esp+4]): sets
 * [0x413164], the world and area from the table at 0x2C5AA0, then the
 * chapter's set-up. OOGIE_START_CHAPTER=N (2..24): the first chapter start of
 * the session that asks for chapter 1 (New Game) asks for chapter N instead.
 * For the whole-game test runs; progress from earlier chapters (forms,
 * items) is not given, so later chapters may differ from a real playthrough. */
void sub_00040950_enter(void)
{
    static int target = -1, done;
    if (target < 0) {
        const char *e = getenv("OOGIE_START_CHAPTER");
        target = e && *e ? atoi(e) : 0;
    }
    if (target >= 2 && target <= 24 && !done && OOGIE_MEM32(g_esp + 4) == 1u) {
        done = 1;
        OOGIE_MEM32(g_esp + 4) = (uint32_t)target;
        fprintf(stderr, "[TEST] OOGIE_START_CHAPTER: New Game starts chapter %d\n", target);
        fflush(stderr);
    }
}

/* ── Oogie: the D3D fence wait gives the CPU up (dev cycle 4, #72) ─────
 *
 * Verified (text 0x1D78D0..0x1D7A13): the fence wait sub_001D78D0(fence)
 * hands the commands to the GPU (sub_001D7670, called from 0x1D79D3) and
 * then spins at 0x1D7A07 on the GPU semaphore *(device+0x30) with no call in
 * the loop, until ([device+0x2C] - fence) >= ([device+0x2C] - semaphore),
 * the same test the function makes on entry. Under the guest CPU lock that
 * spin held the CPU for the GPU's whole latency, so the game's other threads
 * could not use it (Casino Clash, ch17: 543-692 frames of 20-34 ms with the
 * lock, 337 without). When sub_001D7670 returns to that call site, this
 * thread now waits for the same condition with the CPU let go (100 ms at
 * most); the original loop then finds it met and ends, or carries on as
 * before. Not under the lock: nothing changes. OOGIE_FENCE_SPIN=1: the spin. */
extern void sub_001D7670_gen(void);
void sub_001D7670(void)
{
    static int spin = -1;
    uint32_t ret = OOGIE_MEM32(g_esp);
    sub_001D7670_gen();
    /* Opt-in (OOGIE_FENCE_YIELD=1): tried as the default, ch17 got worse
     * (c17fw: 765 frames of 20-34 ms, 17 stalls, against 423-692 / 7-10):
     * the time is not lost in this spin. */
    if (spin < 0) { const char *e = getenv("OOGIE_FENCE_YIELD"); spin = !(e && *e == '1'); }
    if (!spin && ret == 0x001D79D8u && guest_cpu_held()) {
        uint32_t dev = OOGIE_MEM32(OOGIE_D3D_DEVICE_PTR), fence = t_fence_wanted, sem_va;
        DWORD t0 = GetTickCount();
        uint32_t eax = g_eax, ecx = g_ecx;
        if (!dev) return;
        sem_va = OOGIE_MEM32(dev + 0x30);
        if (!sem_va) return;
        while (GetTickCount() - t0 < 100) {
            uint32_t cur = OOGIE_MEM32(dev + 0x2C), sem = OOGIE_MEM32(sem_va);
            if (cur - fence >= cur - sem)
                break;                          /* the GPU passed the fence */
            guest_cpu_part();
            SwitchToThread();
            guest_cpu_join();
        }
        g_eax = eax; g_ecx = ecx;
    }
}

/* ── Oogie: DirectSound voice parameters without register traps (#72) ──
 *
 * Verified (DSOUND.asm 0x20B53F..0x20B5D5): sub_0020B53F (thiscall, voice
 * object in ecx, no stack arguments) raises IRQL (sub_00203A90, ecx = a
 * local), and if the voice is active ([+0x12] bit 0) has sub_0020A3F2 (ret 4)
 * fill three arrays of [+0x64] values (locals at ebp-0x2C, -0x20, -0x14);
 * waits until the APU's free-FIFO register 0xFE820010 >> 2 is at least
 * count * 6; then for each sub-voice writes the voice index ([+0xC + 2i],
 * word) to 0xFE8202F8, 1 to the lock 0xFE8202FC, the three values to
 * 0xFE820360/364/368 and 0 to the lock; lowers IRQL (sub_00203AB2); eax = 0.
 *
 * Every one of those register accesses was an access-violation trap decoded
 * by the APU model (main.c veh_handler -> xbox_VehMmioFault). Casino Clash
 * calls this about 1,400 times a second (mm1_ch17, OOGIE_MMIO_STATS: up to
 * 162,383 traps in 10 s, 97,000 of them here; Chapter 16: 20,000 in all),
 * under the guest CPU lock. This version makes the same accesses, in the same
 * order, as direct calls into the model (mcpx_apu_mmio_read/write, what the
 * trap handler ends up calling). OOGIE_DS_NATIVE=0: the recompiled original. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *g_apu_state;
uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size);
void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size);
extern void sub_0020B53F_gen(void);
extern void sub_00203A90(void);
extern void sub_00203AB2(void);
extern void sub_0020A3F2(void);

void sub_0020B53F(void)
{
    static int on = -1;
    uint32_t self = g_ecx, esp0 = g_esp, frame, lock, cnt, i, spins = 0, sp;
    if (on < 0) { const char *e = getenv("OOGIE_DS_NATIVE"); on = !(e && *e == '0'); }
    if (!on || !g_apu_state) { sub_0020B53F_gen(); return; }
    frame = esp0 - 4;                   /* push ebp; mov ebp, esp */
    lock = frame - 8;                   /* [ebp-8], the IRQL helper's local */
    OOGIE_MEM32(frame - 4) = 0;         /* and [ebp-4], 0 */
    sp = frame - 0x2C - 4;              /* sub esp, 0x2c; push esi */
    g_esp = sp - 4;
    OOGIE_MEM32(g_esp) = 0x0020B554u;
    g_ecx = lock;
    sub_00203A90();                     /* raise IRQL; ret */
    if (OOGIE_MEM8(self + 0x12) & 1) {
        g_esp = sp - 8;
        OOGIE_MEM32(g_esp) = 0x0020B565u;
        OOGIE_MEM32(g_esp + 4) = frame - 0x2C;
        g_ecx = self;
        sub_0020A3F2();                 /* fill the three arrays; ret 4 */
        cnt = OOGIE_MEM8(self + 0x64);
        while ((uint32_t)(mcpx_apu_mmio_read(g_apu_state, 0x20010, 4) >> 2) < cnt * 6u)
            if (++spins > 1000) { SwitchToThread(); spins = 0; }
        for (i = 0; i < OOGIE_MEM8(self + 0x64); i++) {
            mcpx_apu_mmio_write(g_apu_state, 0x202F8, OOGIE_MEM16(self + 0x0C + 2 * i), 4);
            mcpx_apu_mmio_write(g_apu_state, 0x202FC, 1, 4);
            mcpx_apu_mmio_write(g_apu_state, 0x20360, OOGIE_MEM32(frame - 0x2C + 4 * i), 4);
            mcpx_apu_mmio_write(g_apu_state, 0x20364, OOGIE_MEM32(frame - 0x20 + 4 * i), 4);
            mcpx_apu_mmio_write(g_apu_state, 0x20368, OOGIE_MEM32(frame - 0x14 + 4 * i), 4);
            mcpx_apu_mmio_write(g_apu_state, 0x202FC, 0, 4);
        }
    }
    g_esp = sp - 4;
    OOGIE_MEM32(g_esp) = 0x0020B5D2u;
    g_ecx = lock;
    sub_00203AB2();                     /* lower IRQL; ret */
    g_eax = 0;
    g_esp = esp0 + 4;                   /* leave; ret */
}

/* ── Oogie: pure virtual call (R6025) survivable and traced (#78) ─────
 *
 * Verified (text.asm 0x1C9D48..0x1C9D5B): sub_001C9D48 is the C runtime's
 * _purecall: if the handler at [0x412CC0] is set it is called, then
 * _amsg_exit(25) (sub_001CE85C) prints "R6025 - pure virtual function call"
 * and bug-checks. Owner 2026-10-25 18:2x, Chapter 5, level 0102: it fired
 * eleven times; after the first the game kept running with a broken object
 * (calls to the wild address 0xC3AB0000, ten million of them), the pad no
 * longer read and Jack kept his last direction. A pure call means a virtual
 * method was called on an object that is being destroyed or is gone.
 *
 * This logs where it came from (the game code that made the call: the
 * return address on the guest stack; the object in ecx and its vtable; the
 * host call chain, whose sub_XXXXXXXX frames are the guest functions) and
 * returns 0 to the caller instead of ending the game. OOGIE_PURECALL_OLD=1:
 * the original. */
extern void sub_001C9D48_gen(void);
void sub_001C9D48(void)
{
    static int old = -1;
    static volatile LONG n;
    LONG k;
    if (old < 0) { const char *e = getenv("OOGIE_PURECALL_OLD"); old = e && *e == '1'; }
    if (old) { sub_001C9D48_gen(); return; }
    k = InterlockedIncrement(&n);
    if (k <= 20 || (k & 1023) == 0) {
        void *bt[24];
        USHORT nf = RtlCaptureStackBackTrace(0, 24, bt, NULL), f;
        uintptr_t mod = (uintptr_t)GetModuleHandleA(NULL);
        uint32_t ret = OOGIE_MEM32(g_esp), obj = g_ecx;
        fprintf(stderr, "[PURECALL] #%ld: pure virtual call from guest 0x%08X; this (ecx) 0x%08X, vtable 0x%08X; guest stack:",
                (long)k, ret, obj, obj >= 0x10000u && obj < 0x04000000u ? OOGIE_MEM32(obj) : 0u);
        for (f = 1; f <= 8; f++) fprintf(stderr, " %08X", OOGIE_MEM32(g_esp + 4u * f));
        fprintf(stderr, "\n[PURECALL]   host frames (exe offsets):");
        for (f = 0; f < nf; f++) {
            uintptr_t a = (uintptr_t)bt[f];
            if (a >= mod && a < mod + 0x4000000u) fprintf(stderr, " +%llX", (unsigned long long)(a - mod));
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    g_eax = 0;
    g_esp += 4;                         /* ret: the caller goes on, as if the method did nothing */
}

/* ── Oogie: D3D's vblank handler without the acknowledge spin (0.2) ───
 *
 * Verified (D3D.asm 0x1DB220..0x1DB2DC): sub_001DB220 (the vblank work, from
 * the DPC sub_001DB9D0 and five other callers) ends by acknowledging the
 * interrupt: "do { PCRTC_INTR_0 [0xFD600100] = 1 } while (PMC_INTR_0
 * [0xFD000100] & 0x1000000)". On the NV2A the write clears the pending bit
 * at once, so the loop runs once. Here the registers are memory held at zero
 * by the GPU thread between command batches (xbox_Nv2aAckBusyBits), so the
 * loop spun until that thread came round: 79% of the timer thread on a
 * two-core test (wkprof, scripts/weak_pc_test.ps1), and on two cores the spin
 * took the core the GPU thread needed, so it slowed what it waited for
 * (Casino Clash on two low-power cores: 18.7 fps). The PCRTC bit of PMC_INTR
 * is cleared as the handler starts, as the acknowledge would have left it
 * by the time the loop reads it. OOGIE_VBLANK_SPIN=1: the original. */
extern volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);
void sub_001DB220_enter(void)
{
    static int old = -1;
    volatile uint32_t *pmc;
    if (old < 0) { const char *e = getenv("OOGIE_VBLANK_SPIN"); old = e && *e == '1'; }
    if (old) return;
    pmc = xbox_Nv2aRegPtr(0x100);
    if (pmc && (*pmc & 0x1000000u))
        InterlockedAnd((volatile LONG *)pmc, ~(LONG)0x1000000);
}

/* ── Oogie: D3D KickOff without the flush-acknowledge spin (0.2) ──────
 *
 * Verified (D3D.asm 0x1D76F0..0x1D779F): sub_001D76F0 (KickOff, thiscall,
 * device in ecx, no stack arguments) hands D3D's commands to the GPU. In the
 * normal state ([dev+8] & 0x2000 clear): edx = [dev+8] & 4 ? [dev+0x770] :
 * [dev+0]; regs = [[0x1E25E8]+0x1C28] (the NV2A at 0xFD000000); PFB
 * 0x100410 |= 0x10000 and spin while that bit is set; then PUT
 * ([[dev+0x1C20]+0x40]) = edx & 0x0FFFFFFF; then, while [0x1E2140] is set,
 * sub_001D3C60 is called until it returns 0, and [dev+8] |= 0x2000.
 *
 * The 0x100410 bit is a flush request the hardware acknowledges at once.
 * Here it is memory, cleared by the GPU thread between its sleeps and command
 * batches (xbox_Nv2aAckBusyBits), so every KickOff waited for that thread to
 * be scheduled. On a two-core test (exprof: Casino Clash on two low-power
 * cores) the game thread spent 70% of its time in this spin while the GPU
 * thread slept 90% of its own. The flush is acknowledged here, as the
 * hardware would; everything else is as the game does it. The other path
 * ([dev+8] & 0x2000) runs the original. OOGIE_KICK_SPIN=1: the original. */
extern void sub_001D76F0_gen(void);
extern void sub_001D3C60(void);
void sub_001D76F0(void)
{
    static int old = -1;
    uint32_t dev = g_ecx, flags, put, regs, esp0 = g_esp;
    if (old < 0) { const char *e = getenv("OOGIE_KICK_SPIN"); old = e && *e == '1'; }
    flags = OOGIE_MEM32(dev + 8);
    if (old || (flags & 0x2000u)) { sub_001D76F0_gen(); return; }
    put = (flags & 4u) ? OOGIE_MEM32(dev + 0x770) : OOGIE_MEM32(dev);
    regs = OOGIE_MEM32(OOGIE_MEM32(0x001E25E8u) + 0x1C28u);
    MemoryBarrier();                                    /* sfence */
    OOGIE_MEM32(regs + 0x100410u) |= 0x10000u;          /* flush request ... */
    OOGIE_MEM32(regs + 0x100410u) &= ~0x10000u;         /* ... acknowledged, as the hardware does */
    g_ecx = OOGIE_MEM32(dev + 0x1C20u);
    g_edx = put & 0x0FFFFFFFu;
    OOGIE_MEM32(g_ecx + 0x40u) = g_edx;                 /* PUT: the GPU may run to here */
    g_eax = OOGIE_MEM32(0x001E2140u);
    if (g_eax) {
        do {
            g_esp = esp0 - 4;                           /* the original pushed esi */
            g_esp -= 4;
            OOGIE_MEM32(g_esp) = 0x001D7759u;
            sub_001D3C60();
        } while (g_eax);
        OOGIE_MEM32(dev + 8) |= 0x2000u;
    }
    g_esp = esp0 + 4;                                   /* ret */
}
