# Project Oogie 0.2.8 test 2

## PS2 HD textures and button prompts, fixed
- **The packs now load wherever the game is.** The packs' own folder names
  are about 130 characters long. Unless the game sat in a short folder, the
  path to each texture went over Windows' old 260-character limit, so the
  game saw the pack but could not open one file in it: no HD textures, and
  the button choice did nothing. Packs are now read from any depth.
- **Dance battles follow the button choice.** The dance icons (and the
  dance gauge) now show Modern Xbox or PlayStation buttons as chosen. The
  PlayStation dance icons come from the HD pack, so keep it in mods\ for
  those; it no longer has to be switched on for them.
- The packs' PNG editions work as well as the DDS ones.
- F1 > Mods now says what the packs give, for example "HD pack: in use
  (831 HD textures)", instead of only that a folder is there.

Download the packs from RetroGenerica's page:
https://gbatemp.net/threads/the-nightmare-before-christmas-oogies-revenge-ps2-hd-texture-pack-xbox-button-texture-pack-100-complete-updated-2024.623200/
and put both folders, as they are, anywhere inside mods\.

# Project Oogie 0.2.8 test 1

A test build for the sound balance (GitHub #9). Everything else is as in 0.2.7.

## Sound
- **Voices are easier to hear.** The game's cutscene sound is in 5.1
  surround, with the dialogue in the centre channel. When the port turned it
  into stereo, the rear channels came in at full strength and the bass
  channel was mixed in loud, so they covered the dialogue. They now use the
  usual stereo downmix, which brings the dialogue 1 to 2.5 dB forward.
- **Sound effects no longer drown out the rest.** Every ordinary sound effect
  was also played a second time through the reverb channel, as a plain copy,
  so effects came out about 3.5 dB louder than the game asks for. In fights
  the sound hit the loudness ceiling and distorted: 64,689 clipped samples in
  one Mega Spider fight, now about 2,000.
- **New: Music, Voices and Sound Effects volumes** (F1 > Audio, 0 to 200 %
  each). Jack's singing is part of the song tracks, so it follows Music.

These were measured on Windows, not yet heard on a Steam Deck. If the voices
are still too quiet, raise Voices, and please say where.
