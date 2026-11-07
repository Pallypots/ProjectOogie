# High frame rate: findings and plan (2026-11-01)

Status (2026-11-07): published as a test only, "0.3.0 High Frame Rate test 1", from the branch
high-frame-rate, for the owner to try on a high refresh screen. It goes into a main release only after the
owner confirms it. F1 > Display > High Frame Rate (Test), settings.ini high_frame_rate (OOGIE_INTERP).

## Why the cap cannot simply be raised

**Verified:** the game moves one fixed step per frame. The Chapter 1 opening takes about 2,380 frames at any
frame rate (KNOWN_ISSUES #37); uncapped it ran at 200+ fps and everything ran fast (#34); at about 120 loops a
second films played at double speed (#90). The game's own code reads no real-time clock. Only five routines
pass a 1/60 s value (sub_00178B00, sub_0018CE50, sub_0018E5A0, sub_0018EBF0, sub_00194780; **Hypothesis:**
local effect timers, not read yet).

## The established pattern (other ports, source read 2026-11-01)

- Engines that already measured real time per frame are uncapped by loosening their time step: Project Cane
  (Sly Cooper, `clock.cpp` clamps dt between 1/target and 1/30), Perfect Dark (`lvupdate240`, 1/240 s ticks),
  Unleashed Recompiled (time step followed, high-frame-rate glitches fixed one by one, some remain).
- Fixed-step engines keep their logic rate and interpolate extra frames in the renderer: Ship of Harkinian and
  2 Ship 2 Harkinian (`frame_interpolation.cpp`: record transforms of two real frames, match them, blend), and
  the N64 recompilations through RT64 (Zelda64Recomp, Banjo: Recompiled: "Changing framerate has no effect on
  gameplay"). Oogie's Revenge is in this group.

## What the probes measured (OOGIE_INTERP_PROBE=1, d3d11_backend.c; TEST_RESULTS ip, ip2, ip3)

- 1,900-3,800 3D draws per frame in play.
- **Draws can be paired between frames:** named by vertex program, stage-0 texture, vertex source address
  (patch backend_gpu_vsh passes it as `src_addr`, 0 for inline data) and counts, and numbered in draw order,
  99.8-100% of draws are found again in the next frame (ch 1, 3, 16).
- **What changes:** constants (two 4x4 matrices at c0-c3 and c8-c11 for most draws; 17-64 registers from c79
  up, bone matrices, for characters) and, for 59-88% of draws, the vertex positions themselves (attribute 0);
  colours, texture coordinates and the other attributes almost never change. 73-89% of draws send their vertices
  inline each frame. So the game computes many positions on the CPU.

## Prototype so far (local, OOGIE_INTERP; TEST_RESULTS ipA-ipH)

- Works: frames recorded at draw time, replayed at the flip with matched draws' constants and positions
  blended halfway; 99.7-100% of vertex-program draws matched; an in-between picture every flip; no crashes.
- Too slow: the replay costs 6-10 ms a frame on this PC (levels drop from 54-60 to 26-44 fps, so the game runs
  slow). Reusing the live draws' prepared state cut recording to about 1 ms but not the replay: re-issuing
  ~3,500 draws means ~7,000 small constant uploads and as many ring writes, the same CPU work as the live frame.
- GPU replay (built 2026-11-01, TEST_RESULTS ipI-ipN): each frame goes to the GPU in two uploads at its flip
  (its data block, read as t8/t9 and as the index buffer; one constant buffer with the frame's distinct constant
  sets and a block per draw), draws bind them by offset (D3D 11.1 constant buffer offsets; without them the old
  replay runs), and a replay variant of each vertex program blends the previous frame's constants (b2) and
  positions (t9) on the GPU. **Verified:** replay 7.6-10.4 ms -> 1.0-1.2 ms a flip; recorded data 14 -> 3.5 MB
  a frame; whole in-between cost about 4 ms (record 0.9, match 1.5, upload 0.9, replay 1.1).
- What still blocks it: **Verified** on the test laptop (Intel Arc 140V, 60 Hz), the game's own frame already
  takes 18-24 ms with recording alone (ipM), so the ~4 ms of in-between work on the same thread lowers the frame
  rate (37-43 fps against 56 without). Skipping the in-between draws only reached 48 fps (ipL): the processor
  side, not the GPU, is the limit here.
- Next step: move matching, upload and replay off the game's thread (a second thread building the in-between
  picture into a D3D11 deferred command list while the game computes its next frame; the render thread then
  only executes it and presents). Recording (0.9 ms) stays. Then measure again, and check the pictures by eye
  on a 120 Hz or faster screen, which this PC does not have.

## Progress 2026-11-02 (TEST_RESULTS ipO-ipU)

- **Pacing by the screen, Verified (ipP):** when the screen runs at twice the cap, each picture is presented for
  one refresh and Present's own wait keeps the game at the cap; the half-frame wait on the render thread is gone.
  The old swap chain blocked each Present until its refresh (ipO: 17-29 fps at a cap of 30), so with
  OOGIE_INTERP set the window uses the flip model (3 buffers). Cap 30 on this 60 Hz screen stands in for 60 on
  120 Hz: 29.6 fps, 60 pictures a second, an in-between picture every frame.
- **Pictures, Verified (ipQ, ipS2):** saved in-between pictures are clean; Jack's run shows his legs and arms
  halfway through the stride. Not yet seen on a 120 Hz or faster screen.
- **Matching while recording, Verified (ipT):** each draw finds its partner as it is recorded; the in-between
  picture now costs about 3 ms a frame. At the 60 cap on this laptop: 47.7 fps against 57.5 off (ipU).

## Next

1. A look at it on a 120 Hz (or 240 Hz) screen: smoothness, and anything that jumps or smears.
2. Screens that are not a whole multiple of 60 (144, 165 Hz): a presentation clock of its own, the blend factor
   from the time each picture will be shown (passed per picture instead of fixed at 0.5).
3. Cost on slow PCs: the upload and replay still run on the render thread (about 2 ms); a deferred context on a
   second thread, if the driver supports command lists, or skip interpolation when the frame runs late.
4. Whole-game sweep with it on, then the owner's decision about a release.

## Plan

1. Prototype, local only: keep the last two real frames' draws; between them draw extra frames with matched
   draws' vertex positions and constants blended (positions linearly, matrices by components as RT64 does).
   Unmatched draws, the 2D HUD and films are drawn from the newer frame unchanged. Logic stays at 60.
2. Camera cuts and spawns: a frame where few draws match is shown without blending.
3. Measure the cost (keeping two frames' vertices, drawing the scene more than once per game frame) and look
   at it at 120 and 144 Hz.
4. Whole-game sweep at each rate, owner test, then decide about a release.

## Test 1 (2026-11-07, TEST_RESULTS hfoff, hfon, hb3, hc*, hg*)
- New: the settings row; pacing for 240 Hz (two refreshes a picture), never run.
- **Fixed, Verified (hb3 -> hc3):** every other picture had a flat fog-coloured tint. The GPU replay gave the
  pixel constants a 512-byte window, but ShaderConsts is 528 bytes since the PS2 texture packs (tas), so the
  replay read tas as 0 and every sampled alpha was 0. The window is now 768 bytes. After: consecutive pictures
  equal in mean colour in chapters 1, 3, 16 and 17. The CPU replay (OOGIE_INTERP_GPU=0) never had it.
- The flicker detector counted the replay's own clear as a broken frame (200 reports a run); it is off while
  this feature is on.
- Laptop (60 Hz): with it on 38-60 fps (6 chapters, 1 min each), no crashes; off 54-60.
