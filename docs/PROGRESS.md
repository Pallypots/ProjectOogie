# Progress Log

## Session 1: 2026-10-16 (cloud, Wine)

### Done (all verified, see TEST_RESULTS.md)
- Located the game: `Tim Burton's ... (USA).xiso.iso` (XDVDFS, partition base 0). Wrote `tools/xiso_tool.py` (read-only lister/extractor). Inventory: 723 entries in `docs/game_inventory.tsv`.
- Extracted and hashed `default.xbe` (SHA-1 667c5bd8...). Read-only copy in `game/`.
- Reviewed XboxRecomp v0.14.0 and both BLACK ports (`docs/BLACK_COMPARISON.md`). Decision: XboxRecomp whole-XBE recompilation.
- Ran the full XboxRecomp pipeline: 10,370 functions, 0 translation failures, 1.4 M lines of C.
- Cross-built a native Windows x64 exe with MinGW-w64. Booted it under Wine through kernel init, D3D init, first rendered frames, audio chip start, XACT banks, `data.inf`, `shock.stm`, `door.stm`.
- Fixes in `port/src/recomp_manual.c`: 5 seeded functions, D3D fence-wait hook `sub_001D78D0`, SwitchToThread override `sub_001BD332`, TLS declaration fix.

## Session 2: 2026-10-16 (Claude Code, real Windows, `C:\ProjectOogie`)

### Done (verified, see TEST_RESULTS.md win_run1 to win_run5)
- Project now lives at `C:\ProjectOogie` (outside OneDrive). The Windows-ready files from `Downloads\projectoogie_claude_code` were copied in; the older session-1 versions are backed up in `_transfer\backup_before_claude_code\`. XBE hash verified.
- Installed Visual Studio 2022 Build Tools (MSVC 19.44), CMake 4.4.4, Python 3.12.10 + capstone 5.0.7 (winget, with the owner's OK). Cloned XboxRecomp v0.14.0 (eb0aa1d) into `third_party/xboxrecomp`.
- Full pipeline and **first MSVC build** linked first time (same numbers as session 1).
- Extracted `movie/` (24 Bink files) from the disc image into `game/`.
- First runs on real Windows. Windows exposed three thread bugs in the runtime that Wine's timing had hidden. All worked around in `recomp_manual.c` (KNOWN_ISSUES #11 to #13):
  1. Threads created "suspended" started at once, so the boot job finished before the main thread registered it: Now Loading forever. Hook `sub_000584E0_enter`.
  2. The scheduler's handle to the main thread had no access rights: every suspend/resume failed. Hook `sub_001BC71B_enter`.
  3. Every worker thread had thread ID 0: the scheduler suspended the wrong threads. Hooks `sub_001BD49C_enter` / `sub_001BD404_enter`.
- Result: the session-1 blocker (wild call through 0xFFFFFFFF) is gone; it was a symptom of 2 and 3. The game now runs the Now Loading screen at ~35 fps with no errors.
- Added `scripts/run_capture.ps1` (repeatable test run) and a call-chain dump on the first wild indirect call (`OOGIE_ICALL_STOP=1` aborts there).

### Current blocker
Level load never finishes (KNOWN_ISSUES #15): after `shock.stm` and one `door.stm`, no more files are opened in 100 s.

## Session 2, sprint 2: 2026-10-16 (title screen)

### Done (verified, TEST_RESULTS.md win_run6 to win_run19)
- Added a per-thread stack sampler (`OOGIE_SAMPLE_SECS`) and scheduler logging. Found that the game-logic thread 0x0019D8E0 stopped itself because it read its own thread ID as 0. Fixed with a full override of XAPI GetCurrentThreadId `sub_001BD50A` (host-TLS ID). Level/front-end loading now completes.
- Fixed a race in the job-start workaround (gate on job.func, not job.state). Boot to the intro movie is now repeatable (2 of 2 runs).
- Seeded 14 functions only reachable through pointers (title path 0x000791A0, controller driver, Bink colour converters, D3D/D3DX); found with run logs and a scan for code outside any known function.
- `main.c` now starts the emulated USB controller (`xbox_OhciInit`); the template never did.
- **Milestone: title screen operational.** With a scripted Start press the game skips the intro movie, shows the title ("PRESS START BUTTON"), searches for save data and opens the Load Game menu (`docs/title_screen_win_run19.png`, `docs/load_game_menu_win_run19.png`).
- `play.bat` added for the owner (controller or keyboard, no time limit). `run_test.bat` and `scripts/run_capture.ps1` now enable the controller.

### Current state and blockers
- Movies decode but show black (no colour pass for Bink's YUV textures in the CPU rasteriser). KNOWN_ISSUES #17.
- Everything renders on the CPU at 5 to 35 fps. KNOWN_ISSUES #18.
- Not yet tried: starting a new game, saving, a real controller in a person's hands.

### Next actionable task
1. Drive the menus with `RECOMP_PAD_SCRIPT` (New Game) toward the first playable level; fix what breaks.
2. Start the Direct3D 11 back end (`nv2a_backend.h` interface: clear/draw/flip, textures via `nv2a_backend_decode_texture`), then add the register-combiner YUV pass so movies show. BLACK 2006 Recomp's D3D11 layer is the reference.
3. Confirm whether the `sub_001D75F0` latch hook (#14) is ever needed; remove it if not.
4. Report KNOWN_ISSUES #11, #12, #13, #19 upstream to XboxRecomp (owner's decision).

## Session 2, sprint 3: 2026-10-16/17 (into the first level)

### Done (verified, TEST_RESULTS.md live1 to live23)
- Live, screenshot-driven sessions: `scripts/live_start.ps1`, `live_press.ps1` (send presses), `live_flips.ps1` (save finished frames), `live_route_ch1.ps1` (boot to Chapter 1 hands-free, self-correcting menu presses).
- Menus verified: title, main menu (New Game / Load Game / Options), difficulty select, opening cutscene skip.
- Build-time patches to the runtime without touching the checkout (`port/patches/`, `oogie_patch_source` in `port/CMakeLists.txt`): 32 thread stacks (#23), per-channel clears (#24), vertex current value, plus two inert diagnostics.
- Race-free scheduler park/wake (overrides of sub_001AD4B0 / sub_001AD4F0) fixed an intermittent boot stall (#29).
- Release builds now write a PDB (crash reports name the function).
- **Milestone: first playable level reached.** "Chapter 1 – Jack's Return", Town Square in 3D, then gameplay with Jack, skeleton enemies and the HUD (`docs/gameplay_town_square_live23.png`). The opening in-engine cutscene holds a black screen; Start skips it (#25).
- Two more missed functions found during gameplay and seeded (0x0008CFB0, 0x0001C460).

- After seeding those, the opening cutscene **plays** instead of holding black (live24/25).
- Swizzled render targets patch: the striped artefacts are gone; frames are clean (`docs/cutscene_clean_live25.png`).

### Open
- Jack's movement not yet confirmed (tests so far were during cutscenes).
- Pause menu black (#31); ~1-2 fps (#26), so cutscenes run ~15x slow; intermittent GPU-executor crash on the title (#28); movies black (#17).

### Next actionable task
1. Begin the Direct3D 11 back end (`nv2a_backend.h`): at 1-2 fps every test takes many minutes, and the D3D11 path is the planned fix for speed. Reference: BLACK 2006 Recomp's D3D11 layer.
2. Once faster: play past the opening cutscene, confirm Jack moves and fights, test the pause menu and saving.
3. Bink colour pass (register combiners) so movies show.

## Session 2, sprint 4: 2026-10-17 (Direct3D 11 renderer, playable)

### Done (verified, TEST_RESULTS.md d3d1 to d3d3)
- Wrote the Direct3D 11 back end (`port/src/d3d11_backend.c`): game surfaces as GPU render targets (sampled directly when the game reads them as textures), cached decoded textures, blend/alpha/depth/stencil/colour-mask mapping, present, keyboard, screenshots. Speed: 25 fps in movies, 7-8 fps in Town Square (was 6 and 1-2).
- Fixed the intermittent GPU-executor crash (#28) with a build-time bounds patch; the new PDB named the function.
- **Milestone: first playable level.** Chapter 1 cutscenes play (Jack walks in, Soul Robber close-up, dialogue), gameplay follows, the controller drives Jack, and combat works: a skeleton is smashed into bones and drops pumpkin pick-ups (`docs/combat_d3d11.png`).
- `play.bat` uses the new renderer (`play.bat cpu` for the old one); `live_start.ps1 -D3D`.

### Performance work (same sprint, verified with tools/sampler and the window's fps)
- Level frame rate 7-8 -> ~40 fps: quiet GPU logging, kickoff acknowledgement during long segments, DirectSound DSP watchdog fix (it reset the stub DSP 8x/s with 10 ms busy-waits), parallel vertex programs, two per-draw getenv calls removed. Menus/movies held at 60 Hz by new frame pacing.
- `tools/sampler/sampler.exe <pid> <tid> [n] [ms] -callers`: sampling profiler with module names and exact caller offsets.
- `scripts/live_route_fast.ps1 -Name <run>`: boot to Chapter 1 gameplay at real speed, driven by log milestones.

### Next actionable task
1. Register-combiner support in the back end (colour movies; the title background movie also shows block noise).
2. Pause menu (black, #31), saving, audio check, play further into Chapter 1.
3. Vertex programs on the GPU (the remaining performance lever).

### How to resume
Work from `C:\ProjectOogie`. Rebuild after editing `recomp_manual.c`: `powershell -ExecutionPolicy Bypass -File scripts\regen.ps1 -SkipDisasm` (about 10 min; needs `C:\Program Files\CMake\bin` and the Python 3.12 folder on PATH). Test: `powershell -ExecutionPolicy Bypass -File scripts\run_capture.ps1 -Name win_runN`.

## Session 2, sprint 5: 2026-10-17 (movies)

### Done (verified, TEST_RESULTS.md mov2, bcap2, binkdiff, mov3, ch1m)
- **Colour movies.** The D3D11 back end recognises Bink's three-plane YUV draw (build-time patch `backend_extra_stages` exposes texture stages 1-3 and the combiner stage count) and converts BT.601 in the shader.
- **Movie speckles fixed: a recompiler bug.** New tool `tools/binkdiff` runs a captured call of the Bink plane decoder twice, as the original Xbox x86 code (natively, in a 32-bit process laid out like the Xbox) and as the game's recompiled C, and compares the pictures; a swap mode finds the one function whose recompiled version is wrong. Result: `movsx ebx, bp` in the inverse DCT was lifted without sign extension (KNOWN_ISSUES #35, 16 sites in the game, 8 of them in game code). `scripts/oogie_recomp.py` applies the corrected rule during code generation; the checkout stays untouched. After the fix the two versions match byte for byte and the movies are clean (`docs/movie_clean_after_movsx_fix.png`).
- Capture hook `OOGIE_BINK_CAPTURE=N` (`sub_00232C40_enter`), off by default.
- **Full brightness: register combiners in the D3D11 renderer.** The game keeps colours at half strength and doubles them in the NV2A register combiners; the renderer skipped those, so the HUD, menus and parts of the scene were half as bright. Each combiner program the game uses now becomes a small generated pixel shader (cached; 8 programs by early gameplay). New build-time patches pass the needed state and per-vertex data (`backend_vertex_extra_*`). ~35 fps in the level (39 without). KNOWN_ISSUES #36.
- **Pause menu works** (#31 closed): it was the same half-brightness problem, plus one test where Start skipped a cutscene instead (`docs/pause_menu_combiners.png`). Title screen in full colour over its movie (`docs/title_screen_d3d11.png`).

- **Audio verified by measurement** (au1): the sound sent to the speakers carries the intro movie, title music, chapter movie and gameplay sound, real stereo, no clipping. Not yet listened to by a person.
- **Save area verified** (#22): the game writes its title data to `%LOCALAPPDATA%\xboxrecomp\UDATA\42560004`. Writing a save needs the in-level save character (hands-on play).
- **Game timing understood** (#37, tc60/tc25): the game advances one fixed step per frame and targets 60 Hz, so any frame below 60 fps is slow motion. That made level speed a correctness problem.
- **60 fps in the level: vertex programs on the GPU.** Each NV2A vertex program is translated to an HLSL vertex shader (same rules as the runtime's interpreter) and cached; the executor hands whole batches to the D3D11 renderer (patches `vsh_access`, `backend_gpu_vsh`). Town Square 38 -> 60 fps; the opening scene now runs at full speed (gv3, `docs/gameplay_60fps_gpu_vsh.png`). `OOGIE_GPU_VSH=0` switches back.
- New scripts: `scripts/timing_cutscene.ps1` (reliable boot into Chapter 1, timing, `-Keep` to stay in gameplay), `scripts/timing_probe.ps1`.

- **Further into Chapter 1** (ex1-ex4): combat, defeat, the Continue screen and revive all work; after the first fight the game moves on to the Town Square fountain and Jack walks around it at 60 fps.
- **Memory fix** (#38): a Continue after a death crashed because the runtime never freed memory by default; `RECOMP_HEAP_RECLAIM=1` is now set by every launcher and the same sequence works.

- **More of Town Square checked** (ex5): fountain, well, side streets, the sealed-gate scenes and a vine gate with the objective marker all render right at 52-60 fps.
- **Shader disk cache** (#39): generated shaders are compiled once ever instead of once per session, removing most first-appearance hitches on later runs.

### Next actionable task
1. Story progress needs a person (or a known route): the objective past the vine gate is not obvious by scripted presses. Hands-on play would also cover the save character (#22).
2. Use the binkdiff method on other suspect code (it generalises to any self-contained function).
3. Fog check against a reference; programs that write c[] (still on the CPU) if any show up in the logs.

## Session 2, sprint 6: 2026-10-17 evening (owner's Chapter 1-2 report)

### Done (TEST_RESULTS.md au1-au17, ld1, ls1-ls15)
- **Music pumping fixed (#48, verified in Town Square, owner to confirm in the Oogie fight).** Two game threads were programming the sound chip at the same time: the sound libraries protect that by raising the CPU priority level, which stops other threads on the original single-core Xbox but not on a PC. The music then sometimes received a sound effect's volume settings, so it lost its right channel and its level followed the effect. `port/src/recomp_manual.c` now gives those four lock helpers a real shared lock. Before: wrong in 5 of 6 test runs; after: 0 of 4.
- **Stereo balance:** the stand-in for the Xbox audio effects chip put the centre channel and effect sends on the left speaker only (a 3-5 dB lean). Patch `apu_mixdown_stereo` spreads them over both (0.2 dB after).
- New diagnostics: `RECOMP_APU_BINS=1` (mixbin levels, voice routing changes, SetMixBinVolumes callers, cross-thread voice writes).
- **Loading a save works by script** (`scripts/load_save.ps1`): Load Game, GAMEDATA 1, Chapter Select, start a chapter, pause. The Chapter 2 title card from a fresh load shows the right picture (#49 needs a continuous session to show).
- **#43 lead:** every crash of the "USB driver" kind (4 logs) comes right after the audio chip's scratch-memory table changes from page 0x738000 to 0x73C000; the emulator's DSP doorbell helper then writes zeros into the new page. `scripts/repro_loop.ps1` repeats a run until that happens, with `RECOMP_WATCH=0x80744000` to name the writer.

### Next actionable task
1. Catch the #43 table change under the memory watch, then fix the writer or stop the doorbell helper following a changed table.
2. #49: Chapter 1 card, quit to Chapter Select, Chapter 2 card in one session.
3. #47 (announcer on the results screen): plausibly the same voice race as #48; needs a level clear to check.
4. #46 flicker: capture frame by frame in combat.

### Night of 2026-10-17/18 (owner asleep; autonomous)
- **Flicker fixed (#46, verified by measurement, owner to confirm).** The white "fogged" frames and the dark frames came from the runtime's pushbuffer reader losing GPU commands: at the command ring's wrap it stopped at the highest write position seen so far instead of following D3D's own jump, and a command cut off by the write position lost the rest of its data (which was then executed as garbage). A missed "use this vertex program" command drew the whole scene with the screen-overlay program, hence the white fog. Two build-time patches (`pb_segment_carry`, `pb_segment_wrap`). Before: 6-10 bad frames per two minutes around the Town Hall; after: 0 in six runs.
- **Chapter card (#49):** the card picture is loaded at 0xEA4000, the same address as a glow surface used in every gameplay frame, so after playing a chapter the renderer showed the old surface. The renderer now notices when memory under an old surface has been replaced (`rt_replaced`). Needs a continuous Chapter 1 -> 2 session to confirm.
- New tools: `scripts/flash_hunt.ps1` (with `-NoCapture`), `scripts/video_flashes.py` (also on owner videos), `scripts/flash_log.py`, `scripts/drawdiff.py`, `scripts/load_save.ps1` (now waits for each menu's file load), renderer switches `OOGIE_FOG_TRACE`, `OOGIE_FLASH_MEASURE`, `OOGIE_VC_DUMP`.
- **Hang after the pause menu (#50, fix in the build):** the game wraps its semaphores with its own counters updated without a lock; with threads running truly in parallel the counter drifted and the logic thread waited forever (picture frozen, controller ignored). Both wrapper functions now keep their counters under a lock (`recomp_manual.c`).
- **Small speed-up:** the GPU command reader's statistics are off in normal play (`pb_note_off`): 82 -> 86 fps uncapped at the Town Hall; 60 fps held while fighting there.
- **Stability:** 8 soak runs with no crash after the pushbuffer fix; one USB-driver crash (#43) seen in 13 runs, analysed (a corrupted free list in the controller driver).

### Next actionable task
1. Owner playthrough: confirm the flicker is gone, the music in the Oogie fight, the chapter cards across chapters, and the results-screen announcer (#47).
2. #43: watch the USB driver's free list (0x4334EC) to catch who writes -1.
3. #50: a reliable reproduction to prove the fix (the menu stress pattern did not hang either way).
4. Frame rate in heavy fights: the GPU thread spends ~10% fetching vertex attributes on the CPU for GPU vertex programs and ~20% in the D3D11 driver (3,600 draws a frame).

## Session 2, sprint 7: 2026-10-18 (owner away all day; PC port features)

### Done
- **Controllers (owner request):** `port/src/pc_input.c` replaces the runtime's `xbox_InputGetState` (build patch `pad_wrapper`). Xbox 360/One/Series pads are read through XInput, as before. PS4, PS5, Switch Pro and other pads are read through SDL2 2.30.9 (`bin\SDL2.dll`, loaded at run time; `scripts\get_sdl2.ps1`). All pads are hot-plugged and polled on their own thread. The game's rumble is forwarded (patch `pad_rumble`). Scripted pad and keyboard Verified (in1, in2). Real pads not yet tried: none is connected here.
- **settings.ini (owner's decision, `docs/PC_FEATURES.md`):** keyboard and pad rebinding, window size (`auto`), fullscreen, 4:3 aspect, fps cap, rumble, light bar. Written with comments on first start. Tests use `logs\settings_test.ini`.
- **Display:** the swap chain follows the window, the picture is letterboxed 4:3 (it was stretched in the owner's video), F11 or Alt+Enter toggles borderless fullscreen, the process is DPI aware (sharp on a 200% screen), and the window opens at the largest whole scale that fits (ws1-ws3).
- **#51 results-screen flicker:** analysed from the owner's video. Fix in the build: present only the D3D device's own frame buffers. Owner to confirm after a chapter.
- **#52 Load Game:** the save-folder split (fixed last night); Load Game held steady in lg1.
- **FPS drops:** no periodic hitch while standing (hz1, 240 s). The `[HITCH]` lines in `logs\play.log` will show what the owner's drops are.

### Next actionable task
1. Owner: try the Bluetooth pad and a PlayStation pad; `logs\play.log` lists `[INPUT]` lines.
2. Owner: finish a chapter and look at the results screen (#51) and the next chapter card (#49).
3. Overlay settings menu (step 2 of the owner's decision), after the playthrough blockers.
4. Higher internal resolution and widescreen (roadmap).

## Session 2, sprint 8: 2026-10-18 afternoon (settings menu, widescreen, render resolution)

Owner request: a RECOMP SETTINGS menu like the BLACK recomp's, Oogie-themed, controlling every PC feature (docs/PC_FEATURES.md).

### Done
- **In-game settings menu** (`port/src/pc_menu.c`): F1, Back+Start or Guide/PS opens it, the game pauses, Oogie-styled (Chapter Select palette, Mountains of Christmas font, moon, framed panel). Pages: Display, Graphics, Audio, Controls, Keyboard, Controller, About. Changes apply live and save to settings.ini (comments kept, `pc_settings_set`). Verified mn1.
- **16:9 widescreen**: the game's camera (`sub_001E4E86`, aspect 4/3) is widened by an entry hook; 2D overlay squeezed back to shape; menus and movies stay 4:3. Verified mn1, hud5 (KNOWN_ISSUES #53 for the HUD placement).
- **Render resolution** 100-400%: targets and depth buffers allocated at a multiple, viewport scaled. Verified rs1 (300%, 60 fps).
- **Vsync, frame cap, anisotropic filtering, frame-rate counter, master volume (patch `audio_volume`), mute in background, rumble strength, monitor choice, window size presets, 16:9 window shape.**
- **Field of view** 70-140% (fov1, Verified).
- Texture probe diagnostic (`logs	exprobe.flag`).
- Chapter 2 start recorded frame by frame (c2a): no flicker in this flow.

### Next actionable task
1. Owner playthrough with a controller using the menu; collect problems.
2. HUD anchored to the screen edges in 16:9 (KNOWN_ISSUES #53).
3. Anti-aliasing / sharpening options; high frame rate (find the game's timestep).
4. Results screen (#51) and chapter card (#49) confirmations.

## Session 2, sprint 9: 2026-10-18/19 night (owner's flashing menus, black bars)

### Done
- **#51 menu flashing, root cause found and fixed (Verified):** D3D deferred its flip into the next frame in some sessions (buffer rotation one step out of phase with D3D's back buffer), so the screen showed a buffer holding the next frame's clear and first quad. Swap now uses the immediate interval (the renderer paces to 60 Hz anyway). 12 slowed sessions with the inventory opened 3 times each: none flashed. Also: surface width from the pitch (zoom clip sizes recreated the screen buffers), and a copy-aside of a finished frame cleared before its flip.
- **Black bars:** Widescreen (Fill Screen) uses the window's or monitor's real shape (16:10 here); the game's own black edge bands and film bars can be hidden (on by default); 4:3 menus and films get blurred sides (option: black or stretch).
- **Frame cap:** "Off" removed (films and music ran fast). Render-resolution help text warns that slow frames slow the game (400% + 16x ran Chapter 2 at about 45 fps on the owner's laptop, likely the music stutter).
- Diagnostics: OOGIE_FRAME_LOG (per-flip draw/clear sequence), method ring dump, OOGIE_TEST_SLOW, scripts/inv_cycle.ps1, scripts/inv_hunt.ps1, scripts/movie_timing.ps1.

### Next actionable task
1. Owner: replay the inventory, results screen and main menu; try Widescreen (Fill Screen) in fullscreen; report whether the taskbar stays on top with F11.
2. Music stutter at high render resolution: confirm with the owner at 200%.
3. HUD at the screen edges in widescreen (KNOWN_ISSUES #53).

## Session 2, sprint 10: 2026-10-19 morning (owner's crash reports, hitches)

### Done
- #43 USB crash: the emulated controller no longer takes a TD whose link is not a descriptor, stops at a skipped ED, and never writes a garbage queue head back (patches `ohci_td_validate`, `ohci_head_writeback_check`).
- #56 DSOUND crash found by a 25 min soak (sk43a, 18 min): freed contiguous memory now waits 3 s before reuse unless memory is short (patches `contig_quarantine_a`..`_e`). sk43b: 25 min, 11 Continues, no crash.
- #54 the 09:55 "crash" was a hang (Windows AppHangB1 in the title film). New stall sampler `port/src/pc_stall.c`: `[STALL]` call stacks by function name when no frame for 200 ms, all threads at 2/6/15 s.
- #55 hitches: one log line and flush per disc read held the game thread (st3); off unless OOGIE_READ_LOG=1 (patch `kernel_read_quiet`). The frame counter shows the slowest frame of the last 2 s; closing the settings menu no longer counts as a hitch.
- #57 flicker: not reproduced with the owner's settings; `[FLICKER]` detector always on.
- Scripts: `load_flicker.ps1`; `-Owner` on `inv_hunt.ps1`; `OOGIE_TEST_SETTINGS` in `live_start.ps1`.

### Next actionable task
- Read the owner's next play.log: `[STALL]` (where hitches and hangs sit), `[FLICKER]` (which menu, caught or broken).
- More soak time on #43/#56 (a shop and Jack's house route), and a run with OOGIE_CONTIG_QUARANTINE_MS=0 to confirm the quarantine is what helped.

## Session 2, sprint 11: 2026-10-19 afternoon (owner's 11:56 log and videos)

### Done
- #58 figure-viewer hang: D3D's "GPU far behind" event wait spun forever; the wait now ends when the GPU has run what was submitted (patch `ke_wait_d3d_gpu`).
- #54 fence-wait freeze: safety net `oogie_gpu_unstick` (kickoff or write the fence once the GPU is idle for 1 s), proven by fault injection.
- #55 lag: log through a pipe and a writer thread (`pc_log.c`), no per-read log, no per-frame file check, DirectSound voice-stop poll lets the APU interrupt in (sub_0020B669). Soak: 0 hitches in 10 min (was 179-251).
- #57 flicker: the #51 copy-before-clear workaround is off (it showed half frames 4000+ times for the owner); clear rectangles are honoured; near-empty frames in heavy scenes are held back.
- Diagnostics: [STALL] lists every game thread at 200 ms; [CS] names lock holders; [FLICKER] in every log; OOGIE_RING_AT_FLIP; OOGIE_CLEAR_LOG.
- Scripts: boot_loop.ps1, load_transition.ps1, frames from the owner's iPhone videos (scratch, OpenCV).

### Next actionable task
- Owner plays the new build; read play.log for [FLICKER], [STALL], [GPU], [CS] lines.
- #59 loading-screen banner blink; #60 door transition capture (needs a door route).

## Session 2, sprint 12: 2026-10-19 evening (stability bisect)

### Done
- Found by bisect (bsB/bsC/bsD) that the pipe log writer, by removing the log lock that serialized the game's threads, exposes a render-list race (#61, freezes of minutes). Pipe log now opt-in (OOGIE_FAST_LOG=1); default build stable in bsD.
- GPU safety nets only act in D3D's normal state; quick kickoff for fences whose commands were never handed over; KickOff ack thread (bit 16 of 0x100410 only).
- USB: ED head 0xFFFFFFF0 repaired to the empty queue (`ohci_head_repair`).
- Tried and dropped (switches left in for experiments): one processor for game threads (OOGIE_GUEST_CORES), the render-list check (OOGIE_RLIST_CHECK=1), a C replacement of the walk.
- Scripts: soak_report.ps1, bisect_soak.ps1.

### Next actionable task
- #61: make the main thread / render task hand-off behave as on one CPU, then turn the pipe log back on (it removed most hitches: 179 -> 12 in 10 min).

## Session 2, sprint 13: night of 2026-10-19/20 (owner asleep; Ch3 reports)

### Done
- Pumpkin bombs (#65): full regen with the playtest session's ICALL seeds (0x9F0F0, 0x9FB40 now generated).
- Ch3 1-2 fps (#64): DirectSound's DPC may finish a voice stop while a game thread polls (IRQL >= 2 and not a game thread).
- Cross-machine (docs/COMPATIBILITY.md): feature levels 11.0/10.1/10.0, WARP fallback (verified warp1), OOGIE_D3D_DEBUG debug layer with a fallback (Graphics Tools not installed: the elevation prompt was not answered), GPU/driver line in every log, the GPU vertex-program path off below 11.0.
- Palettised textures decode with their CLUT (#67); clear rectangles (#57).
- Command-ring desync found and characterised (#66): the fence-writing safety net removed (it caused replays), DMA walk made the default; instrumentation for releases, CALLs, carries, ring history.
- USB (#43): frame number and SOF after the list walk. Rarer since, not gone (fin1 crashed at 34 s).
- Guest CPU lock made usable by port code (#61), not on by default yet.

### Next actionable task
- #61: lock mode now passes three earlier boot hangs; the remaining one is a lost wake-up between the main task poll (sub_000583C0) and the loader at sub_0022F210 / thread 0x0022F360 (logs/gz1.err.log). Then soak with the lock on; if clean, it should end the D3D write race and allow the fast log (stutter 170 -> ~10 per 8 min).
- #66: the lost fence after a resync (mo1).
- #63 subtitles: a save just before the dt07 cutscene.
- Owner: answer the Graphics Tools elevation prompt (or run scripts\install_graphics_tools.ps1 as administrator) for the debug-layer check.
- fz1/fz2 (default build): 7 min each, no crash, no stall, no desync.
- fin1 (final build, default): USB crash at 34 s (#43 still open).

## Session 2, sprint 14: 2026-10-20 midday (owner's title-screen freeze)
- Owner's play.log: froze on the title screen (lost GPU fence after a walk resync, #66). Safety net: when every command is consumed and the fence is still not reached, the semaphore is set (300 ms / 2 s).
- USB crash (#43): DISPATCH_LEVEL lock between XPP's raised sections and its DPC (patches usb_dpc_lock_a/_b). 9 runs since, no crash.
- Next: what puts the DMA walk out of step (a longer header history, PB_HIST); the GIL lost wake-up (#61).

## Session 2, sprint 15: 2026-10-20 afternoon (owner's 13:50 session)
### Kept (on by default)
- Voice stops: forced after 50 ms (OOGIE_DS_STOP_MS), no more 0.5 s freezes (#55).
- XACT's raised sections never entered without the lock (the 13:50 XACT crash, #55).
- USB DPC lock (#43), lost-fence net (#66), 64-entry PB history.
### Built but opt-in (made soaks worse)
- OOGIE_PB_RING_CHECK=1, OOGIE_SUSPEND_NEW=1, OOGIE_SCHED_YIELD_MS=30.
### Lock mode (RECOMP_GUEST_LOCK=1)
- Fair ticket lock and wait tracing in; boots further; still stops in Bink's worker without OOGIE_SUSPEND_NEW.
### Next
- Lock mode with OOGIE_SUSPEND_NEW=1 (both are needed together; untested as a pair).

## Session 2, sprint 16: night of 2026-10-20/21 (owner asleep; improvement plan)
See docs/IMPROVEMENT_PLAN.md.
### Done
- Pause/inventory menu flicker: the back end shows the buffer each frame finished in (patch flip_done; #57).
- Bink worker created in a safe order (#42).
- play.bat: saves backed up to saves_backup\<date_time> before every session (ten kept); a session that crashed or froze leaves a report in reports\<date_time> (log + settings).
- Render Resolution "Auto" (render_scale = 0, default for new settings files): the largest the window holds, at most 2x on Intel graphics, lowered when 3D frames run slow.
- #66 diagnostics: every CALL the walk takes is logged with a re-read of the stream and the 24 headers before it.
- D3D entry lock (#66): 69 D3D entry points wrapped so only one game thread is inside D3D at a time; contended about 8 times per 15 min (so the overlap was real), never timed out.
- Test routes read scaled frame dumps, so soaks run at the owner's settings (fullscreen, 3x).
- Final soaks (fx1, fx2 owner settings; fy1): 3 x 15 min, 9 Continues each, all clean.

### Next actionable task
- Owner: play a normal session with the new build (menus, doors, Ch3); `reports\` will hold anything that goes wrong.
- #49 chapter card in one session, #63 barcode subtitles (save before dt07), #60 door transition shapes, #47 announcer.
- Lock mode's task stall (only if new races show up; the D3D lock covers the one that mattered).

## Session 2, sprint 17: 2026-10-22 (owner's 11:25 freeze)
- **Verified:** the 10-21 build froze the owner's game at the first frame-order cap: a diagnostic stack dump suspended the main thread itself. Fixed in `pc_stall.c` (no self-suspend) and the cap no longer dumps stacks (KNOWN_ISSUES #57).
- A test script closed the owner's (already frozen) game. Test scripts now stop only games they started (`logs/test_pids.txt`, `scripts/stop_test_game.ps1`) and refuse to start while another game runs (`scripts/owner_game_check.ps1`). Verified sk0.
- D3D11 debug layer run (dbg1): no errors or warnings.
- Next: owner retests the trophy shelf and menus with the fixed build; a save just before a chapter end for the announcer (#47).
- **Verified (afternoon):** cause of the remaining menu flicker: the game's semaphore waiter count goes wrong when a new menu task thread is released before it first waits; the main thread then shows the menu frame half drawn. Fixed in `sub_001AA3E0`/`sub_001AA460` (KNOWN_ISSUES #57). Forced-race tests: 41 and 29 flicker lines before, 1 and 0 after.
- Slowdowns near Jack's house (owner 12:21): every stop of five DirectSound voices waited the full 50 ms before being forced (#55). A voice that needed a forced stop once is now forced after 2 ms. Not yet checked in that area (owner).
- **Verified (owner, 13:30):** menu flicker gone in play ("you fixed it"); slowdowns near Jack's house reduced.
- Dev cycle 3 started (docs/IMPROVEMENT_PLAN.md). Item 1 done: Render Resolution in real sizes (480p, 720p, 900p, 960p, 1080p, 1200p, 1440p, 1800p, 1920p, 4K) and Native (the screen's height); the renderer now takes any scale, not only whole multiples. Verified rs1080, rsnat2 (60 fps at 2400x1800 on the Arc 140V).
- Dev cycle 3 items 2-3 done: Anti-Aliasing (Off/Low/High, edge blend on the finished frame) and Sharpening (Off/Low/Medium/High, contrast-adaptive), both in F1 > Graphics, defaults High and Low; run before scaling to the window so the F1 menu stays crisp. Verified pp_split.
- Dev cycle 3 item 4: streamed reads on one worker thread (sub_001A8410/sub_001A83A0; the game made a thread per read, 981 in the owner's 13:30 session). Soak swold/swnew: read threads 181 -> 1, hitches 191 -> 134, long frames 100 -> 84, both 4 Continues.
- Item 8: F12 screenshots (pc_shot.c, PNG in screenshots\) and HUD at Screen Edges (widescreen corner pieces anchored to the screen sides), both built, visual check pending.
- Item 9: first-run setup (pc_setup.c): finds game\ next to the exe or one folder up, else asks for the player's disc image, checks default.xbe's SHA-1, copies with a progress window. Verified with --extract-iso: 722 entries, 3.5 GB in 8 s, identical to game\ (sizes all, SHA-1 of default.xbe, mv_04.bik, 0000.lz); a non-Xbox image is refused and nothing is written.
- 2026-10-23 night: #68 (10 GB play.log from a wrapped log counter; the task runner's poll spinning on the game mutex ~500 000 times a second) fixed in the build, waiting for a test slot (a game not started by the tests was running at 00:20).

## Dev cycle 4 (2026-10-23, owner: "thread scheduler fix first, then mods folder and the disc-art menu, then the rest")
- Rival port reviewed (github.com/josiahg2502-glitch/OogiesRevengeRecompiled, GPL-3.0): ideas only, no code. Its notes confirmed the frame ordering problem: the game builds each frame on a higher-priority thread the render thread must not interleave with.
- Scheduler (#69): lock mode with priorities and safepoints, 59.3 fps in fights (same as without the lock). Full checks running (lm6).
- Mods folder (patches mods_folder_a/b): mods\<name>iles\<disc path> replaces disc files; a file named disabled turns a mod off. Built, not yet tested with a mod.
- Menu art: .stm archive format read; .pig sprites are plain DDS (REVERSE_ENGINEERING.md).
- Scheduler done and on by default (lm7/nl7 parity, lm6 menu order kept with the #57 fix off). Mods folder Verified (mod2).
- F1 menu with the game's art (pc_menu.c art_load): reads frames, hook cursor, vines and button pictures from the player's game\stm_data\sbuty_u.stm at first open (own DXT5 decoder, own .stm reader), draws them in a full-colour layer; falls back to the drawn look without the file; OOGIE_MENU_ART=0 off. Built and checked as an offline mock-up (layout as the code draws it); not yet seen in the game (owner playing, no test runs).
- Night of 2026-10-23: Mods page in F1 (switch mods, open folder) and texture packs (dump and replace by content hash) Verified (modp1, tx1-tx3).
- Test switch OOGIE_START_CHAPTER=N (sub_00040950_enter): New Game starts chapter N; Verified chj4 (Chapter 4 at the Hanging Tree, 0100.lz). scripts/chapter_sweep.ps1 runs every chapter for -Minutes with A presses and Continue; overnight run sw (results in TEST_RESULTS).
- Release readiness (local only, nothing uploaded): LICENSE (MIT), THIRD-PARTY-NOTICES.md, new README, .gitignore (logs, _transfer, *.obj, *.pdb, dist, texture_dump), username removed from two scripts, scripts/package_release.ps1 (dist\ProjectOogie + zip). The program runs without play.bat (pc_setup_standalone: runtime switches, saves\, saves_backup\ ten kept, logs\play.log with two older kept) when RECOMP_PB_EXEC is unset. OOGIE_SETUP_FROM=<image> runs the first-run setup without dialogs (tests).
- Graphics safety: the high-performance GPU is asked for (IDXGIFactory6, plus NvOptimusEnablement / AmdPowerXpressRequestHighPerformance exports; OOGIE_GPU=powersave for the other); a lost graphics device (driver crash, update, GPU switch) is logged with its reason and the player is told, instead of a frozen black window.
- Audio Buffer setting (Audio page, default Large: 6 XAudio2 buffers, 64 ms) for the menu crackles (owner 10:37 log: 27 underruns while a save loaded).
- Morning of 2026-10-24: whole-game sweep done (24/24 chapters reach their level and play 3 min with no crash or broken frame); release zip tested in an empty folder (first-run setup from the disc image, standalone saves/logs, 60 fps). Open: #72 (Casino Clash slower under lock mode), #47 announcer (needs a save), #60 door transition. Nothing uploaded to GitHub; the GitHub CLI was not installed (owner stopped it).
- Night 2 (2026-10-24 02:30-06:00, owner asleep): long sweep (6 min per chapter, lsw); crash reports for standalone players; Skip Intro Logos; window place remembered. #72 profiling waits for a free test slot.
- Night 2 results: long sweep chapters 1-21 clean (6 min each); Skip Intro Logos, window place, crash reports Verified; release zip rebuilt (9.0 MB). Open: #72 Casino Clash (ch17 56.5 fps under lock), #71 menu frames during save load, #47, #60.

## Dev cycle 5: 2026-10-24 (owner away; owner logs play.log / play.1.log)
- Controls lockout after the settings menu fixed (KNOWN_ISSUES #73). COMBO overlap with HUD at Screen Edges fixed (#74).
- Slowdowns / audio (#75): owner PC is an Intel Arc 140V laptop at 1800 high. Auto resolution now steps down in half steps; [POWER] log line; Large audio buffer 8 x 512 (85 ms). Host CPU profile of ch17: plenty of headroom.
- Remote Control turned on for the session (owner request).
- Sweep sw9 (24 chapters x 3 min) with this build: see TEST_RESULTS.
- Next: owner play test with the new build (the [POWER] line tells whether the laptop throttled); #72, #71, #60.

## Dev cycle 6: 2026-10-24
- Fast texture path (13x less texture time, every decode identical to the reference) and Graphics > Mipmaps (default on). Sweep sw10: 24/24 chapters clean.
- Next cycle plan in IMPROVEMENT_PLAN.md (native .lz decompressor for loads first).

## Dev cycle 7: 2026-10-24 (loading pauses)
- Load pauses traced to the log, not the loading (#76): fast log on by default under lock mode. Sweep sw11: 23 of 24 chapters with no stall and no frame over 100 ms (sw10: about 20 each); underruns 23 -> 4. Native .lz unpacker not needed (it never showed in the profile).
- Casino Clash (#72) fixed: deaf DirectSound voices stopped at once with a precise timer, and the voice-parameter writer no longer traps. Sweep sw12: 60.0 fps in all 24 chapters, 0 crashes; soaks dss1-2 clean. Next: graphics presets, then accessibility (IMPROVEMENT_PLAN.md).
- Dev cycle 8: Graphics > Quality Preset (Low / Medium / High / Ultra) and battery-aware Auto resolution. Verified in the menu (pr1).
- Dev cycle 9 (accessibility): Auto-Repeat Held Button (Controls) and Colour-Blind Filter (Graphics). Subtitle size and invincibility need the subtitle drawing and the health value found first.

## 0.2 release candidate: 2026-10-28
- New since 0.1: Proton startup fixes (#81), version number (title, F1 About, log), system details at the top of the log, widescreen door transition (#60), D3D vblank and KickOff busy-waits removed.
- Verified: 24/24 chapters, two 15-minute combat soaks, the zip under Wine 10 (TEST_RESULTS v02, v02s1-2, v02zip).
- Packaged: `dist/ProjectOogie-v0.2.zip`. Release notes: docs/RELEASE_NOTES_0.2.md. Drafts for GitHub #1 and Reddit: trailer/v02_replies.md (not in git).
- Not done: commit, push, GitHub release, posts (waiting for the owner). Owner play test of 0.2 skipped by the owner's choice.

## Dev cycle 8: 2026-10-28/29 (sound dropouts, GitHub #1 and owner)

- Owner's Chapter 1 play.log and two videos: Mr. Hyde's middle lines and the results announcer silent, no Soul Robber sounds against Shadow Oogie.
- Fixed (#84): voice stops were forced after a 16-50 ms silence because the APU interrupt was held off while DirectSound waited for a stop. Verified in dv6, fx2-fx9: 0 forced stops, 0 crashes. Not yet checked in the Shadow Oogie fight itself.
- Fixed (#47): short streamed voice lines (under about 1.4 s) never started, because XACT left their only packet unsubmitted when the source ran dry before Play. Verified on Mr. Hyde's two silent lines (fz1: now 1.2 s each with a voice). The results announcer is expected to use the same path, not yet checked. The stream-slot theory is ruled out.
- Full-game check after both sound fixes (sv sweep): 24/24 chapters, no crashes or stalls.
- GitHub #2 (#85): deflecting a projectile crashed the game. Cause: five jump-table switches in the hit/projectile code had case targets never translated (19 in all, found by the new scripts/jt_scan.py). All seeded; the scan now finds none. A real deflect is not yet tested in play (df1/df2 didn't trigger one).
- New diagnostics, off by default: OOGIE_DSV_TRACE, OOGIE_DS_TRACE, [XSN] lines under OOGIE_XACT_TRACE, OOGIE_SELFTEST_JT.
- Test harness: scripts/step.ps1 (one press plus screenshot), fight_to_hyde.ps1 -Dsv -NoStop.
- Nothing committed.

