# Project Oogie 0.2.7

One-Shot Kill now works on bosses, PS2 HD textures, three button-prompt
styles, and a Steam Deck audio fix.

## Cheats (F1 > Cheats)
- **One-Shot Kill works on bosses, fixed** (GitHub #6). Bosses take damage
  through their own code, which 0.2.6 never touched. Now one hit finishes
  each fight through the game's own ending, so the cutscenes, rewards and
  dance battles stay as they are:
  - **Shadow Oogie** (Chapter 1): one hit, then his usual reward.
  - **Mega Spider:** hit the glowing spot on its back; it sinks and sends
    out a single small spider instead of a swarm. Kill it, the Mega Spider
    comes back in its last, smallest form, and one more hit ends the fight.
    (In 0.2.6 test builds it could stay underground for good.)
  - **Dr. Finkelstein** (Chapter 8): the first brain you get into his head
    counts as the second, and the normal ending plays.
  - **Lock, Shock and Barrel's bathtub, then the trio** (Chapter 15),
    **Casino Oogie** (Chapter 17, the dice in his belly too) and **Mega
    Oogie** (Chapter 24, his fight only; the dance afterwards is untouched):
    one hit that the game counts.
  - **Fire King and Ice King** (Chapter 20): one hit each. A hit of the
    wrong element no longer heals them to full.
  - Every other enemy kind: one hit (ghosts and the rest, not only
    skeletons).
  - Played through here: Shadow Oogie, the Chapter 5 Mega Spider, ghosts,
    skeletons and Casino Oogie's robots. The other bosses were checked in the
    game's code but not played to the end yet: if one of them does not end
    the way it should, please tell us which and attach logs\play.log.
- **Max Combo, new.** The chapter's best combo stays at 999, the most the
  game counts. It sits between Max Exclamation Points and Freeze Time.

Thank you to DeliriumGuylol for testing, the logs and the ideas.

## PS2 HD textures and button prompts (F1 > Mods)
- **PS2 HD Textures.** RetroGenerica's HD texture pack for the PS2 version
  (made with Solid Snack) now works in this port: about 830 of the game's textures
  are matched to it. It is not included; download it from its author's page:
  https://gbatemp.net/threads/the-nightmare-before-christmas-oogies-revenge-ps2-hd-texture-pack-xbox-button-texture-pack-100-complete-updated-2024.623200/
  Start the game with the downloaded file in your Downloads folder, the game folder
  or mods\ and it offers to install it for you (or unpack it anywhere inside
  mods\ yourself). On and off in F1 > Mods.
- **Button Prompts:** Automatic (follows the pad you use), Original Xbox,
  Modern Xbox (needs RetroGenerica's Modern Xbox Buttons pack, installed the
  same way) or PlayStation (the game's own PS2 art, no pack needed for the
  main prompts). (GitHub #9)
- Needs a DirectX 11 graphics card (feature level 11_0), as nearly every PC
  from the last ten years has.

## Fixed
- **Steam Deck: Jack's singing cut in and out** (GitHub #9). Under Proton,
  some sound voices never report that they have stopped; the game then waited
  on them about four times a second in the dance fights, with no sound at all
  while it waited, and the sound buffer ran dry in Proton's longer pauses.
  On Steam Deck and Linux the wait is now a quarter as long and the sound
  buffer twice as deep (170 ms). Windows is unchanged. If it still happens, a
  logs\play.log helps: it now notes how often voices had to be stopped.

## Tested
- Chapter 1 Shadow Oogie with One-Shot Kill: one hit, his defeat scene, and
  his reward in the pause menu.
- Chapter 5 Mega Spider: weak point, one small spider, the last small form,
  one hit, and the key it gives you. Missing the small spider just brings the
  Mega Spider back, as in the original.
- PS2 HD pack: 831 textures in use, no read errors; all three button styles
  checked in the pause menu.
- The 0.2.7 package: starts, plays and runs Chapter 1 with and without the
  packs.
- Not tested here: the other bosses to the end, the Chapter 16 and secret
  Mega Spider fights, and the Steam Deck sound change (it only applies under
  Proton).
