# How the game picks chapter, world and area (for a chapter-jump test helper)

Read-only research, 2026-10-23 (side session), from `build_out/disasm/asm/text.asm` and the
XBE's data. Verified = read from the code or data; Hypothesis = inferred, not run.

## Globals (Verified: code)
| Address | Meaning | Evidence |
|---|---|---|
| 0x00413164 | Current chapter, 1..24 | written by sub_00040950 (arg) and sub_00077EF0 (= 1) |
| 0x00413168 | Set to 1 by the New Game reset (meaning unknown) | sub_00077EF0 |
| 0x0041316C | Current world (0x00..0x0C) | read by both callers of the level loader |
| 0x00413170 | Current area within the world | same |
| 0x00413174 | Cleared on chapter start (Hypothesis: entry point / sub-area) | sub_00040950 |
| 0x003DF9BC | Copy of the chapter number | sub_00040950 |

## Functions (Verified: code)
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

## Chapter table at 0x002C5AA0 (Verified: XBE data)
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

## Suggested chapter-jump helper (Hypothesis, not built)
1. From the title or a loaded save, call `sub_00040950(N)` from a debug key or env var
   (e.g. `OOGIE_START_CHAPTER=N`), on the game thread at a safe point (between frames), then
   let the game's own chapter-start flow run, ideally the same path Chapter Select uses
   (sub_0004D9E0 / task 0x4BD80).
2. Simpler first test: hook sub_00040950 entry and replace its argument with N when the env
   var is set, then start a New Game: the game should begin at chapter N with its own set-up.
3. Unknown: what else a chapter start needs (story flags, unlocked forms like Pumpkin King
   or Santa Jack, items). Chapters past 3 may misbehave if progress flags (0x413178..) are
   unset; loading a save at the chapter end is the safer route for those.
