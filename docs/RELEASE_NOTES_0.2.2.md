# Project Oogie 0.2.2

Fixes the crash when deflecting projectiles, and two sound bugs.

## Fixed
- **Crash when deflecting projectiles** (GitHub #2). When you knock a
  projectile back, such as a gargoyle's fireball, the game looks up what
  should happen next in a small table. Some entries in that table pointed at
  parts of the game's code the port had never converted, so the game froze
  and then closed. All 19 missing parts are now converted. A new scan of the
  whole game for tables with the same gap finds none left.
- **Short voice lines were silent** (GitHub #1). Voice lines shorter than
  about a second and a half never played. Mr. Hyde's save-point lines were
  among them. Those lines now play in full.
- **Sounds cutting off early.** Some sounds stopped a moment after they
  started. Mr. Hyde's longer lines and the Soul Robber in the Shadow Oogie
  fight were affected. This came from a timing problem in how the port
  handled the sound chip. In testing, sounds no longer get cut off.

Thank you to DeliriumGuylol for the log that showed exactly where the
deflect crash happened, and to Bzshizzle for reporting the missing voice
lines.

## Tested
- Deflecting gargoyle fireballs in the graveyard: no crash (tested in play).
- All 24 chapters, 2 minutes each, with the new build: no crashes, no
  stalls, 60 fps. Each chapter also checked at startup that all 19
  converted parts are present.
- All 24 chapters again after the two sound fixes.
- Mr. Hyde's two silent lines now play, about 1.2 seconds each.

Not checked yet: the results-screen announcer, which likely had the same
missing-voice bug, and the Soul Robber in the Shadow Oogie fight itself. If you still hit a crash or missing sound, please
attach `logs/play.log` from the game folder to a GitHub issue.
