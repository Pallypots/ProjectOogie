# Project Oogie 0.2.3

French, German, Spanish and Italian; a Steam Deck freeze fixed; Discord status.

## New
- **French, German, Spanish and Italian** (GitHub #4, #5). F1 > Display >
  Language, then restart the game. Menus, the pause screen, map hints, save
  screens and the other on-screen text appear in that language; the spoken
  voices stay English (the disc has one set of voices). All five languages
  were already on the US disc; the port now switches the game to them and
  converts the European text to the US font. That font has no accented
  letters, so é, ü, ñ, ç and the like show as plain e, u, n, c.
- **Discord Rich Presence.** With the Discord app running on the same PC,
  your profile shows that you are playing, the chapter and how long you have
  played. F1 > Display > Discord Status turns it off. Under Proton it needs a
  Discord bridge for Wine.

## Fixed
- **Black screen after talking to the clown in Chapter 2 on Steam Deck /
  Proton** (GitHub #3). The game loads the next cutscene with a low-priority
  helper while the main part of the game waits for it. On the Deck the
  helper never got its turn, so the screen stayed black. Now anything that
  has waited too long for its turn goes first, so the helper always runs.

Thank you to DraidenWolf for the Steam Deck logs, and to iAnn57 and
o0oMaRcUso0o for asking for the European languages.

## Tested
- All 24 chapters with the new build (English), and eight chapters spread
  through the game in French: no crashes.
- Map hints and menus checked on screen in French, German, Spanish and
  Italian.
- Discord status checked against a stand-in for the Discord app (menus,
  then "Chapter 3: The Hanging Tree" once the level loads).

## Known
- The Steam Deck freeze could not be reproduced on Windows; the fix follows
  what the player's log shows. Please report if it still happens.
- Some sounds can still cut out on Steam Deck (GitHub #3); a log from this
  version will help.
