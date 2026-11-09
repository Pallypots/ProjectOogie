# High frame rate: findings and plan (2026-11-01)

Status (2026-11-08 night): "0.3.0 High Frame Rate test 4" on the branch high-frame-rate, for the owner to try
on the 165 Hz desktop. Test 3 slowed the game down above 60 there (see Test 4). Goes into a main release only
after the owner confirms it.

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

## Test 2 (2026-11-07 night; TEST_RESULTS h2*, sw*, md_*, rs1)

Owner's test 1 report (RTX 4090, 165 Hz, 4x render, PS2 HD pack): almost every texture flickering, see-through
characters including Jack, black lines from Jack, textures shooting out; and the cap plus "High Frame Rate
(Test)" settings were confusing.

- **Root cause, Verified (h2t1only, h2gpu0, h2guard0, sw*):** the text builder `sadd` dropped any piece longer
  than the space left in its buffer (it only kept 512 bytes free). The replay variant's 600-character header
  was dropped for vertex programs 6 and 7 (`replay variant of vertex program 6 failed: ... unexpected end of
  file`, also in the owner's play.log), and `ipg_replay` skips draws whose variant failed, so those models were
  missing from every in-between picture: Jack see-through (Chapter 1, test 1's exe) and gone (Chapter 12).
  The CPU replay (OOGIE_INTERP_GPU=0) drew him. `sadd` now grows and writes the piece again. No live shader
  ever hit it (no other compile error in the owner's or 60 test logs).
- Screenshot bursts mislead: each BMP write stalls the game, and the pictures after the first one were not
  representative (Jack present in pictures 2-5, missing in 1). Checks now save only the first picture.
- **Also fixed:** the replay used one rasterizer state with no culling (`s_rs`); each record now keeps the live
  draw's (`rs_for`). A pairing check skips blending a draw whose colours/texture coordinates differ from its
  partner at 8 sampled vertices. A bone-matrix check refused 45% of chapter 1's pairs (effect constants) and is
  off (OOGIE_INTERP_BONES=1).
- **Pacing by time (any refresh rate):** real frames sit on the 1/60 s grid; each in-between picture is placed at
  (t - S) x 60 from the time it is presented (b4 `ipa`, one GPU upload per frame for any number of pictures).
  Pictures follow the refresh (flip-model swap chain, frame latency 1, waitable object `swap_wait`) or the
  Frame Rate (timer); tearing allowed with vsync off. While the game computes its next frame (5.6-12 ms on the
  laptop, ~7.8 ms on the owner's PC) no picture can be made.
- **Measured (md_*, Chapter 17, laptop 60 Hz):** 60: 60/60. 30: 30 pictures, game 60 (every other frame shown;
  the old 30 was half speed). 120 vsync off: 120 pictures, game 60. 165 vsync off: ~158. Unlimited vsync off:
  ~250. 120 with vsync on at 60 Hz: in-between pictures off, 60. Resizing and maximising (rs1): no errors.
- Not verified: a real high refresh screen, live switching in the menu, the PS2 HD pack with this build.
- Next: if the hold while the game computes shows as judder at 144-240 Hz, make the in-between pictures ahead
  of time and present them during the next frame's work (a presenter thread, or presents from the executor's
  idle time).

## Test 3 (2026-11-08; TEST_RESULTS k4*, ka*, kb*, kc*, kd*, kg5, kr5, kfin*, kl_*)

Owner's test 2 report (RTX 4090, 165 Hz, render 4.5x, PS2 HD pack, play.log of the session): at 30 and 60
fine; above 60 flicker, see-through walls and Jack, black lines down from Jack. The log showed the pacing
working (161-163 pictures a second at 165 Hz, game 60). Test 2 had been checked on still scenes; the faults
only show while things move.

- **Measure first (Verified):** OOGIE_INTERP_ALPHA=1 (pictures placed on the real frame) gave pictures equal
  to the real frame pixel for pixel (ka1: 0 pixels over 24 of 1,228,800), so the replay itself was right and
  the blending was wrong. ALPHA=0 (on the frame before) should equal the frame before: test 2's code missed by
  ~8,900 pixels in chapter 1 (ka0), a pale band from the skeleton's shadow. This "a=0 test" is the score used
  since. OOGIE_INTERP_FORCE=1 makes one picture a flip even on a slow PC (the laptop at 4.5x).
- **Cause 1, shadows (Verified, kd4 dump):** the shadows are ~560 inline quads a frame (CPU-written, no vertex
  address, position only), each shadow 5-19 quads drawn three times (stencil mark, shade, unmark). Pairing by
  name and count shifted every later quad when one shadow had a quad more or less, and the three passes then
  disagreed and left stencil behind: pale bands, black streaks, shade over walls (owner's photos).
  Pairing quad by quad by place: 1,790 pixels (kb0). Pairing whole runs (same stencil/colour/cull state,
  same quad count, near): 37 pixels (kc0), but half-way pictures still had streaks in the chapter 1 fight
  (kr5): the game rebuilds each shadow volume every frame and its vertices slide along the outline, so
  vertex-by-vertex blending opens cracks. **Now** (`ip_regroup`, rigid): each run is drawn with this frame's
  own shape moved back by the way its centre moved since the frame before (nearest run of the same state
  within 24 places, any quad count, within half its size; shifted copy in the frame's data block, replay
  shader takes it from t8 when ipp.w is set). Volumes stay closed. kg5 and every chapter checked: no streaks.
  97-99.5% of runs move, the rest (new shadows) are drawn as this frame has them.
- **Cause 2, trails (Verified, kb5 vs koff):** the game's motion-trail effect ping-pongs off-screen targets
  (00B22000/00B32000, 00B42000/00B52000) from frame to frame; test 2 restored only the shown picture after the
  in-between pictures, so they were baked into the trail and the next real frames smeared. Every target the
  replay draws into is now saved and put back (`ip_save_targets`, up to 24).
- **Cause 3, skipped frames (Verified, kfin5):** objects written one after another into a shared vertex buffer
  change first index when one appears or goes, so their names were new and 100-180 draws a frame went
  unmatched; whole frames were judged a new scene and lost their in-between pictures (judder). A second pass
  pairs them by program, texture, counts, closest constants and same model-space place (`ip_rematch`). A frame
  counts as a new scene below 30% matched (cuts: 0-3%; effects new each frame took play to 40-80%), judged on
  draws with a vertex address (inline runs may be drawn as is on purpose; menus are all inline and are judged
  on them).
- Also: repeated draws of one name are paired by closest constants (`ip_pick`; it almost never differed from
  draw order in chapter 1). New log fields in the [INTERP] line: repeated draws, inline runs moved/drawn as
  is, found again by shape. `[INTERP] f=N not blended` names frames judged a new scene.
- **Scores (laptop, Intel Arc 140V, 2x, 60 Hz):** a=0 test chapters 1, 3, 7, 10, 16: 553, 0, 0, 0, 0 pixels
  (kc0, kb0). Half-way pictures (ALPHA=0.5) looked at in chapters 1 (walk and fight), 3, 5, 7, 10, 13, 16
  and the later chapters (kl_*): no bands, streaks or see-through walls. Matching cost ~0.5 ms a flip.
- Not verified: the owner's 165 Hz screen, 4.5x on a fast GPU, the PS2 HD pack with this build.

## Test 4 (2026-11-08 night; TEST_RESULTS kv4*)

Owner's test 3 report (RTX 4090, i9-13900K, 165 Hz, vsync on, PS2 HD pack, chapter 1 from Chapters; log kept as
logs/owner/play_20261108_hfr_test3.log): slower than 60 above 60, everything flickering, black lines from Jack.

- **Slowdown (Verified):** the log's [INTERP] line had game 14-21 ms a flip and 0-2 pictures in chapter 1 (game
  40-50 fps), ~1,900 draws a frame with ~1,800 repeats of one name (laptop chapter 1: ~320). Six [STALL]s of
  0.35-0.44 s had the render thread at sub_00232C40_enter+CCC6..CCFB; resolved by disassembling the shipped exe
  (RVA 0x212A0-0x212FA): the 192x4 loop of `ip_cdist`, called from `ip_pick` (48 candidates of 768 numbers for
  every repeated draw). Reproduced on the laptop in chapter 13 with OOGIE_INTERP_PICKOLD=1 (test 3's search):
  pairing 12 ms a frame (worst 22.6), game 24 ms; test 4: 0.85 ms (worst 1-4.6), game 14 ms (laptop GPU)
  (kv4o_c13, kv4n_c13). Now: SSE distance, draw-order candidate first, 8 candidates, a budget of 4,096
  comparisons a frame (s_ip_pick_left; over it draw order), bounded chain walk. The run ordinals in ip_regroup
  are sorted instead of scanned back (was up to 4,096 steps a run).
- **Rematch stalls (Verified by stress):** in the owner's log every 120 frames ~90% of a frame's draws found
  nothing by name (e.g. f=5160: 205 of 1929), not seen on the laptop. ip_rematch then compared every unmatched
  copy with every unclaimed copy (quadratic). Now bounded (claimed draws leave the bucket head, 8 candidates,
  64 steps, its own 2,048 share of the budget). OOGIE_INTERP_SCRAMBLE=120 (every 120th frame new names):
  0 frames lost their pictures, found again by shape ~2,300 a 2 s, match worst 1.9-3.7 ms (kv4s_c13). A new log
  line names what changed when more than 200 draws are found again by shape (cause still **Hypothesis**:
  something in the owner's setup, PS2 HD pack or timing, changes vertex address/first index every 2 s).
- **Unpaired draws used this frame's camera (Verified):** new draws, draws refused by the attribute check and
  shadow runs drawn as is were drawn with this frame's constants while the rest used blended ones, so in each
  in-between picture they sat one frame of camera motion ahead (owner's log: 20-70 a frame, laptop 2-3).
  `ip_camera_fill`: per vertex program, the registers every matched draw shares (equal in all of them, this
  frame and the frame before: camera, projection, lights) take the frame before's value in such a draw when it
  holds the same value; its own object registers stay. Refused pairs now blend constants and keep their own
  positions. a=0 test (OOGIE_INTERP_ALPHA=0, Jack running) camera fix off/on: chapter 3 worst 2,464 -> 40
  pixels, chapter 13 worst 6,335 -> 3,471 (the rest: tower windows, a pixel-shader glow, not blended),
  chapter 1 median 1,480 -> 1,003 (kv4c0_*, kv4c1_*). OOGIE_INTERP_CAMERA=0 turns it off.
- **Shadow passes together:** runs with the same quads (hash of their data) decide together: all move by the
  first one's shift or all are drawn as is (~7,400 groups a 2 s in chapter 1, kv4f_c1).
- **Depth/stencil restored:** ip_save_targets now saves and restores each record's depth-stencil target too
  (48 slots, textures held with AddRef while saved).
- **Flicker check (new, Verified):** OOGIE_SHOT_LOG=1 tags each saved picture real or in-between; every
  in-between picture must lie within the real frames either side (+-40). Chapters 1, 3, 5, 7, 10, 13, 16, 19
  at 2x (kv4f_*, kv4g_*) and chapter 1 at 3x/165 (kv4f3x_c1): outside pixels median 0-822, worst 4,302 (a
  cutscene camera cut), otherwise only real motion (spinning item, glowing windows). Frame Rate 30/60 (auto
  mode), 120, 165, Unlimited, 120+vsync on a 60 Hz screen: correct rates, no stalls (kv4m*, kv4am*).
- **Stale vertex address in the names (Verified on the laptop):** the new log line caught a draw whose vertex
  address alternated 00007FF7 / 000E037C from frame to frame (kv4soak). The backend_gpu_vsh patch took the
  address from the first input the program reads even when that input is switched off (size or stride 0), and
  a switched-off input keeps a stale offset. Now the first input that really is an array (1 if none). Found
  again by shape in chapter 1: 112-346 a 2 s -> 0 (kv4p_c1). Likely the owner's every-120-frames name changes
  (**Hypothesis** until the owner's next log).
- Known, small: a spinning pickup whose vertices the game rewrites each frame can go unpaired (a=0 worst 2,756
  pixels in chapter 3 at one moment, kv4z_c3; 40 in another run): it turns one frame ahead in the in-between
  pictures. No streaks or flicker.
- Not verified: a 165 Hz screen, the PS2 HD pack, the owner's scene.
