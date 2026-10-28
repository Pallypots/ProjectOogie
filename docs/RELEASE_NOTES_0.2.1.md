# Project Oogie 0.2.1

A fix for Steam Machine, Steam Deck and other Proton / Linux players.

## Fixed
- **Crash right after setup under Proton** (GitHub #1). On some systems the
  memory addresses the game prefers are already in use, and two of the areas
  it needs (a copy of the game's memory and the area used for drawing) ended
  up on top of memory Proton was using. The game crashed at its first drawing
  step. The program now reserves the whole area it needs before it starts
  placing anything, so nothing else can be in the way. This is on
  automatically under Proton / Wine, and on Windows PCs where the same
  problem would occur.

Thank you to Bzshizzle for the log that showed exactly what went wrong.

## Tested
- The crash reproduced on Windows by blocking the same addresses, and gone
  with the fix (the game played normally at 60 fps).
- Under Wine 10 with DXVK: setup and the title screen.
- All 24 chapters with the new memory mode forced on.
- Normal Windows PCs keep using the same memory layout as 0.2.

Still not confirmed on real Steam hardware. If it still fails for you,
please attach `logs/play.log` from the game folder to a GitHub issue.
