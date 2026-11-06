# Project Oogie 0.2.9

Fixes for GitHub #12, "A Buggy Finale".

## The ending
- **The ending film no longer freezes the game.** When the film turned its
  sound off, the game waited for the sound chip to report that the sound had
  stopped, with no time limit. If that report never came, the game froze.
  The wait now has a limit, and the reports now arrive (see Sound).
- **No more green and red flashes in the ending film.** A film's colour
  picture could land at the same memory address as a picture left over from
  earlier in the session, and the port showed the old picture instead. Film
  pictures are no longer mistaken for those leftovers.
- Tested on the real route: the final dance with Oogie, the results screen,
  the ending film and the credits, played through by a test script.

## Sound
- **Sound effects should no longer drop out in long fights.** This is the same
  fault that froze the ending. The emulated sound chip tells the game when each
  sound has finished. Three timing faults could hold those reports back, and
  one of them could leave them ignored for a long time, sometimes for the rest
  of a session.
  - The sound chip's interrupt could get stuck behind a lock held by the very
    thread waiting for it, and no sound was produced during that wait.
  - A DirectSound counter could get stuck. It is now protected, and if it ever
    sticks anyway, the game puts it right and notes that in the log.
  - One ignored report could hold back every report behind it.
- In the Casino Clash test, voice stops that ran out of time went from 5 or 6
  in 2,630 to 0 in 2,642.

## If you still hear a problem
Please attach `logs\play.log`. It now records what the sound chip was doing
each time a sound had to be cut off. This may also help the missing sounds on
the Steam Deck (GitHub #3 and #9), but it has not been tested on a Deck yet.
