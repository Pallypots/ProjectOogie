# PC port features

What the Windows port offers on top of the original game, and what is still to come.
The models are the BLACK (2006) recomp ports (`docs/BLACK_COMPARISON.md`) and the
Burnout 3 port Takedawn (xenophobentx/takedawn), whose settings menu (Back+Start / F1,
pages on LB/RB, Y resets a row, settings file) this one follows. Reviewed 2026-10-18.

## Owner decisions
- 2026-10-18 morning: no separate launcher; the game's original Options menu is left
  exactly as it was; `settings.ini` next to the game stores everything.
- 2026-10-18 afternoon (after seeing the BLACK recomp's RECOMP SETTINGS screen): build
  an in-game settings menu now, themed as Oogie's Revenge, that controls every PC
  feature, so nobody has to edit .ini files. Every new feature gets a row in it.
- Defaults: the original Xbox pad layout on every controller. On the keyboard:
  WASD walk, arrows = A/B/X/Y in the pad's diamond, TFGH = d-pad, Enter = Start,
  Backspace = Back.

## The settings menu ("Recomp Settings", `port/src/pc_menu.c`)
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

## How 16:9 widescreen works (Verified: mn1, hud5)
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

## Status of other features

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

## Added 2026-10-21 (night)
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

## Added 2026-10-22/23 (dev cycle 3)
- **Render Resolution in real sizes** and **Native**; **Anti-Aliasing** and **Sharpening** (F1 > Graphics).
- **HUD at Screen Edges** (F1 > Display, default On): in widescreen, 2D pieces wholly in a screen corner (outer 30% across, top or bottom 30%) are anchored to the wide screen's side; menus, subtitles and boxes stay centred.
- **F12 screenshot:** the shown picture (without the F1 menu or the fps counter) as a PNG in `screenshots\`, written on its own thread.
- **First-run setup:** without `game\default.xbe` the program asks for the player's own disc image, checks it is the supported version (SHA-1 of default.xbe), and copies the files with a progress window. The image is only read. `--extract-iso <image> <folder>` does the same without a window.
- **Mods folder** (2026-10-23): put replacement game files in `mods\<mod name>iles\` under the same path they have on the disc (for example `mods\HD Moviesiles\Movie\mv_04.bik`). A mod with a file named `disabled` in its folder is off. If two mods replace the same file, the mod whose name sorts first wins. The log lists the mods found and each file a mod replaces. `OOGIE_NO_MODS=1` turns mods off.
- **One game thread at a time, by priority** (2026-10-23, KNOWN_ISSUES #69): the default. Same speed as before, and the game's frame order is kept the way the Xbox kept it. `RECOMP_GUEST_LOCK=0` turns it off.
- **Settings menu in the game's own art** (2026-10-23): the F1 menu's frame, cursor, dividers and button pictures come from the player's disc (the pause menu's art), so it looks like part of the game. Built; to be checked in game.
- **Mods page in F1** (2026-10-23): every mod in `mods\` with an On/Off switch (writes or removes its `disabled` file; takes effect for files the game loads afterwards), Open Mods Folder (creates it with a how-to file), and Save Game Textures.
- **Texture packs** (2026-10-23): Save Game Textures writes each texture the game shows to `texture_dump\<width>x<height>_<hash>.png` (once, on a background thread). A mod's `textures\` folder with PNG or DDS files of the same names, any size (for example 4x), replaces them; films and render copies are not replaced (films are .bik mods). Verified tx3.
- **Runs without play.bat** (2026-10-24): double-clicking the program sets everything up itself: saves in `saves\` (backed up to `saves_backup\` at each start, ten kept), the log in `logs\play.log` (two older kept). Verified with the release zip in an empty folder.
- **Player download**: `scripts\package_release.ps1` builds `dist\ProjectOogie-<date>.zip` (9 MB: `Project Oogie.exe`, SDL2, the menu font, symbols, README, licenses). First start copies the game from the player's disc image.
- **Pause in Background** (Audio page, default Off): switching to another window opens the settings menu, which pauses the game.
- **Audio Buffer** (Audio page, default Large): more sound queued ahead, so loading hiccups do not crackle; Small for the least delay. Takes effect at the next start.
- **Graphics card choice**: laptops with two graphics chips use the high-performance one (OOGIE_GPU=powersave for the other); if the graphics driver stops mid-game, the player gets a clear message instead of a frozen black window.
- **Crash reports** (2026-10-24, standalone runs): a crash nobody handles saves `reports\<date_time>\` (play.log and settings.ini) and tells the player where it is and that saves are safe. OOGIE_TEST_CRASH=<seconds> crashes on purpose (tests).
- **Skip Intro Logos** (Display page, default Off): while the company logos film (mv_00.bik) plays, Start is pressed for the player, as skipping by hand does; the game's files are not changed.
- **Window remembers its place** (2026-10-24): position, size and maximised state are saved when the window is moved or resized and restored at the next start if still on a connected monitor ([Display] window_place).

## Added 2026-10-24 (dev cycle 6)
| Feature | Status |
|---|---|
| Fast texture path (`d3d11_backend.c` tex_decode_fast / tex_upload) | Done, Verified: DXT1/3/5 decoded a block at a time, swizzled A8R8G8B8/X8R8G8B8 unswizzled in one pass, LIN_L8 film planes expanded; tf3: 3,127 of 3,127 uploads on the fast path and every one byte-identical to the reference decoder (OOGIE_TEX_VERIFY=1). Texture time over 90 s of Chapter 3: 199 ms vs 2,552 ms with OOGIE_TEX_FAST=0 (tf5_1 / tf5_0), worst single texture 2.2 ms vs 8.0 ms |
| Graphics > Mipmaps (On / Off, default On) | Done. The game asks for trilinear filtering (MIN filter 6) on almost every texture but ships one level, so the Xbox shimmered at a distance; On makes a full mip chain on the GPU (GenerateMips). Off is the original look and sends DXT textures to the GPU still compressed (BC1/2/3, least video memory). Screenshots mip on/off (Chapter 3 gate): both correct |

## Added 2026-10-24 (dev cycle 8)
| Feature | Status |
|---|---|
| Graphics > Quality Preset: Low / Medium / High / Ultra (Custom when the five settings match none) | Done, Verified pr1 (menu screenshots logs/live/pr_menu1-3.png): Custom shown for the owner's settings; Low wrote render_scale 1, aa 0, sharpening 0, mipmaps 0, anisotropic 1; Ultra wrote -1, 2, 1, 1, 16 and the game ran 60.0 fps at 3x (Native, 1440 lines). Not saved itself: it follows the five settings. Low 480p, no AA/sharpening, compressed textures; Medium 720p, AA low, sharpen low, mipmaps, 4x; High Auto, AA high, sharpen low, mipmaps, 8x; Ultra Native, AA high, sharpen low, mipmaps, 16x |
| Auto resolution on battery | Done (not tested on battery: this PC was on mains). Auto starts at 1.5x at most on battery, 1x with battery saver, and drops to that at once if the laptop is unplugged during play ([D3D11] render resolution auto ... on battery) |
| Controls > Auto-Repeat Held Button: Off / A / B / X / Y (accessibility) | Done, built; not yet tried in a button-mashing moment. While the chosen face button is held it reads pressed 50 ms, released 50 ms (about 10 presses a second); pad and keyboard (pc_input.c). Saved as [Controller] auto_repeat |
| Graphics > Colour-Blind Filter: Off / Protanopia / Deuteranopia / Tritanopia (accessibility) | Done, Verified cf1 (Chapter 3, deuteranopia, OOGIE_POST_SPLIT=1 leaves the left half unfiltered): columns either side of the split, mean over the height: blue 28.5-31.0 left, 32.5-33.0 right, red level, as the correction moves red-green differences into blue; 60 fps. Daltonize (public algorithm, own code) as one 3x3 matrix in a last post pass (d3d11_backend.c cb_matrix, cbps). Saved as [Graphics] colour_filter |
