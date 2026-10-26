# The Oogie's Revenge PC Port Handbook

**Everything learned building a native Windows port of *The Nightmare Before Christmas:
Oogie's Revenge* (Xbox, 2005, title ID 42560004), for another port's developer and their
Claude.**

Compiled 2026-10-23 from Project Oogie, an XboxRecomp-based static recompilation built with
Claude Code. Shared freely to help your port.

---

## 0. How to use this document (for the Claude reading it)

- **What it is:**
  - Part 1 is a guided summary: what matters, in priority order.
  - Parts 2 to 7 are our full reference material: every problem we hit, its cause and its
    fix; our map of the game's code; the runtime fixes; and the PC features we built.
- **Code:** there is none here, only descriptions, addresses, causes and methods, so it can be
  used under any license. Where it names our own files (for example `port/src/d3d11_backend.c`
  or `recomp_manual.c`) or test names (`live23`, `fl1`, `ch1play2`), those are our references.
  You can't open them; read them as "where we did it" and "which run proved it".
- **Addresses:** all are virtual addresses in the retail `default.xbe`, SHA-1
  `667c5bd8abc4a551ef6b7cbab3bf3a45e7244605`. Verify your XBE's hash first. A different
  build (PAL, demo) moves everything.
- **How sure each claim is:**
  - **Verified** = proven by the code, the data or a measured run.
  - **Hypothesis** = not proven.
  - Keep that distinction when you act on it.
- **Our approach versus yours:**
  - We recompile the whole XBE, including the statically linked Xbox D3D8, DirectSound, XACT,
    Bink, WMA and XPP (USB) libraries, and emulate the hardware underneath: the NV2A GPU's
    pushbuffer and register combiners in a D3D11 back end, and the MCPX APU.
  - A high-level D3D8-to-D3D11 port replaces D3D8 instead.
  - Facts about **the game** apply to both. Facts about **the NV2A, the APU or the
    recompiler** matter to you only where you also emulate or recompile that layer, but they
    often explain *why* the game does something (for example, colour doubling in the
    combiners corresponds to D3D8's MODULATE2X texture-stage setup).

---

## Part 1. The guided summary

### 1.1 The ten facts that matter most
1. **The game logic steps once per frame, targeting 60 Hz.** (Verified)
   - The same scene took about 2,380 frames at any frame rate (62.9 s at 44.5 fps, 104.6 s at
     25 fps).
   - CreateDevice (call at 0x001AE538 to 0x001D87A0) uses 640x480 and presentation interval
     DEFAULT (one vblank).
   - So **any frame under 60 fps is slow motion**, and **uncapping makes films, music and logic
     run fast.**
   - Hold a steady 60. High frame rate = render-side interpolation only.
2. **Colours are stored at half intensity and doubled in the pixel pipeline.** (Verified)
   - PS2 heritage. Vertex colour 0x7F = full brightness.
   - The game's combiner setup computes r0 = v0 + v0, then r0 = t0 * r0 (D3D8 MODULATE2X).
   - Plain MODULATE gives a half-bright HUD, menus and surfaces.
3. **Alpha-only clears.** (Verified) After the scene the game clears only alpha (CLEAR_SURFACE
   0x80). Honour the colour write mask on clears, or the level renders black.
4. **Missed code freezes progress.** (Verified) Functions reached only through pointers or
   switch tables were missing from the recompile:
   - Halloween Door freeze after the Chapter 1 boss.
   - Pumpkin bombs that never explode in Chapter 3.
   - Plus about 140 more found by a scan.
   - (Recompilers only; see Part 2 #20 and #45.)
5. **`movsx r32, bp` mistranslated** (XboxRecomp v0.14.0 lifter): speckled 8x8 blocks in every
   Bink movie. (Verified; recompilers only; #35.)
6. **Menu flicker has two game-level causes.** (Verified)
   - D3D's deferred flip with the buffer rotation one step out of phase (#51, #57).
   - A semaphore waiter-count race when a new menu task starts (0x001AA3E0 / 0x001AA460).
7. **Music skipping and stutters are lock starvation, not "slow audio".** (Verified)
   - The render lock is held while waiting on the GPU (#55, #62).
   - DirectSound's voice-stop poll waits its full timeout (#55, #64).
   - Synchronous logging inside the XACT lock.
8. **The DirectSound DSP alive marker** must read 0x00CCCCCC at [0x00222100]. Otherwise
   DirectSound resets the DSP about 8 times a second with 10 ms busy-waits. (Verified, #33)
9. **Windows threading differs from the Xbox.** (Verified, #11-#13, #23, #29)
   - Suspended thread creation.
   - Handle access rights.
   - Thread IDs.
   - Lost wake-ups in the game's job scheduler.
   - More than 8 task threads in Chapter 1.
10. **Widescreen is a one-function hook.** (Verified) The camera is
    0x001E4E86(out, fovy, aspect=4/3, zn, zf). The 2D overlay is a pass-through vertex program
    in screen pixels, and the game paints its own black bands, which can be hidden.

### 1.2 Methods that found the hard bugs (reuse them)
- **Native-vs-recompiled differential test.** Capture guest RAM and registers on entry to a
  suspect function. In a 32-bit process laid out like the Xbox, run the original x86 bytes
  natively, run the recompiled C on the same state, and diff memory. Then "one function
  recompiled, the rest original" bisection names the guilty function.
  - Laying out a 32-bit process at the Xbox addresses: an image based at 0x10000 with a
    64 MB uninitialised section reserves the low range; the real logic goes in a DLL based
    high.
  - The contiguous mirror at 0x80000000+ needs a second copy.
  - This found the `movsx bp` bug in an hour after days of guessing.
- **Per-thread stack sampler.** Sample each thread's instruction pointer and callers
  (dbghelp plus the PDB) to see who waits on whom. This found the job-scheduler and
  thread-ID bugs.
- **Stall and lock-wait reporter.** When no frame arrives for 200 ms, dump every game thread's
  stack. When a guest critical section is waited on for 100 ms, name the holder and its stack.
  This found the music starvation and the lock chains.
- **Hitch log.** For every frame over 25 ms, log what that frame did: texture uploads, new
  textures, shader compiles, ring wraps. It ruled out texture and shader stutter quickly.
- **Frame trace.** For one whole frame, log every batch: target, texture, blend, depth,
  combiner words and first vertices. That's how the half-brightness doubling and the alpha
  clear were found.
- **Timing probe.** Boot to the same scene at two frame caps and time it, to tell
  "logic per frame" from "logic by clock".
- **Owner play logs:** the player's normal launcher writes a log, plus a crash or freeze
  report folder. Most real bugs came from the player's sessions, not scripted tests.
- **Log hygiene:** write logs through a background thread, and rate-limit repetitive lines. One
  repeated kernel-wait line grew a log to 10 GB overnight.

### 1.3 A suggested plan for your port (from what cost us the most time)
1. **Correctness first:**
   - MODULATE2X / combiner doubling.
   - Alpha-only clears.
   - Swizzled and render-to-texture glow targets.
   - Palettised textures (format 0x0B).
   - The DSP alive marker.
2. **Pacing:**
   - A steady 60 with a 60 Hz cap. No uncapped mode for logic.
   - Measure frame times and 1% lows, not average fps.
3. **Progress blockers:** play every chapter. Watch for freezes at item screens, objects that
   don't react (missing callbacks), and the results screen.
4. **Audio stability:** look at lock holders when music skips. Make sure DirectSound's DPC can
   finish voice stops while a game thread polls.
5. **PC features players expect:**
   - Widescreen with hidden borders.
   - HUD at the edges.
   - Render resolution.
   - AA and sharpening.
   - Rebinding.
   - PlayStation pads.
   - An in-game settings menu (F1 / Back+Start).
   - Saves and backups in the game folder.
6. **Every PC:**
   - Discrete GPU selection on hybrid laptops.
   - Device-loss handling.
   - Debug-layer-clean runs, and a WARP test.

### 1.4 Chapter map (Verified: code and data, see Part 6)
24 chapters. 0x00040950(chapter) starts one. Chapter at [0x00413164], world at [0x0041316C],
area at [0x00413170], table at 0x002C5AA0. Level files are `stm_data\WWAA.lz`.

### 1.5 Still unsolved on our side (we'd value your findings)
- Chapter 3 Lock/Shock/Barrel cutscene subtitles render as a coloured "barcode" (#63).
- The area-to-area loading transition is missing its side pieces (#60).
- The chapter title card shows Chapter 1's picture after a chapter (#49).
- The results-screen announcer skips lines (#47).
- Some sessions run with the GPU permanently behind (#62).

---


## Part 2. Every problem we hit, its cause and its fix (our KNOWN_ISSUES, complete)

_Source: our `docs/KNOWN_ISSUES.md`, included unedited except headings._

## Known Issues and Blockers

| # | Issue | Status | Notes |
|---|---|---|---|
| 1 | Main thread never leaves the `SwitchToThread` drain loop (sub_001A7BA0) | Fixed (run8) | Runtime `xbox_NtYieldExecution` always returns STATUS_SUCCESS; Wine's SwitchToThread is always TRUE too. Override `sub_001BD332` in `recomp_manual.c` with a bounded yield. Worth reporting upstream. |
| 2 | Software GPU path is slow | Improved | Wine/Xvfb: about 0.1 fps. Real Windows (win_run5): ~35 fps on the loading screen. The D3D11 back end (`nv2a_backend`) is still the real target. |
| 3 | Audio WAV capture silent so far | Open | APU runs and interrupts fire. win_run5 logs XAudio2 underruns, so the audio path is being driven. Not listened to yet. |
| 4 | Movies (`movie/*.bik`) | Extracted | 24 files extracted to `game/movie/` on 2026-10-16 (game folder now 614 files). Not yet requested by the game. |
| 5 | Template `recomp_manual.c` declared `g_eax` without thread-local storage | Fixed locally | Upstream template issue; report. |
| 6 | 106 "unimplemented instruction" sites (aas, arpl, outsb...) | Watch | Almost certainly data decoded as code. Only `wbinvd` (D3D cache flush) has been reached; harmless no-op. |
| 7 | Duplicate section name BINKYUY2 x4 | Cosmetic | disasm `.asm` listing overwritten; analysis unaffected. |
| 8 | Not yet run on real Windows | Done | win_run1 to win_run5. Windows behaved differently from Wine (thread timing), which exposed #11 to #13. |
| 9 | Wild indirect call through 0xFFFFFFFF after `door.stm` (session 1, Wine) | Fixed (win_run5) | On Windows the same failure showed as a crash on a worker whose stack was all 0xFFFFFFFF (win_run3). Root cause (Verified by the fix): the job scheduler could not suspend/resume its threads (#12, #13). No bad indirect calls in win_run5. |
| 10 | Movies not extracted | Superseded by #4 | |
| 11 | **Upstream:** `PsCreateSystemThreadEx` ignores `CreateSuspended` | Worked around (win_run12) | `kernel_bridge.c` `bridge_spawn_thread` calls `CreateThread(..., 0, ...)`. The game's job start (sub_00058540) needs the worker not to run until `ResumeThread`. Workaround: `sub_000584E0_enter` waits until job.func (0x3E3F7C) is set. The first version waited for job.state != 0, which another thread can set early (win_run11). A general fix belongs upstream. |
| 12 | **Upstream:** `NtDuplicateObject` with options 0 gives a handle with no access | Worked around (win_run4) | Xbox handles have no access mask. The runtime passes access 0 without `DUPLICATE_SAME_ACCESS`, so SuspendThread/ResumeThread fail with ERROR_ACCESS_DENIED (Verified with a standalone test). Workaround: `sub_001BC71B_enter` sets the SAME_ACCESS bit. Upstream fix: always use `DUPLICATE_SAME_ACCESS`. |
| 13 | **Upstream:** thread IDs not provided | Worked around (win_run8) | `PsCreateSystemThreadEx` never writes its ThreadId argument, and worker thread objects are zero-filled, so `[fs:[0x28]+0x12C]` (read by XAPI GetCurrentThreadId) is 0 for every worker. Workaround: `sub_001BD49C_enter` + `sub_001BD404_enter` assign matching unique IDs, and a full override of `sub_001BD50A` (GetCurrentThreadId) returns them from host TLS, because the +0x12C write alone did not stick (win_run7: the game-logic thread read 0 and stopped itself). |
| 14 | D3D GPU-position spin `sub_001D75F0` (PGRAPH 0x400B10 latch) | Hypothesis | win_run1 stalled there. The runtime models the latch only with `RECOMP_PB_WALK=dma`. Hook `sub_001D75F0_enter` sets it, but has not printed in any run since, so whether it matters is unproven. |
| 15 | Level load never finished; Now Loading forever | Fixed (win_run8) | Cause: the game-logic thread 0x0019D8E0 could not find its own ID in the scheduler table and stopped itself (#13). |
| 17 | Intro movie renders black | Fixed (mov2, mov3) | The D3D11 back end reproduces Bink's YUV pattern (Y on stage 0, two half-size chroma planes on stages 1-2, 7-stage combiner) and converts with BT.601 (`port/patches/backend_extra_stages`, `d3d11_backend.c`). Movies play in colour with subtitles. The block speckles that remained were a recompiler bug, #35. The CPU rasteriser path (`play.bat cpu`) still shows greyscale/black movies. |
| 18 | Movie and title run slowly | Open | ~5.7 fps during the movie (needs 29.97), so the 225 s intro would take ~19 min. CPU rasteriser plus Bink decode in recompiled code. Direct3D 11 back end is the planned fix. |
| 19 | Template `main.c` never starts the USB controller | Fixed (win_run17) | `xbox_OhciInit()` had no caller, so `RECOMP_USB=1` did nothing. Now called after the memory layout. `scripts/run_capture.ps1` sets `RECOMP_USB=1`. Upstream template issue. |
| 20 | Functions reached only through pointers | Ongoing | Seeded so far: 0x000791A0 (title-screen path, win_run8), 0x0025C900 (XPP controller driver, win_run17), 0x0008CFB0 and 0x0001C460 (gameplay, live23), and 12 library starts found by scanning for code right after "; end of function" (XPP 0x0025905C/0x0025BEAC/0x0025C2E1/0x0025D741, Bink colour converters, D3D/D3DX). 375 such blocks remain in `.text`; the ones checked are continuations inside functions, so they were not seeded. Use `scripts/add_seeds.py` + full regen for new runtime misses. |
| 21 | Title screen looks darker than expected | Not a bug | It was a fade frame; live1 captured the title at full brightness. |
| 23 | **Upstream:** only 8 guest thread stacks | Fixed by build-time patch (live2) | `XBOX_MAX_THREAD_STACKS 8` in xbox_memory_layout.c. Entering Chapter 1 a 9th task thread ran inline on the main thread and froze the game. `port/patches/thread_stacks` raises it to 32. |
| 24 | **Upstream:** CLEAR_SURFACE ignored its channel bits | Fixed by build-time patch (live7) | The game clears alpha only (0x80) after the scene; the runtime cleared whole pixels. `port/patches/clear_channels_32/_16`. With it the level becomes visible. |
| 25 | Level stays black after "Town Square / Fountain Area" | Worked around (live23) | Per-flip capture showed every finished frame black. Trace with raw inline vertices (live23): the last full-screen quad's colour comes from the game as `FF000000` (opaque black) and letterbox bars are drawn, i.e. the title is in an in-engine cutscene whose fade-in never starts; the emulation draws it correctly. Pressing Start skips the cutscene and gameplay fades in. Why the cutscene never starts is open (Hypothesis: it waits on an audio cue/voice or event that never completes; XAudio2 underruns throughout). Two missed functions were also running during gameplay (#20). A second cutscene after the first area showed the same black hold. **Update (live24/live25):** after seeding the two missed gameplay functions (0x0008CFB0, 0x0001C460) the opening cutscene plays instead of holding black, so the missing functions were the likely cause. Not yet confirmed over several runs. |
| 30 | Dark striped blocks over the gameplay image | Fixed by build-time patch (live25) | The glow passes render into 128x128 **swizzled** targets (surface format 0x07070228, type 2) and sample them back as swizzled textures; the runtime wrote them in row order. `port/patches/swizzled_surfaces_*` re-orders a swizzled target when drawing leaves it. Clean frame: `docs/cutscene_clean_live25.png`. |
| 31 | Pause/item menu shows black | Fixed (rc3) | It was never black: with the D3D11 back end the menu (ITEM / MAP / ACTION / INDEX / OPTIONS / EXIT, item slots, counters) draws but at half brightness (pz1, mean 9.5); the near-black ch1m frames came from a Start press that skipped a cutscene instead. With the combiners evaluated (#36) it shows at full brightness (`docs/pause_menu_combiners.png`, before: `docs/pause_menu_before_combiners.png`). |
| 26 | Level frame rate | Fixed (gv3) | Town Square: CPU rasteriser 1-2 fps -> D3D11 7-8 -> quiet logging 14-18 -> DSP watchdog fix (+ frame pacing) 22-26 -> parallel vertex programs + getenv fixes ~40 -> combiners ~35 -> **vertex programs on the GPU: 60 fps** (brief dips to 56). Each NV2A vertex program is translated to an HLSL vertex shader and cached (`port/src/d3d11_backend.c`, `port/src/oogie_gpu_vsh.h`, patches `vsh_access` and `backend_gpu_vsh`); `OOGIE_GPU_VSH=0` returns to the CPU interpreter. Programs that write c[] still run on the CPU. |
| 33 | DirectSound reset the stub DSP ~8.5x/s | Fixed (d3d7) | Entry hook `sub_0020856F_enter` writes the DSP alive marker 0xCCCCCC at [0x222100] (= 0xFE85A018); each reset was a 10 ms busy-wait. Found with `OOGIE_STALL_STATS=1`. |
| 34 | No frame pacing: game ran at 200+ fps | Fixed (d3d8) | The executor completes flips instantly. The D3D11 back end holds each flip to the next 1/60 s (`OOGIE_FPS_CAP`, 0 = off); `RECOMP_VBLANK=1` delivers the 60 Hz vblank interrupt (the game's ISR claims it). The game targets 60 Hz: its CreateDevice call (text 0x001AE4D2..0x001AE538, `sub_001D87A0`) passes 640x480 with FullScreen_PresentationInterval 0 = DEFAULT (one vblank) (**Verified** from the code; a later change of the interval was not looked for). See #37 for what a lower frame rate does. |
| 35 | **Upstream:** `movsx r32, bp` / `movsx r32, sp` lifted as a zero extension | Fixed at code generation (mov3) | Speckled 8x8 blocks in every Bink movie. `Lifter._lift_movsx` (tools/recomp/lifter.py) wraps 16-bit register sources in `SX16()` only for ax/bx/cx/dx/si/di; bp and sp fall through to a plain `LO16()` read (`_lift_movzx` lists all eight). In the Bink inverse DCT `sub_00235BA0`, `movsx ebx, bp` (0x00235C49) became `ebx = LO16(ebp)`, so negative coefficients became large positive ones. **Verified** with `tools/binkdiff` (captured decoder state, original x86 code run natively vs the recompiled C): 2,485 bytes / 39 of 4,800 blocks differed; with only `sub_00235BA0` recompiled and every other function original the same blocks differed (2,357 bytes), with any other single function recompiled none did; after the fix 0 bytes differ, and the speckles are gone on screen (`docs/movie_speckle_before_movsx_fix.png`, `docs/movie_clean_after_movsx_fix.png`). 16 sites in this XBE (8 Bink, 8 game code at 0x3813E, 0x382C8, 0x764FE, 0x76554, 0x7656F, 0x765D7, 0x76B5E, 0x76CD6; one more match at 0x24F70A is a float table, not code). Fix: `scripts/oogie_recomp.py` runs tools.recomp with a corrected `_lift_movsx` patched in memory; `scripts/regen.ps1` calls it. Upstream fix: add "bp", "sp" to the 16-bit list in `_lift_movsx`. |
| 36 | Everything drawn at half brightness with the D3D11 back end | Fixed (rc3) | Oogie stores colours at half intensity (vertex colour 0x7F7F7F7F = 1.0, a PS2 habit) and doubles them in the register combiners: frame trace pz1, stage 0 `icw C420C420 ocw 00000C00` (r0 = v0 + v0), stage 1 `C8CC0000 / C0` (r0 = t0 * r0). The back end only did texture x diffuse. Now every combiner program is turned into a small generated pixel shader, compiled on first use and cached (`port/src/d3d11_backend.c`, rules from the runtime's nv2a_combiner.c); the data it needs comes from the build-time patches `backend_extra_stages` and `backend_vertex_extra_*`. A first version that interpreted the program in one general shader ran the level at 3.2 fps on the Intel Arc 140V (indexed register array); the generated shaders run it at ~35 fps (39 without combiners). `OOGIE_NO_RC=1` turns the combiners off for comparison. Verified on screen: HUD, pause menu and level at full brightness (`docs/gameplay_combiners.png`, `docs/pause_menu_combiners.png`). |
| 37 | Game logic runs one step per frame: below 60 fps the game is in slow motion | Mostly fixed (gv3) | **Verified** (tc60/tc25): the Chapter 1 opening scene takes ~2,380 frames whatever the frame rate (62.9 s at 44.5 fps, 104.6 s at 25 fps). The game targets 60 Hz (#34). With vertex programs on the GPU the scene runs at a mean 58.9 fps and takes 46.8 s, and gameplay holds 60 fps (#26), so Chapter 1 now plays at full speed. Heavier scenes later in the game are untested; any frame under 60 fps is still slow motion. |
| 38 | Contiguous memory ran out after a level reload (crash in XACT) | Fixed by setting (ex4) | ex1: after a death and Continue, `MmAllocateContiguousMemory` for XACT (72 KB) was refused with 64.3 of 65.4 MB in use and the engine used the NULL result (`sub_002246F8+0xB2`). The runtime frees contiguous memory only with `RECOMP_HEAP_RECLAIM` (off upstream so that addresses stay comparable between runs); the game had freed nothing in 250 s. With `RECOMP_HEAP_RECLAIM=1` (now set by `scripts/live_start.ps1`, `play.bat`, `run_test.bat`; `OOGIE_NO_RECLAIM=1` in live_start to compare) usage stays near 50-54 MB and the same Continue works. **Hypothesis / watch:** with reuse, a new texture could land at the address of an old render target, which the D3D11 back end samples by address (`find_rt_by_va`); not seen so far. |
| 39 | Hitches the first time an area or effect appears | Improved (sc7) | Each new combiner or vertex program is compiled to a shader on first use (~25 ms). Compiled shaders are now kept on disk (`%LOCALAPPDATA%\ProjectOogie\shaders`; `OOGIE_SHADER_CACHE=0` disables), so each is compiled once ever. A brand-new install still compiles as it goes; shipping a pre-filled cache would remove that. |
| 40 | Owner: Jack stuck charging the Soul Robber, then no keyboard input at all (even on the Continue screen) | Fixed (keys_sync), not reproduced | Owner recording (`docs/owner_stuck_charge_sheet.png`): 111-159 s Jack holds the charged Soul Robber without moving; the Continue countdown then runs with no response. Holding the attack key and releasing it works in tests (virtual pad kb1; real key events through `scripts/key_send.ps1`, kb3), so the game logic recovers. **Hypothesis**: the key release never reached the game window: the owner was recording with the Xbox Game Bar, an overlay that can take the keyboard without the window getting WM_KILLFOCUS, so WM_KEYUP went to the overlay and the X button stayed held; menus that wait for all buttons to be released then ignore everything. Fix: once per frame the D3D11 window releases any key Windows says is not physically down, and releases all keys while another window is in front (`keys_sync` in `port/src/d3d11_backend.c`). |
| 41 | Owner: single black or white frames (Chapter 1 card, gameplay, title) | Fixed (pt6) | The back end presented "the largest surface drawn since the last flip". The game rotates three display buffers (0x104000, 0x35C000, 0x230000) but after finishing a frame sometimes draws into an off-screen 640x480 surface (0x00EA4000 / 0x00EC4000); trace pt3 frame 5480 presented that surface for one frame. Now a surface presented in the last 8 flips (a display buffer) wins; the old rule only when none qualifies (start-up). pt6: 6,128 frames, all from the three display buffers; the off-screen surfaces drawn but never shown. `OOGIE_PRESENT_TRACE=<flag file>` logs the per-frame choice. |
| 42 | Owner: title background movie and its music missing on one launch | Open, not reproduced | Owner recording: after the intro is skipped the title shows the logo and a blinking copyright line on black, never the movie or PRESS START (the title waits for its movie). 7 of 7 boots here were correct (`scripts/title_check.ps1`, plus fast repeated Start presses). Hypothesis: a timing-dependent stall in Bink file streaming on a cold start. `play.bat` now keeps the last three logs (play.log, play.1.log, play.2.log) so the next occurrence can be read. **2026-10-21:** likely cause found: Bink creates its streaming worker (sub_0022F3D0) CREATE_SUSPENDED and only then makes the mutex and event the worker waits on; the runtime starts threads at once (#11), so the worker could wait on an unmade handle (seen as handle 0xFFFFFFFF under the guest CPU lock). sub_0022F3D0 is now a C override that makes the mutex and event first (OOGIE_BINK_ORDER_OLD=1: original order). rh1-rh6: 6 of 6 soaks with all six film files opened. Hypothesis until the owner sees no more missing title films. |
| 43 | Crash in the game's USB driver (XPP) during long combat, and in a shop menu | Fix in the build, not yet proven | ch1a (after ~13 min, around a boss defeat and Continue): access violation in `sub_0025E6FB` reading guest 0x7FFFFFFF (an ED head of 0xFFFFFFF0 turned virtual). usb2 (~3 min, Jack taking hits, no Continue): `sub_0025BEAC` reading 0x84000000 via a misaligned TD pointer (phys 0x29D), followed by a crash in DSOUND `sub_0020A73C`. Both happened with `RECOMP_HEAP_RECLAIM=1`; no USB crash was seen in the earlier runs without it. Hypotheses: (a) the emulated controller (its own thread) writes an ED head back over a change the driver made meanwhile; build-time patch `ohci_ed_head_a/_b` now writes it back only if unchanged (did not stop usb2); (b) with memory reuse, memory the driver freed is reallocated while the emulator still touches it. Next: soak tests with and without reclaim (`scripts/soak_hits.ps1`), reclaim for contiguous memory only. sk3 (12:18 build, ring buffers, 8 min, 4 Continues, reclaim on): no crash. **New lead (2026-10-17 evening, Verified pattern):** all four crash logs of this kind (usb2, prof2, ld1, ls2) show, shortly before the crash, the APU's DSP doorbell helper report that the GP scratch SGE table (GPSADDR 0x744000) now names page 0x73C000 instead of 0x738000. DirectSound builds that table only in its init paths (`sub_0020D53D`, from `sub_00207B38`/`sub_0020EEF2`), so the change is an overwrite; the helper (`dsp_ack_auto_slot`, apu_dsp.c) then clears any non-zero dword at page + 0x810, i.e. writes zeros into memory it does not own. 21 runs with the sound lock: 2 such crashes; 12 under `RECOMP_WATCH=0x80744000` (`scripts/repro_loop.ps1`): none, so the writer is not caught yet. Defence: build-time patch `apu_doorbell_latch` keeps the first page and reports a change once instead of following it. **2026-10-18:** after the pushbuffer fixes (#46, which stopped the executor from running stale data as commands, some of which write guest memory) 8 soak runs (sx1-sx8, 2 min each, load save, play, pause) had no crash and no scratch-table change; one crash in fx2 (186 s): `sub_0025F390` (XPP OHCI driver) writing guest 0xFFFFFFFF. Verified from XPP.asm: it takes a transfer descriptor from the free list at [0x4334EC] via `sub_0025F2C6` (pop head, head = [td+0x14]) and writes into it at 0x25F42B; eax = 0xFFFFFFFF means a free-list link held -1 (an empty list would give 0), so the list itself was corrupted, the same family as before. Next: watch [0x4334EC] / the TD pool for the write that puts -1 there (`RECOMP_WATCH`). **2026-10-19:** the owner hit it again in a shop menu (play.2.log: `sub_0025E6FB+0x80`, guest 0x7FFFFFFF, ebx=0xFED00000). The emulated controller walks a queue on its own thread while the driver edits it on the game thread; a freed TD links to -1, and taking it made the head 0xFFFFFFF0 (Hypothesis, matches both crash dumps). Fix (patches `ohci_td_validate`, `ohci_head_writeback_check`): stop walking an ED once its sKip bit is set, leave a TD whose link is not a valid descriptor, and never write back a head that is neither a descriptor nor the tail. Builds and boots (st1-st4); not yet soaked long enough to call it fixed. **Evening:** another variant (sub_0025E6FB+0x4CC, esi = 0xFFFFFFF0, sk721): patch `ohci_head_repair` resets an ED head of 0xFFFFFFF0 to the empty queue (head = tail) each time the controller visits it, logged as "[OHCI] ED ... repaired". The origin of the -1 link is not found (Hypothesis: the driver takes the next link of a TD it has freed). **2026-10-20 night:** likely root cause in the emulated controller (ohci.c): the frame number register advanced with every 8 reads, and the new frame (HcFmNumber + StartOfFrame) began before the tick walked the lists, so a driver waiting for "the next frame" before editing a queue could edit it while the controller was still walking it. Patches usb_frame_read (reads return the tick counter) and usb_frame_after_a/_b (the frame starts after the walk); OOGIE_USB_FRAME_OLD=1 for the old way. mo1-mo4: no USB crash (dn4 crashed before this patch). Hypothesis until more soaks. **2026-10-20 06:50 (Verified, logs/fin1.err.log):** the frame-clock patches do NOT end it: fin1 (default build) crashed at 34 s during the intro film, right after a scripted A press, in sub_0025F390+0x49C (write to 0xFFFFFFFF, eax=0xFFFFFFFF, ebx=edx=0x806E25E0, stack through 0x0025F76F / 0x0025F88E / 0x0025DFC0 / 0x0025B37D). Since the patch: 1 USB crash in 9 runs (mo1-4, fz1-2, fin1; gy/gz excluded). Still open. **2026-10-20 midday, new fix (Hypothesis, strong):** after the regen, 3 USB crashes in 9 runs (fin1, og1 sub_0025F390+0x49C; og2 sub_0025E6FB+0x4CC). The emulated controller never writes [td+0x14] (ohci.c writes only TD +0/+4/+8, ED heads and the HCCA), so the -1 free-list link is written by guest code. XPP guards the free list ([0x4334EC]: pop sub_0025F2C6, push 0x25E000, 0x258D31) with KeRaiseIrqlToDpcLevel / KfLowerIrql (about 20 pairs in XPP.asm, thunks [0x25F9DC] / [0x25F9D8]), i.e. against its own DPC. The runtime ran that DPC on the timer thread alongside a raised game thread. Patches usb_dpc_lock_a/_b (kernel_bridge.c): a guest thread raising to DISPATCH through the kernel holds a lock until it lowers; a DPC whose routine is in XPP (0x257F40..0x25F9E0) takes it first (waits at most 50 ms, logged "USB DPC ... ran with a thread still at DISPATCH_LEVEL"). DirectSound is untouched (its raises bypass the bridges; #55). OOGIE_USB_DPC_LOCK=0 turns it off. ud1-ud6 (test settings, 4 min, 2 Continues each) and uf1-uf3 (owner settings): 9 runs, no crash, no late DPC. |
| 44 | Process memory grows in levels (~4 MB/s) | Fixed (ch1play2) | sk1/sk2 with `OOGIE_MEM_STATS=1`: private memory ~400 MB in menus, then +1.4 GB at the level load and up to 3.4 GB in 6 min (6.9 GB after ~20 min). The back end's textures are only ~32 MB. `scripts/memmap.ps1` on a running game: within one minute 17 new private blocks of exactly 64 MB appeared (+1.1 GB), the graphics driver's pool for renamed buffers. **Hypothesis (matches the pattern)**: the back end refilled its vertex, extra and index buffers with MAP_WRITE_DISCARD for every draw (~2,000 a frame, buffers of 256 KB and up), so the driver kept a renamed copy of each whole buffer until the GPU finished. Fix: ring buffers (`Ring`, `ring_write` in `port/src/d3d11_backend.c`): append with MAP_WRITE_NO_OVERWRITE, discard only on wrap; the vertex-program input buffer passes its start in `rt.w`. **Verified** in the owner's 18-minute ch1play2 run on the 12:18 build: private memory stayed at 280-650 MB (previously 3+ GB). |
| 45 | Switch tables whose case code no function covers (the Halloween Door freeze) | Fixed (ch1play2) | After beating the Chapter 1 boss the Halloween Door item screen froze with 1,777 "ICALL Failed to resolve VA 0x0006A2F8". The disassembler ended sub_0006A2E0 right after its `jmp [eax*4+0x6A308]` (19 bytes) and made the default case's `ret` a function of its own (sub_0006A307), so the table and its five case stubs belonged to no function; the other session seeded 0x0006A2F3, 0x0006A2F8, 0x0006A2FD, 0x0006A302 and a rarer miss 0x001CC29C (icall_seeds.json) and rebuilt (12:08 build; owner session log `logs/ch1play*`). General: `scripts/scan_switch_tables.py` finds 25 more switch sites with uncovered targets, most inside code gaps after functions recorded too short (sub_000622A0 "ends" at 0x637D3, its code runs to 0x63D98; never recompiled). `scripts/gap_seeds.py` proposed 128 seeds (gap starts + uncovered targets, each decoding as code); after the 12:08 rebuild 110 were still needed and were added (`--apply`, 136 seeds in all); full regen 12:18: 10,526/10,530 functions (was 10,387), 0 failed. ch1play2 (owner, 18 min on the 12:18 build): past the boss and the door screen into the next level group (stm_data/0100.lz), 0 "ICALL Failed" lines. Upstream: function end detection stops at an indirect jmp / inside a function with a later-reached block. |
| 46 | Owner: flickering artifacts coming off characters and the environment | Fixed (fx1-fx4, fv1-fv2), owner to confirm | Owner recording 20:43 (Chapter 2, `logs/vid46.mp4`, `scripts/video_flashes.py`): single frames where the whole scene is washed white (fogged) or uniformly dark while the HUD is normal. Reproduced by script (`scripts/flash_hunt.ps1`: load the save, Chapter 2, walk and attack, detect frames unlike both neighbours): 1-10 a minute around the Town Hall. **Cause (Verified):** the runtime's default pushbuffer reader, the segment scan (`nv2a_pb_scan.c` `pb_walk_segment` + the wrap handling in `xbox_memory_layout.c`), lost commands two ways: (1) on a ring wrap it scanned the old piece only up to the highest PUT seen so far, so whatever D3D wrote past it before its JUMP back to the ring start was never executed; (2) a method whose parameters ran past PUT lost the rest of them, and the next segment read those parameters as command headers. Found with per-draw records (`OOGIE_FLASH_MEASURE=1`: every draw's target, texture and content hash, combiner, fog colour, blend, vertex program and start slot, for the last six frames; the presented picture's brightness read back asynchronously, since a pipeline stall made the flashes rare; `logs/drawdiff_<frame>.txt` per flash, compared by `scripts/drawdiff.py`): in white frame fn41/8215 the first ~250 scenery draws used vertex program B1654753 (the screen-overlay program) instead of 675A4159, i.e. the SET_TRANSFORM_PROGRAM_START was missed and the scene was transformed and fogged by the wrong program; in dark frame fn51/3883 one full-screen draw was missing. **Fix:** build-time patches `pb_segment_carry` (a method cut by PUT is finished by the next segment, as the DMA walk's s_pend_left does; a top-level walk that ends on a JUMP records its target) and `pb_segment_wrap` (on a wrap, walk the old piece to D3D's JUMP and continue from its target; the old guess only if no JUMP is found, logged). After: 0 flash frames in 4 two-minute measured runs (fx1-fx4; before, fn41 10 and fn51 6 in gameplay) and 0 in 2 one-minute screenshot runs (fv1, fv2; before 7, 2, 1). The menu and loading-screen blink frames the detector used to report are gone too. Diagnostics left in (off by default): `OOGIE_FOG_TRACE`, `OOGIE_FLASH_MEASURE`, `OOGIE_VC_DUMP`. `OOGIE_PB_WRAP_OLD=1` restores the old wrap guess (not meaningful with the carry patch). **Hypothesis:** the garbage commands of (2) may also explain the rare memory corruption of #43 (some NV2A methods write guest memory). Upstream: both are bugs in the segment scan; the DMA walk already handles split methods. Final build (fz/fq runs): 0 in gameplay in 7 more runs. Residual, unexplained: fz1 showed Chapter Select alternating black/normal frames for ~200 frames while the script kept pressing A on the chapter-start dialog (the black frames lack the first background-panel draws); not seen in 6 other runs. |
| 47 | Owner: results screen skips the announcer for Combo, Damage and "!" counts | Open | After clearing a level the ranking lines (clear time, combo, damage, exclamation count, Nightmare rank) appear but the announcer voice for some is late or missing. **2026-10-21:** needs a reproduction: the results screen only follows a chapter end, which the scripted routes cannot reach; the owner's 11:07 session has none. Lead (Hypothesis): the screen waits for each announcer line to report finished before the next, and a voice end reported late by the emulated APU (the same weakness as #55) makes it skip lines. Asked the owner for a save just before a chapter's end. |
| 48 | Owner: music volume pumps up and down (e.g. the Oogie shadow fight) | Fixed (au14-au17), owner to confirm in the Oogie fight | **Cause (Verified):** two guest threads programmed sound voices at the same time. DirectSound selects a hardware voice (PIO 0x2F8) and then writes its volumes (0x360-0x368); it guards that only by raising IRQL to DISPATCH_LEVEL (`sub_00203A90`/`sub_00203AB2`; XACT has the same pair at `sub_002227D2`/`sub_002227F4`), which on the single-CPU Xbox stops every other thread. The runtime keeps IRQL per host thread, so nothing was stopped. Seen with `RECOMP_APU_BINS=1` (mixbin levels, voice route changes, and hooks on SetMixBinVolumes `sub_00204155` and the volume calculation `sub_0020A3F2`): the Town Square music (a 3D-positioned stereo voice, hw 75, mixbins 0/1) was given the volume words of an effect routed to mixbins 10/3/31 ("front left and centre, front right muted"): right channel gone and level following the effect, in 5 of 6 runs (au1, au5, au7, au8, au9; au4 clean). `[APU-XTHREAD]` (patch `apu_fecv_trace_*`) counted volume writes from a thread other than the one that selected the voice: 7 in au13. **Fix:** `recomp_manual.c` replaces the four raise/lower helpers with the same logic plus one shared host critical section, taken also by threads already at raised IRQL (the runtime's DPC/ISR threads, which a single CPU would never run beside a raised thread). After: 0 cross-thread writes and the music volume steady at 31A/31A in au14, au15, au16. Separately the stub mixdown (`apu_dsp.c`, all even mixbins to the left, odd to the right) put the centre and effect sends (2, 10..31) on the left only, a 3-5 dB lean; build-time patch `apu_mixdown_stereo` sends speaker bins to their side, centre/LFE to both at -3 dB, effect sends to both at -6 dB (au17: balance 0.2 dB, was 0.8-3.8 dB; no added clipping). `OOGIE_NO_DS_LOCK=1` and `RECOMP_APU_MIXDOWN_EVENODD=1` restore the old behaviour. Not yet checked in the Oogie fight itself; may also explain #47. Upstream: the IRQL model gives no mutual exclusion; any XDK library that relies on raised IRQL for it races. |
| 49 | Owner: every chapter title card shows the Chapter 1 ("Jack's Return") picture | Open | Reported after Chapters 1-2. ld1 (Chapter 1 save loaded, Chapter 2 chosen on Chapter Select): the Chapter 2 card shows the correct Chapter 2 picture (`logs/live/ld_h_360.png`), as does the Chapter Select preview. So the art itself is found; the wrong picture needs something that happens in a continuous session (Chapter 1's card shown first). **Hypothesis**: the D3D11 back end shows a stale copy of the first card: a render target lives on after the game stops drawing into it, and `get_texture` answers any texture at that address with it. Fix (in the build, unconfirmed for #49): `rt_replaced` keeps a sampled hash of guest memory under each target at the flip after its last draw; a target never presented and not drawn for 8 frames whose memory has changed since is dropped and the texture decoded from guest memory (`OOGIE_KEEP_STALE_RT=1`: old behaviour). The owner confirmed (2026-10-17) that a Chapter 2 card from a fresh load is correct. **Verified overlap (fn4x draw records, 2026-10-18):** the Chapter 2 card is drawn from textures at 0x00E64000 and 0x00EA4000, and 0x00EA4000 is exactly the 128x128 glow render target the game draws into every gameplay frame (post-processing chain, #46 notes). After any gameplay in the same session that render target exists, so without `rt_replaced` the card's picture was answered with the old surface; on a fresh load (no gameplay yet) the texture is decoded from memory and is right, which is what the owner and ld1 saw. Still to test: Chapter 1 -> Chapter 2 in one session (Chapter Select does not move off the next chapter with up/down by script). |
| 50 | Game hangs (picture frozen, pad no longer read) after the pause menu | Fix in the build, mechanism verified, fix not yet proven by A/B | cc3, cc5 (2026-10-18): load the save, Chapter 2, pause, EXIT (closes the menu), pause, EXIT again: one more press is read, then none; the picture freezes, streaming continues. `OOGIE_SAMPLE_SECS` stacks: the main thread in `sub_000583C0` (0x5845B: waits for a game task to end), the logic thread `sub_0019D8E0` blocked in `sub_001AA460` (0x1AA494) on its semaphore; only those two guest threads alive. **Verified** (text.asm 0x1AA3E0-0x1AA4C0): the game wraps 64 semaphores in a table at 0x40C058 (handle, its own count, maximum, waiters); release (`sub_001AA3E0`) skips the real ReleaseSemaphore when its count is at the maximum, and wait (`sub_001AA460`) does `inc` waiters and `dec` count with plain read-modify-write and no lock, so threads running in parallel lose updates; once the count reads "full" the releases stop and the waiter never wakes. Fix: both functions rewritten in `recomp_manual.c` with the bookkeeping under one host lock (the wait itself outside it); the real semaphore calls go through the recompiled XAPI functions (`guest_stdcall3`). cc6 (same sequence): still responsive. `scripts/menu_stress.ps1` (15 pause/EXIT cycles) stayed responsive both with the fix (ms1) and with the lock disabled by `OOGIE_NO_SEM_LOCK=1` (ms2), so the race needs other timing and the fix is not yet confirmed by A/B. Earlier "pad stopped being read" in cc1 (blamed on the owner's keyboard) may have been this. |
| 51 | Owner: menus flash, jitter and show partial layers (results screen, inventory/pause menu, main menu after the title film) | Fixed (Verified im1-im6, fz) | **Root cause (Verified):** D3D's Swap here ends frames through its finish path. With a presentation interval (0x001E254C = 1) it pushes the FLIP_STALL at once only if the bound target is its back buffer; otherwise it defers the stall to the next SetRenderTarget (sub_001D6BD0, sub_001D3210). In some sessions (about 1 in 6, more when frames are slow: the owner's 400% render scale) the game's buffer rotation is one step off D3D's for the whole session, so every flip landed after the next frame's clear and first quads. Each buffer then held the tail of one frame plus a clear and the start of the next, and the screen showed black or a few layers. OOGIE_FRAME_LOG=1 shows it per flip: bad `D104*134 C104 D104`, good `C104 D104*135` (ss3, sw5). **Fix:** an entry hook on Swap (sub_001D6D20) sets the interval to immediate (0x80000000), so D3D pushes the stall at the frame's end. The back end paces to 60 Hz, so game and film speed are unchanged (intro film mv_00 234.6 s for 225.8 s of frames, mt6; title film 60 fps). Result: im1-im6, slowed by OOGIE_TEST_SLOW=12, had zero flips in the bad pattern (before: every session at boot, and about 1 in 6 sessions in menus). OOGIE_FORCE_IMMEDIATE=0 restores the original. **Also in the build:** (a) the surface width comes from the pitch, because clip zooms (645x483 ... 1886x1414) recreated the screen buffers; (b) a finished frame is copied aside if its buffer is cleared before the flip (OOGIE_NO_CLEAR_SNAPSHOT=1 turns this off); (c) a deferred flip that drew two buffers shows the first; (d) a draw that samples its own target gets a copy (none seen). **Tried and dropped:** frame-locked vblank (did not help), waiting for the GPU at Swap (sx1, did not help). Tools: scripts/inv_cycle.ps1, scripts/inv_hunt.ps1, OOGIE_FRAME_LOG (with the method ring dump), OOGIE_TEST_SLOW. |
| 52 | Owner: Load Game menu unresponsive, flickering, saves missing | Fixed (Verified) | Programs started from the Claude desktop app see a private copy of AppData (MSIX virtualisation), so test runs and the owner's own runs used different save folders. `play.bat` now sets `RECOMP_SAVE_DIR=saves` in the project folder, and copies newer saves over from the old AppData folder. Test scripts use `saves_test`. The Load Game screen held steady for 90 frames in lg1 (2026-10-18, no flicker). |
| 53 | 16:9 widescreen: 2D overlay placement | By design for now | In 16:9 the 2D overlay (pass-through vertex program, screen pixels) is squeezed by 3/4 about the centre, so it keeps its shape but sits inside the middle 4:3 area (hud5, Verified). Draws that span the full width or reach off-screen (edge masks at x -20..0 and 640..660) are not squeezed. A first rule squeezed the edge masks too and drew dark stripes inside the picture (hud1); fixed. A frame is shown 16:9 only when it drew 3D into a frame buffer. Deciding it from the time of the last camera call failed on the Continue screen, whose camera is not rebuilt every frame (hud2). Refinements: anchor left and right HUD pieces to the screen edges; check for objects culled at the new side areas (Hypothesis, none seen). |
| 32 | Direct3D 11 back end limits | Open | Register combiners are evaluated (#36). Texture stage modes bump-env and dot-product read zero and cube maps are sampled as 2D (as in the runtime's CPU path); fog uses the per-vertex factor but has not been checked against a reference. No culling (left to the transform). Game CPU reads of render targets would see stale guest memory (none seen yet). |
| 28 | Intermittent host crash in the GPU executor during the title movie | Fixed by build-time patch (d3d3) | Named by the PDB: `pb_walk_segment+0x5F` reading guest 0x8F000000; the segment walk followed a JUMP/CALL target past the 64 MB window. `port/patches/pb_segment_bounds_*`. Earlier note: | live11, live12: access violation reading guest 0x8F800000 (past the 64 MB contiguous window) on a non-guest thread, right after Start on the title (`mv_title.bik` playing). Not seen in live7-live9. Release builds now write a PDB (`/DEBUG`, `/Zi` for xbox_kernel) and `bin\oogie_recomp.pdb` is copied, so the next occurrence names the function. |
| 29 | Intermittent boot stall while reading `door.stm` (lost wake-up) | Fixed (live19, live20) | live4, live17, live18: Now Loading forever. live18 samples: game-logic thread parked at 0x001AD4E8 (scheduler park, sub_001AD4B0) and never woken. The waker's ResumeThread arrived before the parker's SuspendThread, which Windows ignores; on the Xbox's single CPU that order cannot happen. Fix: full overrides of sub_001AD4B0 (park) and sub_001AD4F0 (wake) using a per-thread auto-reset event, which remembers an early wake. The bounded SwitchToThread override (#1) is what opens this window; the event makes it harmless. |
| 27 | Live-session timing depends on CPU load | Note | Several game copies running at once shifted menu timings and may have caused one boot stall (live4). Run one copy at a time. |
| 22 | Save data | Fixed / verified (owner, 2026-10-17) | The owner saved after Chapter 1 (at Mr. Hyde): the game wrote `UDATA/42560004/D5AD83CA26E2/` with SaveMeta.xbx and data.sav under `%LOCALAPPDATA%/xboxrecomp/`. Loading the save back not yet confirmed. |
| 16 | `xbox_kernel.log` can grow huge | Watch | The runtime writes it in the working directory; win_run4 produced 1.3 GB of repeated suspend errors. Overwritten each run. `RECOMP_FB_DUMP` BMPs also land in `C:\ProjectOogie\`. |

| 54 | Freezes: the game stops responding (owner 09:55 in the title film; sk58e at boot) | Fix in the build (safety net, Verified by fault injection) | The [STALL] sampler caught it (sk58e): the game thread spinning in D3D's fence wait `sub_001D78D0` (last loop, 0x1D7A07: "until the GPU semaphore reaches the fence") while the GPU thread had nothing to do, so the semaphore release for that fence never came (Hypothesis: lost or never handed over). Not reproduced in 8 more boots (bl1-bl8). Fix: `oogie_gpu_unstick` (recomp_manual.c), called by the stall sampler once no frame has been shown for 1 s: with the GPU idle (GET == PUT) it hands the GPU any commands D3D wrote past PUT, or, when everything submitted has run, writes the fence value as the release would have. Proven with OOGIE_TEST_DROP_SEM=600 (every release from the 600th dropped, ds3): the game kept running on the safety net (35 actions) instead of freezing. OOGIE_NO_UNSTICK=1 reports only. Also: emulator semaphore releases are dropped until the first fence wait tells it where the semaphore is (nv2a_pb_exec.c, s_sem_va). **Evening:** the safety nets are restricted to D3D's normal state ([dev+8] & 0x2004 clear): KickOff hands over [dev+0x770] instead of [dev+0] while a command buffer is being recorded. A quick kickoff (two 50 ms samples) handles the common case (a game task waiting on a fence whose commands were written but not handed over, GPU idle). Semaphore watch: "[GPU] semaphore ... went back" logs a fall of the GPU semaphore (bsA2 showed it at 0 in the menus once). |
| 55 | Owner: short freezes ("lag") while the counter reads 60 | Fixed (Verified sk58d, sk58e) | Three causes, all found with the [STALL] sampler and the new [CS] lock-wait report (patch `cs_wait_report`: a guest lock waited on for 100 ms names its holder and the holder's call stack): (1) logging: one log line and a flush per disc read ([READ], now off unless OOGIE_READ_LOG=1, patch `kernel_read_quiet`) and every other log line written to disk on the thread that logged it, so a slow write held DirectSound inside the XACT lock (sk58c: 234 ms). The log now goes through a 4 MB pipe to its own writer thread (`port/src/pc_log.c`; the crash handler waits for it; OOGIE_SYNC_LOG=1 restores direct writes). (2) a disk check for a debug flag file every 16 frames on the render thread (now only with OOGIE_TEXPROBE=1). (3) the half-second freezes: DirectSound stops a voice by polling "while (voice[+0x12] & 0x8000)" (sub_0020B669), which only the APU interrupt clears, and gives up after 500 ms (sub_0020AA01, 0x4C4B40 x 100 ns). The poll runs inside the raised-IRQL lock of #48 and the interrupt needs the same lock, so every voice stop cost 500 ms with the game thread blocked behind XACT (owner play.log: 7 of them, 500-523 ms). sub_0020B669 is replaced: while it polls, an ISR (IRQL above DISPATCH) may enter DirectSound without the lock, as the console's interrupt cuts into that loop; no other thread may. (A first version released the lock for any thread and XACT crashed on a chapter load, lt3.) OOGIE_DS_STOP_OLD=1 runs the original. Results, same 10 min combat soak: hitches 179/251 (sk58b/c) -> 12 (sk58d, logging fixed) -> 0 (sk58e); frames over 100 ms 56/54 -> 4 -> 0. **Update (evening):** the pipe log writer (pc_log.c) is now opt-in, OOGIE_FAST_LOG=1: with it the threads overlap more and the render-list race of #61 froze both soaks within 8 min (bsC1/bsC2), while the same build with direct log writes ran clean (bsD1/bsD2). So the shipped build has the voice-stop fix (no half-second freezes) but the morning level of shorter hitches (about 20 a minute over 25 ms in combat). Also added: a KickOff ack thread (0x100410 bit 16 only; OOGIE_ACK_THREAD=0 turns it off): the game waited in D3D KickOff (sub_001D76F0) while the GPU thread was busy (100-460 ms, sk60d). **2026-10-20 13:50 owner session (Verified, logs/owner/play_20261020_1350.log):** after walking Town Square <-> Hinterland several times, 19 freezes of 470-530 ms in a row: an XACT thread polling in sub_0020B669 (via sub_002258BD / sub_002055B1) and each voice stop timing out at 500 ms; then a crash in XACT sub_002246F8+0xB2 (list walk through a node whose link was 0: eax = 0 - 0x28). Fixes: (1) a stop that has waited OOGIE_DS_STOP_MS (default 50 ms) gets its start time [+0x78] moved back, so sub_0020AA01 takes DirectSound's own forced-stop path sub_0020A918 (counted by DirectSound at [0x222108]); ds1-ds3: 0-2 per 5 min run, no 0.5 s freezes. (2) The ISR/DPC admission of #64 now applies only to DirectSound's raise (sub_00203A90), not XACT's (sub_002227D2): an XACT section entered without the lock while the poller sat mid-walk in XACT is the likely cause of the list crash (the same crash as lt3, when the lock was opened for everyone). Hypothesis until the owner plays that route again. **2026-10-22 (owner play.log 12:21):** five voices near Jack's house never report their stop; each stop waited the full 50 ms (110 hitches of 66-120 ms, "more slowdowns"). A voice whose stop was forced once is now forced after 2 ms (sub_0020B669, table of 32 voices). |
| 56 | Crash in DSOUND (`sub_0020B140`) during long combat with Continues | Fix in the build, one clean soak | sk43a (2026-10-19, 18.2 min, 10 Continues): access violation reading guest 0xFE45093C; `sub_0020B140` (DSOUND) got ecx=0xFE45092A from `[buffer+0xC]` (`sub_00204183`), so the sound buffer object held garbage. usb2 also ended in DSOUND (`sub_0020A73C`). All of these and the #43 USB crashes ran with `RECOMP_HEAP_RECLAIM=1`, which handed a freed contiguous block to the very next request and zeroed it. Hypothesis: a structure still in use (by the game, or by the emulated USB/APU/GPU threads finishing with it) lost its memory that way. Fix (patches `contig_quarantine_a`..`_e` on xbox_memory_layout.c): freed contiguous pages wait 3 s before they are reused; a request that finds no room takes them at once, so #38 cannot come back. OOGIE_CONTIG_QUARANTINE_MS sets the time (0 = off). sk43b (same 25 min route, 11 Continues): no crash, contiguous memory 48-53 MB in use, no exhaustion. |
| 57 | Owner: menus, inventory, trophy shelf flicker (see-through menus, shelf without figures, menu flashes) | Fix in the build (root cause Verified 2026-10-21) | The owner's play.log (11:56 session) with the new always-on [FLICKER] detector: in these screens a frame draws part of the picture, clears the whole screen, then draws the rest (inventory: 152 world draws, a full clear, 153 menu draws; trophy shelf; loading screen). The #51 "copy before clear" workaround took the pre-clear half for a finished frame and showed it over 4000 times in that session: the see-through inventory (the world showing through, video IMG_6326 at 9-10 s) and the shelf without figures (IMG_6327 at 2 s and 8 s). With the Swap interval fix (#51) the flip falls at the end of the frame, so the copy is now off by default (OOGIE_CLEAR_SNAPSHOT=1 turns it on). Also: (a) SET_CLEAR_RECT_HORIZONTAL/VERTICAL (0x1D98/0x1D9C) were never applied, so every clear wiped the whole surface; the back end now clears only the rectangle (D3D 11.1 ClearView; patch `clear_rect`; OOGIE_FULL_CLEARS=1 for the old way). (b) a near-empty frame between full ones in a heavy scene (60+ draws a frame; the figure viewer had 1-30 draw frames among 300-draw frames every ~8 frames) is not shown; the frame before stays one more flip (OOGIE_SHOW_SPARSE=1 shows them). Tools: [FLICKER] lines in every log; scripts/load_flicker.ps1, inv_hunt.ps1 -Owner. **2026-10-21 00:30, pause menu flicker (Verified, flip dump during the owner's live view of soak rh3):** 181 shown frames over 3 s, about 4 in 10 completely black, in runs of up to 15, the pause menu between them. The flip intervals held only the next frame's clear ("D35C*132 || C230 || D104*132"), and the back end showed the buffer drawn since the last flip, i.e. a just-cleared one; the sparse-frame hold covers only one frame in a row. D3D's vblank path that sets PCRTC_START (sub_001D8928) is not taken with immediate swaps (hook: never called), so instead patch flip_done records, at NV097_FLIP_INCREMENT_WRITE (sent by Swap right after the frame's last draw), the buffer the frame finished in, and be_flip shows that buffer (falls back to the old choice if it stops changing for 30 flips). OOGIE_PRESENT_CRTC=0 turns it off. pf4: 181 frames, 0 black; menu_flicker mf9 (Load Game): 0 flash frames; fd1-fd4 soaks: 3 of 4 clean (fd3: #66 desync). **2026-10-21, the owner's video (11:07, play.log):** the flip_done change was not enough: in the owner's session the pause menu, trophy shelf, figure viewer, save and chapter select menus were mostly black frames with real ones between (scripts/video_flicker.py: 298 flash frames in 185 s). Root cause (Verified, D3D traces ts21, gt1, fw2): every frame two game threads draw. A task thread (started at 0x00057E60, woken through its semaphore or by the frame-start wake sub_00057E00) begins its frame with D3D Clear (sub_001A5310: Clear(0xF3), from sub_0019D390, sub_000781F0, sub_001A0540 ...) and draws the scene; the main thread then draws the overlay and swaps in sub_000586E0 (0x1A85DE ..., Swap 0x1A7BB8). In flickering sessions the overlay went in before the task's Clear ("D35C*11 C35C D35C*4"): the D3D lock was contended 12 288 times in the owner's session, 7-8 in clean ones. On the console the task always finished its step first. Fix: sub_000586E0 is wrapped; a thread taking its drawing path first waits until every other thread that called D3D in this or the last frame and is inside a step (woken from its semaphore, sub_001AA460, or from parking, sub_001AD4B0, and not yet back) has finished, at most OOGIE_FRAME_ORDER_MS (default 30; 0 = off). Under the condition that reproduced the flicker (OOGIE_SCHED_YIELD_MS=200: 2 of 3 and 1 of 3 runs before), fk1-fk3: 0 of 3; fq1-fq3 clean; hitches and Continues unchanged. A first try that waited for every woken task (sub_00057E00 wrapper, OOGIE_TASK_RUN_MS) made every frame a hitch: opt-in only. **2026-10-21 evening (owner video 18:21, play.log 18:55):** better but not gone: 49 flash frames in 102 s (298 in 185 s before), D3D lock contended 4096+ times, and the frame-order wait ran into its 30 ms cap at least 453 times. Cause of the caps (Verified, cs5 stack): the "in a step" rule also waited for tasks that sleep or load inside their step (the logic thread 0x7E000005 in KeDelayExecution); with a copy of the owner's saves that held every frame 30 ms and boot never reached the intro film within 60 s (cso30), 11 s with the wait off. Changed: the wait now holds only while another game thread is drawing right now (a D3D call from it in the last 3 ms). With the owner's saves: intro in 11 s (csn1, csn2); flicker trigger fr1-fr3: 0; Chapter Select with the owner's saves cs1-cs4: 0; 15 pause open/close cycles x 3 at the owner's settings (scripts/pause_cycles.ps1, pcy1-pcy3): 0 flicker reports, contention 8, no caps. The owner's exact state (thousands of contentions) has not been reproduced here, so this needs the owner's confirmation. **2026-10-22 (Verified, owner play.log 11:25, logs/owner/play_20261022_1125.log):** the 10-21 build froze the owner's game for good the first time the frame-order wait hit its cap (frame 38016): the cap's diagnostic asked for the main thread's own stack, and the stack printer suspends the thread it walks, so the main thread suspended itself (stuck in ZwSuspendThread from pc_stall_start from sub_000586E0+2D2). Fixed: pc_stall_print_thread refuses the current thread, and the frame-order cap no longer prints stacks at all (log line only). A test script then closed that frozen game at about 11:29 (it stopped every oogie_recomp by name); the owner's saves were not touched. Scripts now record the games they start in logs/test_pids.txt and stop only those (scripts/stop_test_game.ps1), and live_start.ps1 / boot_like_play.ps1 refuse to start while any other game is running (scripts/owner_game_check.ps1); verified sk0. **2026-10-22 afternoon, root cause of the remaining menu flicker (Verified, owner video 12:24 + play.log 12:21, logs/owner/play_20261022_1221.log; scripts/video_flicker.py: 554 flash frames in 278 s):** the trophy menu's task thread (0x00057E60, guest 0x7E000117, a new thread at each opening) drew its 137 draws after the main thread's Swap, then its Clear, then the Swap came (D3DTRACE f=9005..9067, 200 reports, 0 frame-order caps). The task runner sub_000583C0 releases a task's semaphore (sub_001AA3E0) and polls sub_001AA550 until the game's count [+4] is 0 and its waiter count [+0x10] > 0. A release that comes while no thread waits does not take a waiter off, and the next wait adds one and passes at once (semaphore already signalled), so [+0x10] stays 1 while the task runs: from then on the runner takes every task frame as finished as soon as it starts. It begins when the runner releases a just-created task thread before that thread first waits (a race at thread start), so each menu opening could go either way. Fix (recomp_manual.c, sub_001AA3E0 / sub_001AA460): a release that found no waiter is remembered per semaphore and the wait that consumes it takes itself off the waiters ([SEM] log line). OOGIE_SEM_CREDIT=0 = the original; OOGIE_TASK_WAIT_LATE_MS=N (test) delays a task thread's first wait to force the race. scripts/sem_race_check.ps1 (10 pause cycles each): original + forced race 41 flicker lines, fix + forced race 1, fix 1 (the single lines are an empty loading frame, not a split menu frame); Chapter Select with the owner's saves (select_flicker): original + forced race 29, fix + forced race 0 (7 corrections). |

| 58 | Owner: game locked up and "crashed" in the Shadow Oogie figure viewer (Jack's house) | Fixed (mechanism Verified) | Not a crash: Windows closed a hung game (AppHangB1, 11:56:17). [STALL] (15 s, all threads): the game thread in D3D's fence wait (sub_001D78D0) in its "GPU far behind" path, which writes a NO_OPERATION with a value into the pushbuffer and loops "while (KeWaitForSingleObject(device + 0x1DCC))" on an event the GPU's software-method interrupt sets (0x1D7A18). D3D builds that event itself, so the runtime had no shadow for it, every wait failed at once, and the loop spun forever. At 200% render resolution the figure viewer let the GPU fall far enough behind to take that path. Fix (patch `ke_wait_d3d_gpu` on kernel_bridge.c): a wait on that event waits until the GPU has run everything submitted (the NO_OPERATION is before PUT) and reports success; at most 2 s. Logged as "[D3D] GPU-far-behind wait". |
| 59 | Chapter loading screen: the chapter banner blinks (shown about 2 frames in 6) | Fixed (Verified lt9) | lt1-lt7 (chapter start from Chapter Select): most frames draw ~20 items (mist sprites, sometimes the banner) into the next buffer, then clear it fully (clear rect 0..639 x 0..479), then draw the background; the banner survives only in frames where it comes after the clear. The method ring around the flips (OOGIE_RING_AT_FLIP) shows D3D's normal Swap sequence, so the flips are where the game swaps; same with the original presentation interval (lt7). Hypothesis: a loading thread draws the overlay at its own times; whether the console shows the same blink is not known. Next: compare with xemu, look at who issues the pre-clear draws. **2026-10-21 (Verified, lt9: 398 frames of a Chapter 2 start):** with the back end showing the buffer each frame finished in (patch flip_done, #57), the banner area reads the same in all 258 frames after the fade-in (red mean 45.4-45.8), the "Now Loading" screen is steady, and the fades are smooth. The blink was the same wrong-buffer choice as the menu flicker. |
| 60 | Owner photo: the loading transition (orange shapes sweeping in) is missing its left and right pieces | Open | Not captured yet: it plays between areas (door.stm), which the scripted routes do not reach. "Hide Black Borders" removes only black untextured quads, so it is not that rule. Next: a door route with frame dumps. **2026-10-21:** in widescreen the 2D overlay is squeezed back to 4:3 (#53), so edge-anchored pieces stop short of the sides. Now a 2D batch that touches a side of the 4:3 screen keeps that side on the side of the wide screen (shader vaa.w; OOGIE_WIDE_EDGES=0 off); centred menus and the HUD are unchanged. Not yet seen on the door transition itself (no door route); the owner's video has no transition. |
| 61 | Freeze: no frame for minutes while the GPU keeps drawing (the game task walking its render lists without end) | Open; avoided in the default build | Seen in sk58d, sk60b/c/e, sk712, bsC1/bsC2: the main thread waits for a game task (sub_000583C0); the task (sub_0019CE60 -> sub_000849D0 -> sub_001B24E0) walks 51 render-list buckets (0x413060, objects linked at +0xA4, callback lists at +0xA8/+0xAC) and keeps drawing; D3D wraps the command ring again and again. Hypothesis: the lists are rebuilt (sub_001B2490 -> sub_001B2270, which appends without clearing the new tail's next pointer) while the task walks them; on the single-CPU console the higher-priority task finishes its walk before the main thread runs again, here both run at once. A check that cut looping lists before each walk (OOGIE_RLIST_CHECK=1) found "loops" many times a minute, i.e. lists still being built, and made things worse; a C replacement of the walk crashed in its callbacks (cause not found). Pinning all game threads to one processor (OOGIE_GUEST_CORES=1) ended these freezes but made pacing poor and showed a USB crash. It appears when the threads are not serialized by the log lock (OOGIE_FAST_LOG=1), so the pipe log is off by default. Next: emulate the console's hand-off (a task released by the main thread runs before the main thread continues), or find the game's own synchronization that is not holding here. **2026-10-20 night:** the runtime already has a guest CPU lock (RECOMP_GUEST_LOCK=1: one game thread runs game code at a time, released in every kernel call). Port code that waits on its own locks or yields outside a kernel call now lets it go (oogie_cs_enter, oogie_host_yield; patch guest_cpu_held). With it on, the game ran at a steady 60 with tiny frame times but boot never reached the intro film (gx1-gx4: frame 10670, no mv_00.bik): some game thread waits for another without a kernel call (the lock's documented weak point). Off by default; next: find that wait (a [STALL] all-threads dump during the boot stall). Also added: the render-step lock (sub_0007D1B0/sub_00039920). **2026-10-20 06:00-06:45 (lock mode, Verified from gy1/gz1 stall dumps):** three waits that hold the guest CPU were found and fixed: the job-worker start wait (sub_000584E0_enter, Sleep under the lock), the thread park wait (sub_001AD4B0, WaitForSingleObject under the lock), and a game loop that spins on "is it my turn" with no kernel call (sub_0004F4E0 via sub_00057EA0; new hook sub_00057EA0_enter yields the CPU, only in lock mode). With these, boot gets further (frame 248) and then every game thread sits in a kernel wait that nobody signals: the main thread in the task poll (sub_000583C0), a loader thread in sub_0022F210 (from sub_0022F0E0 / sub_001A7F90) and the thread started at 0x0022F360. Hypothesis: a wake-up lost between them, for example a signal sent by a host callback while the lock order differs. Lock mode stays OFF by default; the default build (no lock) is unaffected by these hooks (each one checks guest_cpu_held()). **2026-10-20 afternoon (Verified, gw1/gf1/gs1 [WAIT] dumps):** the boot hang under RECOMP_GUEST_LOCK=1 was starvation: the main thread polls its task table through kernel calls (mutex 0x48000002, 28 million times in 50 s) and the critical section behind the lock let it retake the CPU every time. Patch guest_cpu_fair: the guest CPU is a ticket lock (first come, first served). Boot then goes further (6 film files) and stops on Bink's worker (0x0022F360) waiting on handle 0xFFFFFFFF: the runtime ignored PsCreateSystemThreadEx's CreateSuspended (argument 7), and Bink creates the worker's mutex and event after CreateThread (sub_0022F3D0). Patches thread_suspended_a/_b/_c honour it, opt-in (OOGIE_SUSPEND_NEW=1): normal-mode soaks went worse with it on. Wait tracing (hooks on XAPI WaitForSingleObjectEx/WaitForMultipleObjectsEx/SetEvent/ReleaseSemaphore/ReleaseMutex, printed at the long-stall dumps) is on with the lock or OOGIE_WAIT_TRACE=1. Lock mode still off by default. **2026-10-21 03:30 (gv1/gv2):** with the Bink worker fixed (#42) lock mode plays all six films and runs into the level, then stalls at frame ~4000 the same way as gu1: the main thread polls its task table (mutex 0x48000002 taken 17 M times) while the workers wait on job semaphores 0x48000021 / 0x48000024 that it last released seconds earlier. Next: read the task table (0x40C058 entries, sub_000583C0) at the stall to see which task the main thread waits for. Lock mode stays off; the default build is stable without it (ln1-ln3: 45 min clean). |
| 62 | Some sessions run with the GPU behind all the time: about one hitch of half a second per few seconds | Open | fin1 (10 min soak, default build): 194 hitches of ~500 ms; the [CS] report names Swap (sub_001D6D20) waiting in the fence wait (sub_001D78D0, sleeping in the game's callback sub_001A53B0) while holding the render lock. fin2, same build, had none. Hypothesis: the per-session out-of-step state seen in #51 in another form, or the 250 ms GET wait in the sub_001D75F0 hook taken twice per frame. |
| 63 | Owner (Ch3, Lock/Shock/Barrel cutscene dt07): subtitles drawn as a "barcode" of coloured blocks | Open | Owner screenshots 2026-10-19. Dialogue subtitles render correctly (sub1: talking to the Mayor in Ch3, "When Oogie's monsters approach, I have to run"), so the font path works; the cutscene text is at the right place and size with garbage contents. Hypothesis A: the cutscene draws its text into a render target once and shows it for many frames; the #49 rule (`rt_replaced`: target not drawn for 8 frames and its guest memory changed) then decodes the texture from guest memory, which the D3D11 back end never writes (test: OOGIE_KEEP_STALE_RT=1 in that cutscene). Hypothesis B: a swizzled/linear mix-up (the block pattern looks like Morton-order data read linearly). Next: a save just before dt07 and a texture probe (OOGIE_TEXPROBE=1). |
| 64 | Owner: Ch3 at 1-2 fps, threads in DirectSound's "wait for the voice to stop" | Fix in the build, untested in Ch3 | play_20261019_2053.log: [STALL] shows threads in sub_0020B669/sub_0020AA01 (the 500 ms voice-stop poll, #55) and the DirectSound DPC (sub_0020821C via kernel_timer_thread). The stop is finished by that DPC (IRQL 2), which the #55 fix still kept behind the lock (only IRQL above 2 was let in). Now an emulator thread (not a game thread; flag set at sub_001BD404 entry) at DISPATCH_LEVEL or above may enter DirectSound without the lock while a game thread polls. |
| 65 | Owner: Ch3 pumpkin bombs freeze and never explode | Fix in the build (regen), untested in Ch3 | 45 failed ICALLs to 0x0009F0F0 and 6 to 0x0009FB40 (real function starts). Both added to scripts/icall_seeds.json by the playtest session; full regen 2026-10-19 21:06: both now generated (10521 functions). |
| 66 | The emulated GPU loses its place in the command ring: old commands replayed, fences never reached, "no frame" freezes | Root cause found; DMA-walk mode under test as the fix | Night of 2026-10-19/20. Instrumentation: the GPU semaphore watch ([GPU] semaphore ... went back), a 32-step history of the segment scan ([PB]), the address of each release ([NV2A] release X after Y: method at A), CALLs into the ring, carried parameters. Findings: (1) the 1 s safety net's "write the fence" branch lied to D3D (rs2: every replay began right after it); removed, the net now only kicks. (2) Even without it (rt3, rw11) releases with old values came from 0x8000200C (the ring's own start) "inside a CALL", after a carried-over method 0x0000 with 1581 parameters and a release whose value was a method header (0x00040110): the default segment scan read data words as commands, i.e. it lost sync within ordinary forward segments. Not caused by a kick (none before). (3) The runtime's other walker, RECOMP_PB_WALK=dma ("the more faithful of the two", opt-in only because D3D's GPU-far-behind wait used to hang, fixed by #58): dw1-dw3 with the fast log had no backwards release or semaphore; dw1/dw2 ran 6 min with 9-10 hitches. dw3 stalled in the Bink path: BlockOnResource (sub_001D7C00) inserts its fence with flag 2, which does not kick off (sub_001D7810 0x1D78AB); the quick kick handed it over (GET = PUT = 0x8ACF0) but the semaphore stayed at 0xB7D (open). Also added: patch pb_ring_reset (a title GET write is a ring reset, not a wrap; never fired in tests), sem_offset (releases go to the word SET_SEMAPHORE_OFFSET names; D3D uses offset 0, nothing else seen), the render-step lock (sub_0007D1B0 / sub_00039920, OOGIE_RENDER_LOCK=0 to turn off). **Decision (2026-10-20 05:00):** RECOMP_PB_WALK=dma is now the default (main.c; RECOMP_PB_WALK=segment for the old scan): dn1-dn3 and mo2-mo4 had no replay, no backwards semaphore, 4-5 Continues each. The walk still resyncs on a garbled word about once per run (words like 0x3F000000, i.e. vertex data where a command should be), and that can lose a fence (mo1: a Swap waited on fence 0xCB1 for good). Hypothesis: two game threads writing D3D commands at once (#61); with the fast log on it happens far more often (df1-df4). **Owner freeze 2026-10-20 09:50 (Verified, logs/play.log):** title screen after skipping the intro film; fullscreen, render_scale 3, Intel Arc 140V. The walk followed a jump word 0x00011101 (at 0x89478, again at 0x8BFAC; 0x00011329 at 0x8D1C0) to 0x11100, read 0xC8C420C0 at 0x1113C as a 561-parameter header (the walk accepts any word with (w & 0x00030003) == 0; the NV2A only takes 0 or 0x40000000 in the top bits), landed on 1.0f at 0x11A04 and resynced (3 times). A release was lost: fence 0x126B wanted, semaphore stuck at 0x1261, GET = PUT = D3D write pointer 0x8E76C, for 80 s. The 8-entry history starts after the bad jump, so what put the walk out of step before 0x89478 is not known (Hypothesis: concurrent D3D writes, #61; it happens with fullscreen + render_scale 3, i.e. a slow back end: 1 of 3 fullscreen test runs (ow1: 16 resyncs), 0 of 2 windowed with the same settings). **Fix (safety net):** when every command D3D wrote is consumed (GET = PUT = [dev+0]) under the DMA walk and the fence is still not reached, the semaphore is set to the newest issued fence ([dev+0x2C], or the wanted one): after 300 ms in oogie_gpu_kick_check, after 2 s in oogie_gpu_unstick. ow1 (2 s version): 81 such recoveries, no 15 s stall. OOGIE_NO_UNSTICK=1 turns it off. **2026-10-20 afternoon:** the owner session had 4 resyncs and 88 backwards semaphores; the first came while the figure viewer loaded (fig0e10.lz): a parameter 0x00000006 at 0xBFB08 read as a CALL to 0x4, zeros up to 0x1000, a jump to 0xDA24 and stale commands (release 0xDC75 after 0xDCD3, then 0xE00B repeated). Patch pb_ring_target_a/_b/_c (nv2a_pb_scan.c): the ring start is the GET of the first resync; a JUMP or CALL into the ring anywhere but its start, or below it, is a bad target (drop to PUT) instead of being followed. Also PB_HIST 8 -> 64 so the next resync shows what came before. OOGIE_PB_RING_CHECK=0 turns the check off. **Correction (2026-10-20 evening):** the ring check is OFF by default (OOGIE_PB_RING_CHECK=1 to try it): with it on, CALLs below the ring (0x300, 0x4) were refused, real commands were dropped, fences were lost and runs stalled for 15 s (ts, tb, sy, sz: 1 good run in 15). Those low CALLs are not always junk. Verified with the 64-entry history (rc3, ts1): the walk is in step up to the bad word, e.g. texture-stage methods 0x1B08/0x1B14, 0x1B48/0x1B54, 0x1B88/0x1B94 in order and then 0x2F100FF8 where stage 3 (0x1BC8) belonged, with PUT well past it: the word was not yet written when D3D moved PUT, i.e. another thread's commands (#61). Lead: the game's scheduler waker (sub_00057E00, drain loop sub_001A7BA0 with D3D Swap) carries on while the woken worker still runs; making the drain wait (sub_001BD332, OOGIE_SCHED_YIELD_MS=30) made soaks worse, so it is opt-in. **2026-10-20 18:20, the race hypothesis weakened (Verified, logs/gu1.err.log):** with RECOMP_GUEST_LOCK=1 + OOGIE_SUSPEND_NEW=1 (one game thread runs guest code at a time) gu1 ran 200 s and still had 17 bad targets and 36 backwards semaphores. So two game threads writing at once is not the main cause. Ruled out as well: BlockOnTime's in-place patch (sub_001D78D0 0x1D797A..0x1D79D3 rewrites four words at fence record +0x10..+0x1C as header/value/header/value, the same shape, so it cannot shift the walk). Remaining candidates: (a) guest code run outside the lock by host threads: D3D's DPC (sub_001DB9D0 -> sub_001DB220 calls a title callback, "call eax") on the timer thread, the APU and USB ISRs/DPCs; (b) a command form the walk decodes wrongly (CALL into low memory 0x0..0x2000, where phys 0 holds a JUMP to 0x2000). Next: log every CALL target the walk takes, and which host thread last wrote [dev+0] (D3D's write pointer). **2026-10-21 night (Verified, logs/rr1, logs/fd3):** clean runs take no CALL at all (rr2-rr4, rh1-rh6: 0), so the first CALL marks the desync. fd3: the stream was in step through D3D_SetScissors' output (sub_001D3D80, path for several rectangles: 0x80200 640/480, 0x402B4 0, 0x402C0 value) and then held 0x42, 2, 2, 0x40 where 0x402E0 belonged; the generated C for that loop writes esi = 0x402E0 correctly, so the words were not written by it at the time the walk read them. 2 ms later the words were unchanged; the re-read now waits 50 ms and 500 ms to tell a descheduled writer from an overwrite. **2026-10-21 05:00, root cause and fix (Verified):** fn1's history shows two D3D writers overlapping: a complete SetTexture command (0x00081B00 0x00103000 0x00010629) and then four floats (1.0, 1.0, 16777215.0, 1.0) whose header had been overwritten by that command's last value. D3D reserves ring space from one write pointer and is not thread-safe; on the console only one thread was ever inside it. All 69 D3D functions the game or D3DX calls directly are now wrapped (recomp_manual.c, generated block "D3D entry lock": sub_X calls sub_X_gen inside a recursive lock taken by game threads only, at most 200 ms wait). dc1/dc2: the lock was contended 8+ times per run (callers 0x001A85DE, 0x001A60EE, 0x001A1FC2 in the game's graphics wrapper, 0x001E5F76 in D3DX), i.e. two game threads really were in D3D at once; never timed out; hitches unchanged (dl1-dl6: 107-153, same as before). OOGIE_D3D_LOCK=0 turns it off. |
| 67 | Palettised textures (SZ_I8_A8R8G8B8, 0x0B) drew as white boxes | Fixed in the build, untested in game | nv2a_backend_decode_texture set the palette to 0 ("no palettised formats"), so every 0x0B texture failed to decode and the back end used white. Patch palette_decode takes the CLUT from the stage that holds the texture. |
| 68 | Owner's 15-hour session (2026-10-22 09:00 to 10-23 00:10): play.log grew to 10 GB of one line, "[KERNEL] NtWaitForSingleObjectEx: token=0x48000001 ... timeout=finite" | Fixed in the build (Verified wp1 for the cause; the fixed counter not yet over a long session) | The bridge logged its first 20 calls with `static int logged; if (logged++ < 20)`; after 2^31 calls the int wrapped and every call was logged (found by the peer session, which kept head and tail in logs/owner/play_20261023_0010_*.log). Patch wait_log_bounded / wait_multi_log_bounded: the counters stop at 20. The patch also reports a wait polled over 10 000 times a second; wp1: about 500 000 calls a second, token 0x48000002, INFINITE, from sub_001BD1CC (the game mutex [0x40C858]) called by sub_001AA550, the task runner's poll in sub_000583C0: the main thread spins on the mutex while a task runs its step, one CPU core busy and the mutex shared with the task's own semaphore calls. Fix (recomp_manual.c, sub_001AA550 wrapper): a poll from the runner that finds the task running sleeps (WaitOnAddress) until a task reaches its wait (sub_001AA460) or 1 ms; OOGIE_TASK_POLL_SPIN=1 = the spin. |


## Part 3. Our map of the game's code and data (REVERSE_ENGINEERING)

_Source: our `docs/REVERSE_ENGINEERING.md`, included unedited except headings._

## Reverse Engineering Notes: default.xbe

All facts below are **Verified** from `logs/xbe_parser.log`, `logs/disasm.log` and run logs unless marked **Hypothesis**.

### Source image
| Item | Value |
|---|---|
| Image | `Tim Burton's The Nightmare Before Christmas - Oogie's Revenge (USA).xiso.iso`, 3,498,311,680 bytes |
| Made from Redump | MD5 509aa949d60777ac01bb1c80fd1a3bc0, SHA-1 b39ba1e3f4d0088fdbb7b4261491d580ca535011 (per Vimm's Lair.txt) |
| XDVDFS | partition base 0x0, root dir sector 264, size 200 |
| Files | 723 entries (`docs/game_inventory.tsv`) |

### default.xbe
| Item | Value |
|---|---|
| Size | 3,751,936 bytes |
| SHA-1 | 667c5bd8abc4a551ef6b7cbab3bf3a45e7244605 |
| SHA-256 | 9a8319c7b1cb5ee29781bf641a0cf549c221fdc57effb8f72085608019f41548 |
| MD5 | 9513bdeac7617654666a38817c9f1bb0 |
| Title | "Tim Burton's The Nightmare Before Christ" (truncated in cert), ID 0x42560004, region NA, Retail, disc 0 v1 |
| Build date | 2005-07-25 02:20:04 UTC; cert 2005-08-05 |
| Debug path | `c:\Project\nightmareXbox\xbox\project\xnightmare\xnightmare\Release_US\xnightmare.exe` |
| Base / image size | 0x00010000 / 0x004403C0 |
| Entry point | **0x001BEF07** (raw 0xA8E7B8AC, retail XOR) |
| Kernel thunk table | 0x0025F8E0 (raw 0x5B48B856), 125 imports |
| TLS | index addr 0x0041279C, zero-fill 144 bytes |
| Stack commit | 0x10000 |

#### Libraries (all XDK 1.0.5849)
XAPILIB, D3D8, D3DX8, XGRAPHC, DSOUND, XACTENG, XBOXKRNL, LIBCMT, LIBCPMT.
Bink (RAD) and WMADEC/DOLBY code is present as sections but not in the library table.

#### Sections (31)
`.text` 0x00012000 (1.75 MB code), D3D 0x001D1B80, D3DX 0x001E4B00, XGRPH 0x00201960, DSOUND 0x00203A60, XACTENG 0x00222380, BINK 0x0022CEA0 plus 15 small BINK* pixel-format sections (BINKYUY2 appears **four times** with the same name), WMADEC 0x0023EEC0, XPP 0x00257F40, .rdata 0x0025F8E0, .data 0x00280340 (1.72 MB virtual), DOLBY 0x00437DC0, BINKDATA 0x0043EF40, $$XTIMAGE, $$XSIMAGE, .XTLID. Full table in `logs/xbe_parser.log`.

#### Kernel imports (125)
Av 4, Ex 5, Hal 8, Io 9, Ke 21, Kf 2, Mm 11, Nt 33, Ob 2, Ps 3, Rtl 12, Xbox 8, Xc 4, other 3 (DbgPrint, LaunchDataPage, IdexChannelObject). Notable: `XeLoadSection`/`XeUnloadSection` (dynamic section loading), `KeConnectInterrupt` (GPU interrupt), `MmClaimGpuInstanceMemory`, `IoCreateDevice`/`IoStartPacket` (XAPI drivers), `XcSHA*`/`XcHMAC` (save signing).

### Disassembly (XboxRecomp v0.14.0, all code sections)
- 804,237 instructions, 10,322 functions (5,399 call targets, 2,909 cc boundaries, 908 prologues), 162,191 xrefs, 2,499 strings, 876 jump tables resynced.
- Recursive-descent reachable: 65.6% of swept instructions.
- func_id: 318 thiscall functions detected; categories in `build_out/func_id/summary.json`.

### Runtime-discovered functions (indirect-call targets)
Measured by running the exe (Verified, run1):
| VA | Section | Seen from |
|---|---|---|
| 0x001D4320 | D3D | first D3D device init, after KeConnectInterrupt vector 3 |
| 0x001D4B70 | D3D | same |
| 0x001D59E0 | D3D | same |

### Boot sequence observed (run1, Verified)
1. Entry 0x001BEF07 creates a system thread (routine 0x001BD404, ctx 0x001BEE93).
2. CRT/heap init: NtAllocateVirtualMemory 1 MB reserve then commits.
3. About 90 alternating calls to ordinal 289 `RtlInitAnsiString` (from 0x001C07B5) and 279 `RtlEqualString` (from 0x001C07C5): a string-table lookup loop, not a hang (Verified ordinals; purpose **Hypothesis**).
4. Opens `UDATA\42560004\TitleMeta.xbx`, `TitleImage.xbx`, `SaveImage.xbx` (save metadata), reads partition0 sector, queries partition5 (cache), looks for `TDATA\42560004\$u\contentmeta.xbx` (not found, normal on first run).
5. Allocates 4 KB, 1 MB, 3600 KB, 1200 KB contiguous memory (**Hypothesis**: push buffer and frame buffers), connects GPU interrupt (vector 3, ISR 0x001DB020).
6. Executes `wbinvd` at 0x001D9F34 (D3D section, cache flush; no-op is fine).

### Identified functions and globals (Verified by disassembly)
| Address | What | Evidence |
|---|---|---|
| 0x001E25E8 | XDK D3D global device pointer (`g_pDevice`) | `mov eax,[0x1e25e8]` throughout D3D section; run4 device = 0x001E25F0 |
| 0x001D78D0 | D3D fence wait (`D3D_BlockOnTime`): reads device+0x30 (semaphore ptr) and device+0x2C (write seq) | D3D.asm; matches xboxrecomp d3d8ltcg-device-context.md |
| 0x001D7BE0 | Block until idle: `D3D_BlockOnTime(device->fence(+0x2C), 2)` | D3D.asm |
| 0x001D7810 | Kickoff called by the fence wait when waiting on the newest fence | D3D.asm |
| 0x80001000 | GPU semaphore (device+0x30 target) at run time | run4 log |
| 0x001BD332 | XAPI `SwitchToThread`: `NtYieldExecution() != 0x40000024` | text.asm |
| 0x001BD012 | XAPI `WaitForSingleObjectEx` wrapper (returns 0x102 timeout / 0xC0 APC) | text.asm, sub_001A7BA0 |
| 0x001A7BA0 | "Drain" loop: enter CS [0x40B0E0], D3D call, then `while (SwitchToThread()) WaitForSingleObjectEx([0x40B0E4],0,TRUE)` | text.asm |
| 0x000586E0 | Loader-state step; 0x003E3F64 = state (1 loading, 2 done), worker ctx 0x000584E0 | text.asm, run7c thread ctx |
| 0x001BD404 | Thread start trampoline used for all PsCreateSystemThreadEx calls | run logs |
| 0x001BCE35 | XAPI CreateFile call site (all game file opens come from here) | run logs |
| 0x00208AD1 | DSOUND/APU interrupt service routine (vector 5) | run7c |
| 0x0020EB76 | Interrupt routine for vector 6 (Hypothesis: USB/OHCI) | run7c |
| 0x001DB020 | NV2A GPU interrupt routine (vector 3) | run1 |

### Session 2 additions (Verified by disassembly, `build_out/disasm/asm/`)
| Address | What | Evidence |
|---|---|---|
| 0x001D75F0 | D3D "GPU position" helper `(device, wait)`: with wait != 0 spins until `(*(dev+0x30) << 2 ^ PGRAPH[0x400B10]) & 0x7C == 0` | D3D.asm; win_run1 watchdog return 0x001D79AF |
| 0x001D7670 | D3D kickoff flush: sets bit 0x10000 in `[dev+0x1C28]+0x100410`, spins until cleared | D3D.asm |
| 0x0003DF90 | Boot sequence: starts job 0x0003DD20 via sub_00058540, then `while (sub_00057E90()) { sub_000583C0(); sub_00052520(); }` (Now Loading loop) | text.asm |
| 0x00052520 | Draws one Now Loading frame (3 quads via sub_001A5990), then sub_000586E0 | text.asm; win_run2 stack 0x0005273D |
| 0x00057E90 | `job_running()`: returns `[0x3E3F7C]` | text.asm |
| 0x00058540 | Start background job: state=0, CreateThread(0x000584E0, 0x3E3F64, CREATE_SUSPENDED), job.func=arg, ResumeThread, state=2 | text.asm 0x58587..0x58604 |
| 0x000584E0 | Job worker: calls job.func(job.arg), then clears the job record at 0x3E3F64 (+0x18 = func) and exits via sub_001AD520 | text.asm |
| 0x003E3F64 | Job record: +0 state, +4 thread handle (0x3E3F68), +8 (0x3E3F6C), +0x18 func (0x3E3F7C), +0x1C arg | text.asm |
| 0x001AD720 | Scheduler thread create: XAPI CreateThread(..., flags 4, &tid), fills a 0x18-byte entry in table 0x40EDD8 (+0 handle, +4 tid, +0xC state) | text.asm |
| 0x001AD3D0 | Scheduler resume by tid (sets entry state 2, ResumeThread) | text.asm |
| 0x001AD4B0 / 0x001AD4F0 | Scheduler suspend / resume by tid through table 0x40EDD8 | text.asm |
| 0x001AD260 | Scheduler init: `DuplicateHandle(-2 current thread, options 0)` into table slot 0 | text.asm 0x1AD284 |
| 0x001BD49C | XAPI CreateThread -> PsCreateSystemThreadEx (CreateSuspended = flags bit 2) | text.asm |
| 0x001BD290 / 0x001BD2B6 | XAPI SuspendThread / ResumeThread (NtSuspendThread / NtResumeThread) | text.asm |
| 0x001BD50A | XAPI GetCurrentThreadId: `[fs:[0x28] + 0x12C]` | text.asm |
| 0x001BC71B | XAPI DuplicateHandle -> NtDuplicateObject(src, &dst, options) | text.asm |
| 0x0025F8E0 | Kernel thunk table base (slot n = 0xFE000000 + 4n at run time) | default_analysis.json |
| 0x001AD3A0 | Scheduler "should this thread stop?": 1 if the caller's ID is not in table 0x40EDD8 or its entry state is 3 | text.asm |
| 0x001AD440 | Scheduler kill by tid (entry state = 3, signal its event) | text.asm; win_run8 logs |
| 0x001AD520 / 0x001AD540 | Scheduler thread exits (unregister, then XAPI ExitThread) | text.asm |
| 0x0019D8E0 | Game-logic thread: sets game modes via sub_001A0D30, exits when sub_001AD3A0 says so | text.asm, win_run7 |
| 0x0003E0F0 | Main loop after boot: `for (;;) { input; update; sub_00049470; sub_000ED350; ...; sub_0019C4D0; sub_001C7EE1 }` | text.asm, win_run6 samples |
| 0x001A0660 / 0x001A0970 | Movie player: fade, then `while (sub_001A7DC0()) { draw frame sub_001A7E30; input; scheduler }` | text.asm |
| 0x001A7DC0 | Movie frame step: bink = [0x40B148]; decode (sub_0022E730); stop at last frame unless looping ([0x403974]); next frame (sub_0022E980) | text.asm; win_run12 counts |
| 0x0040B148 | Current Bink handle; +8 frame count, +0xC current frame (mv_00: 6775 frames) | win_run12 log |
| 0x001A7C10 | Movie open (calls Bink 0x0022D300 etc.) | text.asm |
| BINK section 0x0022CEA0..0x00237000 | Statically linked Bink; worker threads start at 0x0022F360 | section table, win_run9 |
| 0x00234AF0 | Bink frame planes: calls the plane decoder 3x (Y, then two chroma planes); caller passes 11 cdecl args | BINK.asm 0x234B6D/0x234BDB/0x234C30 |
| 0x00232C40 | Bink plane decoder (7781 bytes). Args seen (bcap2, Y plane): dst 0x82FD1000, previous frame 0x82F60000, width 640, height 480, pitch 640, per-block state 0x011D6AA0 (1200 bytes = 40x30 16x16 blocks), ... Reaches only BINK-section code; calls 0x231C60 or 0x231F30 through [ebp-0x94] | BINK.asm, bcap2, tools/binkdiff |
| 0x00235BA0 | Bink 8x8 inverse DCT + add to prediction (columns pass, then rows with +0x7F, >>8, byte add); stdcall, `ret 0x14`; multiplier table 0x0027DC00 + arg*256 (Hypothesis: dequantisation by quality level) | BINK.asm, binkdiff (#35) |
| 0x00235470, 0x002357A0, 0x00235F30, 0x00236840 | Other Bink block transforms in the same family (movsx of bp at 0x235508, 0x235848, 0x236162...) | BINK.asm |

### Open questions
- Is BINKDATA code or data? It is flagged W and was swept as code.
- The 106 "unimplemented" instructions (aas, arpl, outsb, bound...) are almost certainly data mis-decoded as code. Confirm none are reached.


## Part 4. Fixes we made to the XboxRecomp runtime (build-time patches)

_Source: our `port/patches/README.md`, included unedited except headings._

## Build-time patches to the XboxRecomp runtime

`third_party/xboxrecomp` stays an unmodified v0.14.0 checkout. Where the runtime
itself is wrong for this game, `port/CMakeLists.txt` (`oogie_patch_source`)
copies the affected source file into the build tree, applies exact text
replacements from this folder, and compiles the copy instead of the original.

Each replacement is a pair of files, `<name>.old` and `<name>.new`. The `.old`
text must occur exactly once in the original file (line endings are
normalised), otherwise configuration stops with an error, so a patch can never
silently stop applying after an upgrade.

| Patch | File | Why | Evidence |
|---|---|---|---|
| `thread_stacks` | src/kernel/xbox_memory_layout.c | Raise the guest thread-stack pool from 8 to 32. Chapter 1 keeps more than 8 task threads alive; the 9th ran inline on the main thread and froze the game | logs/live1.err.log "out of thread stacks"; KNOWN_ISSUES #23 |
| `clear_channels_32`, `clear_channels_16` | src/kernel/nv2a_pb_exec.c | Honour the per-channel CLEAR_SURFACE bits (R 0x10, G 0x20, B 0x40, A 0x80). The game clears alpha only (0x80) after drawing the scene; the runtime cleared the whole pixel and the level showed black | logs/live6.err.log frame trace "clear 80 surf 00230000"; KNOWN_ISSUES #24 |

| `vertex_current_value` | src/kernel/nv2a_pb_exec.c | A vertex-program input not supplied by an array takes the current value set with SET_VERTEX_DATA* (as on the NV2A and in xemu), not 0,0,0,1. The level's fade quad got opaque black and covered every frame | logs/live14.err.log frame trace: last pass `d0 0 0 0 1`, full screen; KNOWN_ISSUES #25 |
| `swizzled_surfaces_a/_b/_c` | src/kernel/nv2a_pb_exec.c | Swizzled render targets (SET_SURFACE_FORMAT type 2): kept in row order while drawn, re-ordered to swizzled layout when drawing moves elsewhere or at a flip. The game's 128x128 glow targets were sampled scrambled: dark stripes over gameplay | logs/live22.err.log: format 0x07070228 for surfaces 0x03206000..0x03236000; KNOWN_ISSUES #30 |
| `pb_segment_bounds_a/_b` | src/kernel/nv2a_pb_scan.c | The segment walk stops at addresses past the 64 MB contiguous window instead of reading them (the DMA walk already refuses them) | logs/d3d2.err.log: crash in pb_walk_segment+0x5F reading 0x8F000000; KNOWN_ISSUES #28 |
| `quiet_gpu_trace` (xbox_memory_layout.c), `quiet_draw_progress` (nv2a_pb_exec.c) | | `OOGIE_QUIET_GPU=1` stops `RECOMP_PB_EXEC` from also logging every kick-off and every 200th draw; with the D3D11 back end the GPU thread spent ~45% of its time writing these lines | tools/sampler on d3d3 |
| `pb_segment_ack` | src/kernel/nv2a_pb_scan.c | The segment walk acknowledges D3D kickoffs every 256 words, as the DMA walk does, so the game thread is not blocked in its kickoff spin for the whole execution of a segment | tools/sampler: game thread ~50% in sub_001D76F0's spin on 0x100410 |
| `parallel_vsh` | src/kernel/nv2a_pb_exec.c | Vertex programs of batches with 512+ vertices run on a pool of helper threads (`OOGIE_XF_THREADS`, default cores-2, max 6; 0 = off) | tools/sampler: 58% of the GPU thread in nv2a_vsh_run in Town Square |
| `cache_verbose_getenv` | src/kernel/nv2a_pb_exec.c | `getenv("RECOMP_PB_EXEC_VERBOSE")` in draw_primitive ran on every draw; cached | tools/sampler with caller offsets: ~13% of the GPU thread |
| `backend_extra_stages` | src/kernel/nv2a_pb_exec.c | Lets the back end read texture stages 1-3, the combiner stage count and the whole register-combiner program plus fog colour (`oogie_backend_stage_texture`, `oogie_backend_combiner_stages`, `oogie_backend_combiner`) | Bink YUV movies (mov2); KNOWN_ISSUES #17, #36 |
| `backend_vertex_extra_a/_b/_c` | src/kernel/nv2a_pb_exec.c | Per back-end vertex, the data `Nv2aVertex` leaves out (specular, fog factor, stage 0 q, texture coordinates 1-3 normalised) in a parallel array (`oogie_backend_vertex_extra`), so the D3D11 back end can run the combiners | KNOWN_ISSUES #36; pause menu and HUD at full brightness (rc3) |
| `vsh_access` | src/kernel/nv2a_vsh_interp.c | Accessors for the loaded vertex program and the constant file (`oogie_vsh_program`, `oogie_vsh_constants`) | needed by `backend_gpu_vsh` |
| `backend_gpu_vsh` | src/kernel/nv2a_pb_exec.c | A back end that registers a draw function (`oogie_gpu_vsh_register`, `port/src/oogie_gpu_vsh.h`) receives each vertex-program batch whole (inputs the program reads, program, constants, triangle list) and runs the program on the GPU; programs that write c[] stay on the CPU | KNOWN_ISSUES #26, #37: Town Square 38 -> 60 fps (gv3) |
| `ohci_ed_head_a/_b` | src/usb/ohci.c | The emulated USB controller writes an ED head pointer back only if the driver has not changed it during the pass (compare-and-swap) | KNOWN_ISSUES #43 (hypothesis; did not stop every crash) |
| `diag_skip_batches`, `diag_trace_d0` | src/kernel/nv2a_pb_exec.c | Diagnostics only: `OOGIE_DIAG_SKIP_EQ` / `OOGIE_DIAG_SKIP_TEX` drop batches; the frame trace prints vertex 0's colours | inert unless enabled |
| `pb_segment_carry` | src/kernel/nv2a_pb_scan.c | The segment scan finishes a method whose parameters were cut by PUT in the next segment (it used to drop them and read them as headers), and a top-level walk that ends on a JUMP records its target (`nv2a_pb_scan_segment_wrap`). Includes the `pb_segment_bounds`/`pb_segment_ack` changes to the same function | KNOWN_ISSUES #46: white/dark flash frames (fx1-fx4, fv1-fv2: 0) |
| `pb_segment_wrap` | src/kernel/xbox_memory_layout.c | On a pushbuffer ring wrap, walk the old piece to D3D's JUMP and continue from its target, instead of stopping at the highest PUT seen so far | KNOWN_ISSUES #46 |
| `apu_mixdown_stereo` | src/apu/apu_dsp.c | The stub DSP's mixdown sends speaker mixbins to their side, centre/LFE to both at -3 dB and effect sends to both at -6 dB (was: even bins left, odd right) | KNOWN_ISSUES #48: 3-5 dB left lean (au17: 0.2 dB) |
| `apu_doorbell_latch` | src/apu/apu_dsp.c | The DSP doorbell ack keeps the first GP scratch page instead of following a changed SGE table (it cleared dwords in memory it no longer owned) | KNOWN_ISSUES #43 (defence) |
| `apu_bin_trace`, `apu_route_trace`, `apu_fecv_trace_a/_b/_c` | src/apu/apu_dsp.c, apu_vp.c | Diagnostics only (`RECOMP_APU_BINS=1`): mixbin levels, voice routing changes, cross-thread voice programming | inert unless enabled |
| `pad_wrapper` | src/input/xinput_device.c | Renames the Windows `xbox_InputGetState` to `xbox_InputGetState_xinput`; `port/src/pc_input.c` provides the real one (XInput + SDL2 pads, rebinding) and falls back to it under `OOGIE_INPUT_OLD=1` | docs/PC_FEATURES.md (port feature, not an upstream bug) |
| `pad_rumble` | src/usb/ohci.c | The game's rumble packets (OUT on endpoint 2) go to `oogie_pad_rumble` in pc_input.c instead of being dropped | docs/PC_FEATURES.md (missing upstream feature) |

All of these (except the diagnostics) are upstream bugs and should be reported to XboxRecomp.


## Part 5. PC features we built, and how (PC_FEATURES)

_Source: our `docs/PC_FEATURES.md`, included unedited except headings._

## PC port features

What the Windows port offers on top of the original game, and what is still to come.
The models are the BLACK (2006) recomp ports (`docs/BLACK_COMPARISON.md`) and the
Burnout 3 port Takedawn (xenophobentx/takedawn), whose settings menu (Back+Start / F1,
pages on LB/RB, Y resets a row, settings file) this one follows. Reviewed 2026-10-18.

### Owner decisions
- 2026-10-18 morning: no separate launcher; the game's original Options menu is left
  exactly as it was; `settings.ini` next to the game stores everything.
- 2026-10-18 afternoon (after seeing the BLACK recomp's RECOMP SETTINGS screen): build
  an in-game settings menu now, themed as Oogie's Revenge, that controls every PC
  feature, so nobody has to edit .ini files. Every new feature gets a row in it.
- Defaults: the original Xbox pad layout on every controller. On the keyboard:
  WASD walk, arrows = A/B/X/Y in the pad's diamond, TFGH = d-pad, Enter = Start,
  Backspace = Back.

### The settings menu ("Recomp Settings", `port/src/pc_menu.c`)
- **Open / close:** F1, or Back+Start together, or the Guide / PS button on any pad.
  B, Esc or F1 closes it.
- **Pausing:** the game is paused while it is open. The renderer keeps redrawing
  the last frame under the menu and doesn't hand control back to the game, so the
  game waits. Game audio is muted while it is open.
- **Look:**
  - The game's own Chapter Select palette: gold-to-orange title, orange selected
    row, olive other rows.
  - A dark framed panel with a full moon.
  - The font is "Mountains of Christmas" (Font Diner, Apache 2.0, `assets/fonts/`),
    a hand-lettered face in the film's style. If it is missing, Georgia is used.
  - The game's own menu words are pre-drawn pictures (texture probe tp1:
    `CHAPTER SELECT`, `CONFIRM` ... are images), so they cannot spell new text.
- **Controls:**
  - Up/down picks a row and left/right changes its value.
  - A / Enter changes the value or starts a rebind; Y / Delete puts the row back
    to its default.
  - LB/RB, LT/RT, Q/E or Tab change the page.
- **Saving:**
  - Every change is applied at once and written to `settings.ini`, which keeps its
    comments.
  - Rows marked (restart) take effect at the next start.
  - Test scripts use their own file (`logs\settings_test.ini`, `OOGIE_SETTINGS`).

| Page | Row | Values | Status |
|---|---|---|---|
| Display | Display Mode | Windowed / Fullscreen (borderless; also F11, Alt+Enter) | Done, Verified (ws2/ws3, mn1) |
| Display | Monitor | The window's monitor / 1..n | Done (one monitor here, so not tested with two) |
| Display | Window Size | Auto / 640x480 ... largest that fits; 16:9 sizes when widescreen | Done |
| Display | Aspect Ratio | 4:3 (Original) / Widescreen (Fill Screen) / Stretch to Window | Done. Widescreen now uses the window's or monitor's own shape (16:10, 16:9, 21:9 ...), so the 3D fills it edge to edge (fl1: 2880x1800 fullscreen, no bars) |
| Display | Hide Black Borders | On / Off | Done: the black bands the game paints along the edges (17 px top and bottom in play, film bars in cutscenes) are not drawn (fl1) |
| Display | Sides of 4:3 Screens | Black / Blurred Picture / Stretch | Done: menus and films keep 4:3, and the sides show a dark blurred copy of the picture (fl1) |
| Display | Field of View | 70% to 140% (100% original) | Done, Verified (fov1: 130% shows more of Town Square) |
| Display | Vertical Sync | On / Off | Done. When the refresh rate is a multiple of the cap, vsync does the pacing |
| Display | Frame Rate Cap | 30 / 60 (Original) | Done. "Off" was removed on 2026-10-19: the game's logic, films and music run per frame, so it ran them fast (owner) |
| Graphics | Render Resolution | Auto, Native (match my screen), 480p, 720p, 900p, 960p, 1080p, 1200p, 1440p, 1800p, 1920p, 2160p (4K) | Real sizes and Native added 2026-10-22 (any scale, not only whole multiples; rs1080, rsnat2: Native 2400x1800 at a steady 60 fps on the Arc 140V). Auto: the largest whole multiple the window holds, at most 2x on Intel graphics, one step down when 3D frames run below 60 fps for 3 s. Old settings files (1-4) still read as before. If the game drops below 60 fps it runs slow: pick a lower size |
| Graphics | Anisotropic Filtering | Off, 2x to 16x | Done |
| Graphics | Anti-Aliasing | Off / Low / High (default High) | Done 2026-10-22: an edge-blend pass (the FXAA idea, own code) on the finished frame at render resolution; pp_split |
| Graphics | Sharpening | Off / Low / Medium / High (default Low) | Done 2026-10-22: contrast-adaptive (after AMD CAS's idea, own code), after the AA pass |
| Graphics | Show Frame Rate | Off / On | Done, Verified (mn1) |
| Audio | Master Volume | 0 to 100 | Done (patch `audio_volume`) |
| Audio | Mute in Background | Off / On | Done |
| Controls | Rumble | Off / Normal / Strong | Done (not tested on a pad) |
| Controls | PlayStation and Other Pads | On / Off (restart) | Done |
| Controls | Reset Keyboard Keys / Controller Buttons | action | Done |
| Controls | Connected Controllers | list | Done |
| Keyboard | 24 rows, one per pad control | press the new key; Delete clears | Done |
| Controller | 16 rows, one per Xbox button | press the pad button | Done (not tested on a pad) |
| About | Settings File, Open Settings Folder, Reset All Settings, Close Menu | | Done |

### How 16:9 widescreen works (Verified: mn1, hud5)
- The game builds its camera with `sub_001E4E86(out, fovy, aspect, zn, zf)` at a
  fixed aspect of 4/3. An entry hook (`recomp_manual.c`) multiplies aspects
  between 1.2 and 1.4 by 4/3, so the 3D world is drawn for 16:9 into the same
  frame and shown 16:9: more at the sides, nothing stretched.
- A frame counts as widescreen only if it drew 3D into a frame buffer. Menus,
  movies and loading screens are 2D and stay 4:3.
- The 2D overlay (health, subtitles, hints, the Continue screen) is drawn with a
  3-instruction pass-through vertex program in screen pixels. In a widescreen frame
  it is squeezed by 3/4 about the centre, so it keeps its shape. Full-width quads
  and the off-screen edge masks are left alone. Effect: the HUD sits inside the
  middle 4:3 area. Moving it to the screen edges, as Burnout's port does, is a
  possible refinement.
- Objects at the new side areas could be culled by the game's 4:3 frustum
  (Hypothesis; not seen in hud5).

### Status of other features

| Feature | BLACK / Burnout ports | Oogie port | Where |
|---|---|---|---|
| Xbox 360 / One / Series pads | XInput / SDL | **Done**: XInput, wired, adapter or Bluetooth, hot-plug | `port/src/pc_input.c` |
| PS4 / PS5 / Switch Pro / other pads | SDL | **Done (untested on hardware)**: SDL2 2.30.9 GameController, USB or Bluetooth | `pc_input.c`, `bin\SDL2.dll` |
| Rebinding | in-game pages | **Done**, in the menu | |
| Saves in the game folder | yes | **Done**: `saves\UDATA` (KNOWN_ISSUES #52) | `play.bat` |
| Anti-aliasing, sharpening | Burnout | **Done** (F1 > Graphics) | `d3d11_backend.c` post_process |
| HUD at the screen edges in 16:9 | Burnout | planned | |
| High frame rate (above 60) | 60 in BLACK | needs the game's timestep found | planned |
| Exclusive fullscreen, HDR | BLACK (HDR) | not planned yet | |
| Accessibility, cheats | no | planned (roadmap) | |

### Added 2026-10-21 (night)
- **Save backups:** `play.bat` copies `saves\UDATA` to `saves_backup\<date_time>\` before every
  session and keeps the ten newest. To restore one, close the game and copy that folder's
  `UDATA` over `saves\UDATA`.
- **Problem reports:** a session that crashed, or froze for 15 s, leaves
  `reports\<date_time>\` with its log and the settings it ran with.
- **Efficiency mode off:** the game opts out of Windows' power throttling (EcoQoS) and runs
  at above-normal priority, so a hybrid laptop CPU keeps it on its fast cores
  (`OOGIE_ECO_QOS=1` keeps Windows' default).
- **Flicker-free presentation:** the screen shows the buffer each game frame finished in
  (pause menu, inventory, chapter banner: KNOWN_ISSUES #57, #59).

### Added 2026-10-22/23 (dev cycle 3)
- **Render Resolution in real sizes** and **Native**; **Anti-Aliasing** and **Sharpening** (F1 > Graphics).
- **HUD at Screen Edges** (F1 > Display, default On): in widescreen, 2D pieces wholly in a screen corner (outer 30% across, top or bottom 30%) are anchored to the wide screen's side; menus, subtitles and boxes stay centred.
- **F12 screenshot:** the shown picture (without the F1 menu or the fps counter) as a PNG in `screenshots\`, written on its own thread.
- **First-run setup:** without `game\default.xbe` the program asks for the player's own disc image, checks it is the supported version (SHA-1 of default.xbe), and copies the files with a progress window. The image is only read. `--extract-iso <image> <folder>` does the same without a window.


## Part 6. Chapter, world and area selection (RE_CHAPTER_SELECT)

_Source: our `docs/RE_CHAPTER_SELECT.md`, included unedited except headings._

## How the game picks chapter, world and area (for a chapter-jump test helper)

Read-only research, 2026-10-23 (side session), from `build_out/disasm/asm/text.asm` and the
XBE's data. Verified = read from the code or data; Hypothesis = inferred, not run.

### Globals (Verified: code)
| Address | Meaning | Evidence |
|---|---|---|
| 0x00413164 | Current chapter, 1..24 | written by sub_00040950 (arg) and sub_00077EF0 (= 1) |
| 0x00413168 | Set to 1 by the New Game reset (meaning unknown) | sub_00077EF0 |
| 0x0041316C | Current world (0x00..0x0C) | read by both callers of the level loader |
| 0x00413170 | Current area within the world | same |
| 0x00413174 | Cleared on chapter start (Hypothesis: entry point / sub-area) | sub_00040950 |
| 0x003DF9BC | Copy of the chapter number | sub_00040950 |

### Functions (Verified: code)
- **sub_00040950(chapter)**: start chapter `chapter` (1..24; anything else spins in place).
  `[0x413164] = chapter`; world and area from the table at **0x002C5AA0** (16 bytes per
  chapter: world, area, a, b); `[0x413174] = 0`; `[0x3DF9BC] = chapter`; except for chapter 9
  it calls `sub_000DDEF0(a, b)` (Hypothesis: spawn point or mission set-up).
- **sub_000576F0(world, area)**: the level loader. Valid if `world < 13` and
  `area < count[world]`, from the table at **0x0026C2D0** (8 bytes per world: first stm file
  index, area count). Builds `host0:data/world/%02x%02x`; the area file is stm_data\WWAA.lz.
- **sub_0004BD80** and **sub_0019D2A0**: call the loader with `([0x41316C], [0x413170])`.
- **sub_00077EF0**: New Game reset: chapter 1, [0x413168] = 1, world 0, area 0, and clears
  progress data from 0x413178 up to 0x4133F4 and more.
- **sub_0004D9E0** (around 0x4DD17): sets world/area from a second table at **0x002C7E48**
  indexed by `[0x3E0020] + [0x3E0024]`, then starts a game task at 0x4BD80 (the loader) with
  sub_00058540. Hypothesis: the Chapter Select (or a debug stage select) load path. The same
  function (0x4DE2F) also sets the world and then area 3 or another depending on game flags
  0xF1/0xF2 (sub_000D9A70).
- **sub_00043A90** (0x43B7D) has the same flag-0xF1/0xF2 pattern.

### Chapter table at 0x002C5AA0 (Verified: XBE data)
| Ch | World | Area | a | b | First file | Walkthrough name |
|---|---|---|---|---|---|---|
| 1 | 0 | 0 | 0 | 12 | 0000.lz | Jack's Return |
| 2 | 0 | 0 | 0 | 4 | 0000.lz | The Witching Hour |
| 3 | 1 | 0 | 4 | 0 | 0100.lz | The Hanging Tree |
| 4 | 1 | 0 | 4 | 2 | 0100.lz | Grave Danger |
| 5 | 1 | 0 | 4 | 6 | 0100.lz | The Crypt Creeper |
| 6 | 1 | 0 | 4 | 5 | 0100.lz | The Doctor's Hunch |
| 7 | 2 | 0 | 8 | 5 | 0200.lz | The Lightning Round |
| 8 | 2 | 7 | 12 | 1 | 0207.lz | Changing Brains |
| 9 | 2 | 11 | 12 | 3 | 020b.lz | Pumpkin Patch Possession |
| 10 | 3 | 2 | 16 | 0 | 0302.lz | Barrel's Maze |
| 11 | 3 | 2 | 16 | 2 | 0302.lz | Peril in the Pumpkin Head |
| 12 | 3 | 4 | 17 | 1 | 0304.lz | Runaway Clown |
| 13 | 4 | 1 | 19 | 6 | 0401.lz | The Vampire Brothers |
| 14 | 4 | 3 | 21 | 1 | 0403.lz | The Mayor's Madhouse |
| 15 | 4 | 6 | 24 | 0 | 0406.lz | Rooftop Rumble |
| 16 | 5 | 0 | 25 | 0 | 0500.lz | The Grizzly Gauntlet |
| 17 | 5 | 3 | 28 | 0 | 0503.lz | Casino Clash |
| 18 | 0 | 2 | 0 | 15 | 0002.lz | Spiral Hill |
| 19 | 7 | 0 | 31 | 0 | 0700.lz | The Hinterlands, Part 1 |
| 20 | 6 | 0 | 29 | 7 | 0600.lz | Fire and Ice Frenzy |
| 21 | 6 | 0 | 29 | 5 | 0600.lz | The Hinterlands, Part 2 |
| 22 | 8 | 2 | 40 | 5 | 0802.lz | Christmas Town |
| 23 | 8 | 0 | 40 | 6 | 0800.lz | Saving Sandy |
| 24 | 9 | 1 | 42 | 1 | 0901.lz | Taking out the Trash |

Consistency check (Verified, owner logs): Chapters 1-2 load 0000.lz and Chapter 3 loads
0100.lz. Chapter names are from docs/reference/walkthrough (order assumed to match the
game's chapter numbers).

### Suggested chapter-jump helper (Hypothesis, not built)
1. From the title or a loaded save, call `sub_00040950(N)` from a debug key or env var
   (e.g. `OOGIE_START_CHAPTER=N`), on the game thread at a safe point (between frames), then
   let the game's own chapter-start flow run, ideally the same path Chapter Select uses
   (sub_0004D9E0 / task 0x4BD80).
2. Simpler first test: hook sub_00040950 entry and replace its argument with N when the env
   var is set, then start a New Game: the game should begin at chapter N with its own set-up.
3. Unknown: what else a chapter start needs (story flags, unlocked forms like Pumpkin King
   or Santa Jack, items). Chapters past 3 may misbehave if progress flags (0x413178..) are
   unset; loading a save at the chapter end is the safer route for those.


## Part 7. Running on every PC (COMPATIBILITY)

_Source: our `docs/COMPATIBILITY.md`, included unedited except headings._

﻿# Xbox Runtime Compatibility Status

Status words: **Working** (seen in a run log), **Reached** (game calls it, outcome not yet confirmed), **Untested**.

| Subsystem | Provided by | Status for Oogie | Evidence |
|---|---|---|---|
| XBE load, section map, 0x10000 base | xboxrecomp kernel | Working | run1: 31/31 sections loaded |
| Kernel thunks (125 imports) | xboxrecomp kernel_bridge | Working | run1: 125 bridged, 2 extra stubbed |
| Threads (PsCreateSystemThreadEx) | kernel | Working | run1: main thread created |
| Heap / virtual memory | kernel | Working | run1: NtAllocateVirtualMemory OK |
| Contiguous memory (GPU) | kernel | Working | run1: 4 allocs, 5.8 MB |
| Save-data paths (UDATA/TDATA) | kernel file layer | Reached | run1: TitleMeta/TitleImage/SaveImage opened |
| GPU interrupt (KeConnectInterrupt vec 3) | kernel | Reached | run1 |
| D3D8 (statically linked XDK code) | recompiled game code | Reached | device init runs, 640x480 surface |
| NV2A push buffer execution | xboxrecomp nv2a_pb_exec (RECOMP_PB_EXEC=1) | Reached | run3: 245 methods seen, 0 draws |
| D3D fence wait | Oogie entry hook on sub_001D78D0 | Working | run4: semaphore 0x80001000 registered, frames flip |
| Rendering (GPU) | Oogie Direct3D 11 back end `port/src/d3d11_backend.c` (`OOGIE_D3D11=1`, default in play.bat) | Working | d3d3: menus, movies (greyscale), Chapter 1 cutscenes and gameplay; 7-8 fps in a level |
| Rendering | PB executor CPU rasteriser | Working (slow) | run4: disc-error screen + Now Loading animation rendered correctly |
| Present / display window | xboxrecomp fb_present (`RECOMP_FB_WINDOW`) | Working on Windows | win_run5: 35 fps, 3,373 flips |
| MCPX APU (audio chip) | xboxrecomp apu (xemu), `RECOMP_AC97_READY=1` | Reached | run7c: title starts APU, DSP handshake, ISR 0x00208AD1 claims interrupts; no audible output yet |
| XACT engine | recompiled game code on APU | Reached | run7c: loads 9 stream banks + se banks + effect.bin |
| Controller (XPP / USB) | xboxrecomp OHCI + usb_gamepad (`RECOMP_USB=1`); host pads through `port/src/pc_input.c`: XInput (Xbox 360/One/Series) and SDL2 (PS4, PS5, Switch Pro, others); keyboard; rebinding in `settings.ini`; rumble forwarded | Working (scripted pad, real keyboard) | 2026-10-18 in1/in2: keyboard via settings.ini Verified; scripted pad boot to chapter Verified. Real pads not yet tried (none connected). See docs/PC_FEATURES.md |
| Bink video | recompiled Bink | Decodes, renders black | win_run12: frames advance; YUV planes drawn as LIN_L8 textures, colour pass missing (KNOWN_ISSUES #17) |
| Title screen and menus | recompiled game code | Working | win_run19 |
| Game data reads | kernel file layer, `game/` = D: | Working | XACT_data, data.inf, stm_data read OK (async + APC) |
| SwitchToThread | Oogie override sub_001BD332 | Working | run8 |
| Suspended thread creation | Oogie hook sub_000584E0 (runtime ignores CreateSuspended) | Working for the job system | win_run3 |
| Thread suspend/resume | runtime + Oogie hook sub_001BC71B (SAME_ACCESS) | Working | win_run5: 0 failures |
| Thread IDs (GetCurrentThreadId) | Oogie hooks sub_001BD49C / sub_001BD404 | Working | win_run5 || Saving (XcSHA/HMAC signing) | kernel crypto | Untested | |


### Keyboard layout (testing, 2026-10-17)

Owner request: arrow keys as the face buttons in the pad's diamond, until the options menu gets rebinding. Down = A, Right = B, Left = X, Up = Y; T/F/G/H = D-pad; Enter = Start, Backspace = Back, WASD = left stick, IJKL = right stick, 1/3 = triggers, Q/E = White/Black; Z X C V still work. Implemented in the D3D11 window (`key_target` in `port/src/d3d11_backend.c`); `OOGIE_ARROW_FACE=0` restores arrows = D-pad.

## Running on other PCs (cross-machine checklist)

Started 2026-10-19 (owner: "this PC port will run on a lot of different machines, Nvidia, AMD etc. ... make sure it just works"). Status words as above, plus **To do**.

### Requirements (what the code needs)

| Item | Need | Notes |
|---|---|---|
| OS | Windows 10 or 11, 64-bit | D3D 11.1 runtime for partial clears (ClearView); without it a clear covers the whole surface |
| Direct3D | feature level 10.0 or newer | Shaders are SM 4.0. The GPU vertex-program path uses a StructuredBuffer and needs 11.0; below it each such draw is declined and the executor runs the program on the CPU (slower) |
| No GPU / broken driver | WARP (Windows' software D3D11) | Used automatically when no hardware device can be made; slow but runs |
| Controllers | XInput built in; SDL2.dll next to the exe for PlayStation and other pads | Without SDL2.dll, Xbox pads and keyboard still work (To do: check) |
| Audio | XAudio2 2.9 (Windows 10+) | |
| Disk | write access to the game folder (saves\, logs\, settings.ini) and %LOCALAPPDATA%\ProjectOogie (shader cache) | OOGIE_SHADER_CACHE=0 turns the cache off |

### Device creation (port/src/d3d11_backend.c, init_device)

- Hardware with feature levels 11.0, 10.1, 10.0; then WARP. The log's `[D3D11] GPU:` line names the adapter, vendor/device IDs, video memory, driver version, feature level, WARP and debug-layer state: the first line to read in a report from another PC.
- Nothing works: a message box asks for a driver update.
- `OOGIE_D3D_WARP=1` forces WARP (stand-in for a GPU we have not tested; catches Intel-only assumptions).
- `OOGIE_D3D_DEBUG=1`: D3D debug layer (needs the Windows "Graphics Tools" optional feature); warnings and errors go to the log as `[D3D11 debug]` lines (at most 300). Not installed: the device is made without it.

### Shader cache

Compiled DXBC keyed by a hash of HLSL source, entry point and target. DXBC is vendor-neutral (each driver compiles it at load), so the cache is safe across GPUs and drivers; no per-driver key is needed. Delete %LOCALAPPDATA%\ProjectOogie\shaders to rebuild.

### Checklist

| Check | Status | Evidence |
|---|---|---|
| Hardware device, Intel Arc 140V (this laptop, driver 32.0.101.8724, feature level 11.0) | Working | every log since 2026-10-17; dbg1 `[D3D11] GPU:` line |
| Feature-level fallback 11.0 -> 10.1 -> 10.0 | To do (code in) | |
| WARP run: boot, title, intro film | Working | warp1 (2026-10-19): "Microsoft Basic Render Driver ... WARP (software)", intro film at 60 fps |
| Debug-layer run: no ERROR/CORRUPTION through boot, menus, Town Square | To do | dbg1: layer not installed here (0x887A002D, DXGI_ERROR_SDK_COMPONENT_MISSING); the fallback without it worked. Needs the Windows optional feature "Graphics Tools" (owner's decision to install) |
| NVIDIA GPU | To do | needs a tester |
| AMD GPU | To do | needs a tester |
| Laptop with two GPUs | To do | default adapter; Windows' per-app GPU setting decides |
| High-DPI (125-200 %) | Working | per-monitor DPI awareness (fl1, 2880x1800) |
| Second monitor | To do | |
| Refresh rates other than 60 Hz | Untested | frame cap paces 30/60 with a timer when vsync cannot |
| Non-English Windows, user names with spaces or non-ASCII | To do | %LOCALAPPDATA% read with the ANSI API |
| Antivirus scanning the growing log | Known cost | log writes can stall (KNOWN_ISSUES #55) |

### Known risks across machines

- Thread timing: the game's threads run truly in parallel on a PC, and core count and speed change the timing. Races found so far: #48, #50, #61 (open). The main risk on other machines.
- GPU speed: a slow GPU makes the game wait in Swap and starves other threads (#62); lower Render Resolution on weak GPUs.


## Part 8. Our improvement plan at the time of writing (IMPROVEMENT_PLAN)

_Source: our `docs/IMPROVEMENT_PLAN.md`, included unedited except headings._

## Project Oogie: improvement plan (2026-10-20 evening)

What the PC port needs, in the order I will work on it. Written after a review
of every entry in `KNOWN_ISSUES.md`, the owner's play logs of 10-19 and 10-20,
and today's soak results. Status markers follow the house rule: **Verified**
means a log shows it, **Hypothesis** means it does not yet.

### Where the port stands

- Boots, plays films, menus and all chapters tried so far at 60 fps on the
  owner's laptop (Intel Arc 140V), with saves, controller and keyboard, a
  themed settings overlay (F1), widescreen, render scale and remapping.
- The default build survives 4-minute combat soaks with Continues
  (bk1-bk4, 4 of 4, **Verified**).
- What still goes wrong in the owner's sessions, roughly by how often:
  1. The emulated GPU loses its place in the command ring (#66). This one
     fault shows up as flicker, replayed frames, a stuck "GPU done" signal
     (freezes, #54) and probably the banner blink (#59). About 1 run in 3 has
     at least one.
  2. Audio-side stalls and crashes (#55, #56, #64): fixed today for the cases
     seen (50 ms forced voice stop; XACT never entered without its lock).
  3. Controller-driver crash (#43): fixed today (USB DPC lock), 9 runs clean.
  4. Rarer, one-off problems: title film missing on one launch (#42), all
     chapter cards showing Chapter 1 (#49), barcode subtitles (#63), missing
     announcer lines (#47), load-screen shapes (#60).

### Tier 1: stability (tonight)

| # | Work | Why |
|---|------|-----|
| 1 | Find why the command reader goes out of step (#66): re-read the bad word 2 ms later to tell "read before it was written" from "decoded wrongly"; log every CALL the reader takes | It is behind most visible faults; today showed it is not two game threads writing at once (it happens under the one-thread lock too) |
| 2 | Bink worker created in a safe order (`sub_0022F3D0`) | The worker could start before its mutex and event exist; likely cause of #42 |
| 3 | Lock mode (`RECOMP_GUEST_LOCK=1`) to gameplay | Removes a whole class of races (USB, XACT, render lists); boots further after today's fair lock |
| 4 | Longer soaks of the default build after each change | Every fix tonight is kept only if soaks stay at least as good as bk1-bk4 |

### Tier 2: correctness the owner has reported

| # | Work |
|---|------|
| 5 | #49 chapter cards: Chapter 1 -> Chapter 2 in one session (needs a route that moves the Chapter Select cursor) |
| 6 | #63 barcode subtitles: needs a save just before the Ch3 dt07 cutscene (owner) |
| 7 | #47 results-screen announcer |
| 8 | #59 / #60 load screen: banner blink, then rebuild the sweeping shapes for widescreen |

### Tier 3: PC-port features

| # | Feature | Notes |
|---|---------|-------|
| 9 | Automatic save backups | Each launch copies `saves\` to `saves_backup\<date-time>\` (keeps the last 10); a crash can then never cost the owner progress |
| 10 | One-click crash report | On a crash or long freeze, the log, settings and the last frame go into `reports\<date-time>\`, ready to send |
| 11 | Performance mode presets in F1 | "Battery / Balanced / Quality" setting render scale and filtering together (render scale 3 fullscreen is heavy on the Arc 140V) |
| 12 | Later, after baseline gameplay is solid | HD texture packs (deferred by the hard rules), modern load transition, 60 fps cutscene pacing |

### Rules I follow while doing this

- Nothing touches `game\`, the owner's `saves\`, or `third_party\xboxrecomp`
  (runtime changes are build-time patches in `port\patches`).
- A change stays on by default only if soaks with it are at least as good as
  without it; otherwise it stays in the build behind a switch, and the doc
  says so.
- No commits.

### Status at 04:30 on 2026-10-21

| # | Item | Result |
|---|------|--------|
| 1 | Command reader out of step (#66) | Much rarer: 1 run in 3 before tonight, 1 in about 25 since the Bink fix (45 min of 15-minute soaks clean). Cause narrowed to D3D_SetScissors' output being read before it was complete; the reader now logs the 24 commands before any desync (always) and can re-read the stream later (OOGIE_PB_REREAD=1). |
| 2 | Bink worker order (#42) | Done; every soak opens all six films. |
| 3 | Lock mode | Boots through every film now; still stalls in the level (task poll). Off. |
| 4 | Soaks | ln1-ln3 (15 min each), os/of (owner settings, fullscreen 3x): all clean. |
| - | Pause / inventory menu flicker (#57) | Fixed: the screen shows the buffer each frame finished in. |
| - | Chapter banner blink (#59) | Fixed by the same change (Verified lt9). |
| 9 | Save backups | Done (play.bat). |
| 10 | Problem reports | Done (play.bat, reports\). |
| 11 | Performance | Render Resolution "Auto"; Windows efficiency mode off for the game. |
| - | Test routes at render scale 2-4 | Fixed (the routes now read scaled frame dumps), so the owner's settings can be soaked. |

Still open, in order: #49 chapter card in one session (needs a Chapter Select route that moves the cursor), #63 barcode subtitles (needs a save before the Ch3 dt07 cutscene), #60 door transition shapes (needs a door route), #47 announcer lines, lock mode's task stall.

### Dev cycle 3 (from 2026-10-22 afternoon)

The menu flicker is fixed and confirmed by the owner (play.log 13:30: 0 broken frames, the
semaphore fix corrected the start-up race 20 times). Gameplay is solid. This cycle makes it
look and feel like a modern PC release, and makes it something other people can just run.
Order is by value to the player; each item is kept only if tests and soaks stay as good.

| # | Area | Work | Why |
|---|------|------|-----|
| 1 | Picture | Render Resolution as real sizes: 720p, 900p, 1080p, 1440p, 4K and "Native (match my screen)", not only whole multiples of 640x480 | Owner asked. Today the steps jump 480 -> 960 -> 1440; on the Arc 140V 1440 is heavy and 960 is soft. In-between and native sizes fit every screen |
| 2 | Picture | Anti-aliasing: SMAA or FXAA as a final pass, Off / Low / High | The 2005 art has hard jagged edges at any resolution; a post pass costs little |
| 3 | Picture | Sharpening (contrast-adaptive), Off / On | Pairs with AA and with resolutions below native |
| 4 | Smoothness | The remaining long frames (owner 13:30: 23 over 50 ms in about 9 min): sort by cause and fix | "More slowdowns" is mostly fixed (#55 voices), not all |
| 5 | Sound | Audio underruns ([XA2] underrun x116 in the 12:21 session): find what starves the mixer | Underruns are audible crackles |
| 6 | Correctness | #47 announcer, #60 door transition in widescreen, #49 chapter cards, #63 barcode subtitles | Owner reports; #47 needs a save just before a chapter end |
| 7 | Widescreen | HUD pinned to the screen edges (option), as in modern remasters | Planned since the widescreen work |
| 8 | Convenience | Screenshot key (F12, PNG at render resolution); option to skip the start-up logos | Small, expected in a PC port |
| 9 | Release | First-run setup inside the exe: pick your own disc image (ISO), it checks it and extracts the game files; one folder with the exe, no .bat or Python needed | "People are going to expect it to just run" (owner) |
| 10 | Research | Above 60 fps: find the game's per-frame timestep; only if logic, films and music can stay at the right speed | Highest-risk item, last |
