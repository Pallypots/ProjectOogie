# Project Oogie 0.3.0

Two new ways to play, requested in GitHub #13. Thanks to Bzshizzle for the idea
and t3ammokeym0key for backing it.

## New: pick how you want to play
Choose **NEW GAME** and a difficulty, and a new page follows. It is part of the
game's own title menu, with the same lettering, glow and buttons.

- **Story**: the whole game in order, from Chapter 1, exactly as before.
- **Chapters**: jump to any of the 24 chapters, at the difficulty you picked,
  from the game's own Chapter Select.
- **Boss Rush**: every major boss in a row, with both forms, upgrades and full
  health:
  1. Shadow Oogie
  2. Mega Spider
  3. Dr. Finkelstein
  4. Lock, Shock and Barrel
  5. Casino Oogie
  6. The Fire and Ice Kings
  7. Oogie, the final dance, and the ending

Good to know:
- Chapters is set up to give you the forms you would have by that chapter
  (Pumpkin King from Chapter 6, Santa Jack from Chapter 9), but not the upgrades
  or souls from a full playthrough, so late chapters can be harder.
- Saving during Chapters leaves all 24 chapters open on that save. Use a spare
  slot if you want to keep your story save as it is.
- Lock, Shock and Barrel and Oogie each start with a short walk to the fight.
- After some bosses you will see their reward screen. Press B to leave it, then
  the results screen leads to the next boss.
- The three new labels are in English in every language for now.

## How well this has been tested
- Tested by script: the new page, Chapter Select, and starting all seven fights
  in order. A test bot beat Shadow Oogie and moved on to the next boss by itself.
  Casino Oogie reached its results after its defeat was forced. The final dance
  and the ending were tested in the previous version, not through a Boss Rush.
- Seven Story chapters were spot-checked across the game on this build.
- Not tested yet: switching forms in these two modes, and real wins against the
  other five bosses in Boss Rush. If a
  fight does not move on after its closing cutscene, it should within about a
  minute. If it still does not, you can play on to the end of that chapter.
  Please report it, with `logs\play.log`.
