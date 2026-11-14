# Project Oogie 0.3.1

Answers every open GitHub issue (#13 to #17). Sorry it took a while: most of my
time has gone into **[Project Soul Robber](https://github.com/Pallypots/Project-Soul-Robber)**,
a full decompilation of the game (its whole code rebuilt as readable source),
which will eventually go further than this recompilation can.

## Boss Rush, improved (#13, thanks Bzshizzle)
Modelled on how other action games run their boss rushes: full strength going
in, a way to recover during a fight, and a score for the whole run at the end.

- **The whole health bar.** Jack starts Boss Rush with the game's maximum
  health (30, three times a new game's 10), refilled before every boss.
- **Health pumpkins during fights.** Boss arenas have no pick-ups, so every
  8 hits you land on a boss (or its minions) earn a big health pumpkin: the
  game's own pick-up, with its sound and glow. If you are already at full
  health it is kept (up to 2) and given the moment you are hurt.
- **Results for each boss, and for the whole run.** After each boss you get
  the game's own results screen, now graded like a normal chapter (in 0.3.0
  it skipped the grading). After the last boss it shows the whole Boss Rush:
  total time, best combo, total damage taken and total "!" points, each grade
  the average of your grades against the bosses, and the overall rank by the
  game's own rule.
- **Boss health bar fixed.** With *HUD at Screen Edges* on, the gold top of
  a boss's health bar moved to the edge of the screen while the bar stayed,
  so the bar seemed to stick out of its frame. The whole bar now moves together.
  The longer pumpkin row from the full health bar also stays in one piece.

## PS2 HD textures (#14, thanks EpicBongZilla)
The PS2 HD Texture Pack by RetroGenerica (with Solid Snack) has worked in
Project Oogie since 0.2.7, including the 2024 update. Download it from
[its GBAtemp page](https://gbatemp.net/threads/the-nightmare-before-christmas-oogies-revenge-ps2-hd-texture-pack-xbox-button-texture-pack-100-complete-updated-2024.623200/),
leave the downloaded file in your Downloads folder (or the game folder), and
start the game: it offers to install it. F1 > Mods switches it on and off.

- **Wire fence fixed.** The pack's Chapter 2 wire fence is no sharper than
  the original and came out a washed-out grey, so the game now keeps the
  original fence. The other 841 HD textures are unchanged.
- Checked against the 2024 download: every file the game uses is in it.

## Settings menu key (#15, thanks Morilix07)
- **F10** now opens the settings menu too, for keyboards where F1 is a media key.
- The key can be changed: F1 > Keyboard > *Open This Menu* (or
  `SettingsMenu =` under `[Keyboard]` in settings.ini). F1, F10 and Back+Start
  on a pad always work.

## Slow motion explained (#16, #17)
**Casino Oogie (#17, thanks DraidenWolf):** this fight really is the heaviest
scene in the game for the port. It sends about 6,600 separate drawing jobs per
frame, about three and a half times the busiest level, and turning them into
PC graphics calls keeps one CPU core fully busy. A slower CPU like the Steam
Deck's falls behind, and because the game's speed is tied to its frame rate,
the fight and its sound slow down. Lowering the resolution does not help much
here, since the limit is the CPU. Merging those drawing jobs is the next
performance task.

The game's speed is tied to its frame rate, so a slow frame rate plays in slow
motion. If gameplay runs well under full speed for 10 seconds, the game now
says so on screen and suggests what to change, and
`logs\play.log` records the likely cause (for example, no graphics driver in
use, so Windows draws the game in software).
