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
