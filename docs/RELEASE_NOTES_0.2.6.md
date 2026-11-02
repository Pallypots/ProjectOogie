# Project Oogie 0.2.6

A cheat menu that really works, four new cheats, and a Steam Deck freeze fixed.

## Cheats (F1 > Cheats)
All six take effect at once and are saved in settings.ini under [Cheats].

- **Infinite Health, fixed** (GitHub #6). Hits and falls now do no damage at
  all, so the health bar stays full. Before, the cheat only refilled Jack
  after each hit, and the bar kept showing the drop.
- **Max Souls, fixed** (GitHub #6). It now holds your souls (the number in
  the pause menu and at the Witches' Shop) at 99999, the most the game
  holds, so buying never takes them down. The 0.2.4 version set the wrong
  number: the chapter's damage count, which the game uses for the Damage
  grade. If you played with it on, your Damage grade in those chapters
  suffered; replaying a chapter fixes it.
- **Max Exclamation Points, new.** The chapter's exclamation points stay at
  999, the most it counts.
- **Freeze Time, new.** The chapter clock stops while it is on, so the Time
  grade only counts the time it was off.
- **One-Shot Kill, new.** Any of Jack's hits that does damage takes all of
  an enemy's health. Bosses too: if a boss fight stalls, turn it off.
- **Super Speed, new.** Jack runs twice as fast.

Thank you to DeliriumGuylol for testing the cheats, the log, and the ideas.

## Fixed
- **Steam Deck: the game could freeze after a while** (GitHub #3). Sound
  effects are played by "voices". When a voice was stopped and then freed
  before the sound system's background routine had finished with it, that
  routine later walked into freed memory: the game froze. Under Proton this
  is more likely because the emulated sound chip confirms stops later. A
  voice is now always taken off the background routine's lists when it is
  freed, whatever the timing. The same crash once showed on Windows in the
  Chapter 5 spider fight; this closes it for good.

Thank you to DraidenWolf for the Steam Deck logs (and for confirming the
game-over sound fix).

## Tested
- All 24 chapters loaded and played for a minute each with no crashes,
  stalls or broken frames (mean 38-60 fps on the test laptop).
- Each cheat was checked in the game by reading the game's own memory:
  health stays full and no damage is counted, souls 99999, exclamation
  points 999, the chapter clock held, every hit kills a full-health
  skeleton (two hits without the cheat), Jack about 1.8x faster on
  average. All six on together: all values held.
- A Steam-Deck-like timing run on Windows: no crashes, no wild calls.
  The Deck itself is the real test for the freeze fix.

## Known
- Steam Deck: some sounds can still cut out (GitHub #3); a log from this
  version helps.
