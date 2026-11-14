# Plan: Project Oogie 0.3.1 (every open GitHub issue, 2026-11-14)

Built from `main` (0.3.0). The high frame rate work stays on its own branch
until the owner confirms test 4 on the 165 Hz desktop.

| Issue | Request | What 0.3.1 does |
|---|---|---|
| #13 (Bzshizzle) | Boss Rush: maxed-out health, health refills, the boss health bar's top sits above its frame | **Full health bar:** Boss Rush gives Jack the game's maximum health (30, the cap in sub_000D1E10; a new game has 10) and fills it before every boss. **Health pumpkins in the fight:** every 8 hits Jack lands on a boss (or its minions) earn a big pumpkin (+8), given through the game's own pickup routine (sub_000A9A30: its sound and glow), saved for later if he is already full. **Boss Rush results:** each boss keeps its own results screen; the last one shows the whole run (total time, best combo, total damage, total "!" points), each grade the average of the bosses' grades, and the rank from the game's own rule. **Health bar:** find and fix the misplaced top. |
| #14 (EpicBongZilla) | PS2 HD texture pack, wire fence error | Already supported since 0.2.7 (and installs itself from Downloads). The owner's 2024 download was checked file by file: all 804 files the port uses are in it. The fence (ami1, Chapter 2) is the one fence picture that matches badly (colour pattern 0.62, same 256x256 size as the original): dropped, the original fence stays. Reply explains how to use the pack. |
| #15 (Morilix07) | F1 does not work on some keyboards | Settings menu key can be changed (Controls > Settings Menu Key, or settings.ini), and F10 always opens it too. Back+Start on a pad as before. |
| #16 (gabrielnascimentobelmot-oss) | 5 fps in gameplay, 45 in menus | Likely software rendering (no usable GPU driver) or a weak GPU at a high render size. The game now says so on screen when it runs slowly for 10 s (with the fix to try), and the log names the cause. Reply in Portuguese and English asking for play.log. |
| #17 (DraidenWolf) | Casino Oogie slowdown on Steam Deck | Profile the Casino Oogie fight on the slow laptop and fix what costs time; same slow-motion notice; ask for play.log. |

Replies to every issue: apology for the delay, pointer to Project Soul Robber
(the full decompilation), and why the recomp has been slower.

Release: version 0.3.1, notes in docs/RELEASE_NOTES_0.3.1.md, zip from
scripts/package_release.ps1, GitHub release, replies on #13-#17.
