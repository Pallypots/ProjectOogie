# Project Oogie 0.2.5

Films and cutscenes at the right speed on every monitor.

## Fixed
- **Films and cutscenes played too fast, and their sound fell out of step**
  (GitHub #7). The game runs its logic, films and music once per frame, so it
  must be held to 60 frames a second. With vsync on, the port let the monitor
  do that, and on some PCs the monitor did not hold it: one player's game ran
  at about 120 frames a second, so everything played at double speed. The
  port now measures the real frame rate; if the game runs faster than the cap
  while relying on vsync, its own timer takes over, whatever the monitor,
  G-SYNC or driver settings do.
- **The 30 FPS cap is now labelled for what it does:** "30 (half speed: slow
  motion)". It does not skip frames; it slows the whole game, music
  included, which is why the sound paused at 30. Keep the cap at 60.

Thank you to BloodyAnvil for the report and the log.

## Tested
- With a test setting that makes vsync stop holding the game back, the game
  ran at 106 to 140 frames a second until the measuring caught it within
  seconds and the timer took over at 60.
- Six chapters spread through the game with normal settings: no crashes,
  and the guard stayed out of the way (it only acts when the game runs fast
  for three seconds in a row).

## Known
- Steam Deck: some sounds can still cut out (GitHub #3); a log from 0.2.3 or
  later will help.
