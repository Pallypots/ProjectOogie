# Project Oogie: improvement plan (2026-10-20 evening)

What the PC port needs, in the order I will work on it. Written after a review
of every entry in `KNOWN_ISSUES.md`, the owner's play logs of 10-19 and 10-20,
and today's soak results. Status markers follow the house rule: **Verified**
means a log shows it, **Hypothesis** means it does not yet.

## Where the port stands

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

## Tier 1: stability (tonight)

| # | Work | Why |
|---|------|-----|
| 1 | Find why the command reader goes out of step (#66): re-read the bad word 2 ms later to tell "read before it was written" from "decoded wrongly"; log every CALL the reader takes | It is behind most visible faults; today showed it is not two game threads writing at once (it happens under the one-thread lock too) |
| 2 | Bink worker created in a safe order (`sub_0022F3D0`) | The worker could start before its mutex and event exist; likely cause of #42 |
| 3 | Lock mode (`RECOMP_GUEST_LOCK=1`) to gameplay | Removes a whole class of races (USB, XACT, render lists); boots further after today's fair lock |
| 4 | Longer soaks of the default build after each change | Every fix tonight is kept only if soaks stay at least as good as bk1-bk4 |

## Tier 2: correctness the owner has reported

| # | Work |
|---|------|
| 5 | #49 chapter cards: Chapter 1 -> Chapter 2 in one session (needs a route that moves the Chapter Select cursor) |
| 6 | #63 barcode subtitles: needs a save just before the Ch3 dt07 cutscene (owner) |
| 7 | #47 results-screen announcer |
| 8 | #59 / #60 load screen: banner blink, then rebuild the sweeping shapes for widescreen |

## Tier 3: PC-port features

| # | Feature | Notes |
|---|---------|-------|
| 9 | Automatic save backups | Each launch copies `saves\` to `saves_backup\<date-time>\` (keeps the last 10); a crash can then never cost the owner progress |
| 10 | One-click crash report | On a crash or long freeze, the log, settings and the last frame go into `reports\<date-time>\`, ready to send |
| 11 | Performance mode presets in F1 | "Battery / Balanced / Quality" setting render scale and filtering together (render scale 3 fullscreen is heavy on the Arc 140V) |
| 12 | Later, after baseline gameplay is solid | HD texture packs (deferred by the hard rules), modern load transition, 60 fps cutscene pacing |

## Rules I follow while doing this

- Nothing touches `game\`, the owner's `saves\`, or `third_party\xboxrecomp`
  (runtime changes are build-time patches in `port\patches`).
- A change stays on by default only if soaks with it are at least as good as
  without it; otherwise it stays in the build behind a switch, and the doc
  says so.
- No commits.

## Status at 04:30 on 2026-10-21

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

## Dev cycle 3 (from 2026-10-22 afternoon)

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

## Dev cycle 6 (2026-10-24)
Done: fast texture path and Mipmaps (PC_FEATURES.md). Found while measuring: the long frames at loads (200-500 ms) are the game reading and unpacking its own files (.stm/.lz, sound banks) on its CPU thread, not texture work.

Next, by value:
| # | Area | Work | Why |
|---|------|------|-----|
| 1 | Loading | Native C version of the game's .lz decompressor (sub_00031120, from the rival's notes; verify byte-for-byte against the recompiled one on every file) | Loads are now the main source of long frames and audio dropouts |
| 2 | Smoothness | #72 Casino Clash frame runs | Only chapter under 59 fps |
| 3 | Low-end PCs | Graphics preset row (Low / Medium / High / Ultra) that sets resolution, AA, sharpening, mipmaps, anisotropic together; on battery, Auto resolution starts lower | Owner: "run on a potato ... to a 5090" |
| 4 | Accessibility | Subtitle size/background, hold-to-mash option, invincibility / assist toggles (needs the health address) | Roadmap item |
| 5 | Correctness | #60 door transition in widescreen, #47 announcer, #63 barcode subtitles | Owner reports |
| 6 | Research | Above 60 fps (needs the game's timestep) | Highest risk, last |
