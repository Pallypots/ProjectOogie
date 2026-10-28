# Project Oogie 0.2

Second test release of the native Windows port of *The Nightmare Before
Christmas: Oogie's Revenge*. You still need your own copy of the original
Xbox disc. Thank you to everyone who tried 0.1 and reported back on Reddit
and GitHub: this release comes from your reports.

## Steam Deck, SteamOS and Linux (Proton)
0.1 would not start under Proton for several people (GitHub #1, Steam Deck
reports on Reddit). Four causes were found and fixed:
- The 0.1 zip stored its folders with Windows-style separators, which Linux
  unpacked as single files with backslashes in their names. The zip is now
  built with normal separators.
- Wine's own shader compiler could not build the game's shaders. Microsoft's
  shader compiler (d3dcompiler_47.dll, redistributable) now ships next to the
  program and is used first.
- Started from Steam, the program wrote no log. It now always writes
  `logs\play.log`.
- The freeze detector could crash under Wine while reading a thread's stack.
  It now stays out of the way there.

Tested under Wine 10 with DXVK 3.1.1, the graphics path Proton uses:
first-start setup from the disc image, booting three times, and the title
screen with a correct picture, all without crashes. **Not yet tested on a
real Steam Deck in Game Mode**: reports are very welcome. See "Steam Deck" in
the README.

## Fixes
- Widescreen: the area-loading transition (the growing tree) now fills the
  whole screen instead of stopping at the 4:3 edges with orange bands at the
  sides.

## Weaker PCs
- Two places where the game's graphics code busy-waited on the (emulated)
  graphics chip are removed: less CPU use, heat and battery drain. On two
  low-power laptop cores Casino Clash runs at about 49 fps on the Low preset.
  If you have an older PC, a log from it helps a lot.

## Reporting problems
- The version number is now in the window title and in the settings menu
  (F1, About).
- The first lines of `logs\play.log` now say which build it is, which Windows
  (or Wine / Proton) version, processor and memory you have.
When something goes wrong, please attach `logs\play.log` to a GitHub issue.

## Known issues
- The results screen can skip some announcer lines (#47).
- Not yet tested on a real Steam Deck.
- French and other European languages are not supported yet (planned).
