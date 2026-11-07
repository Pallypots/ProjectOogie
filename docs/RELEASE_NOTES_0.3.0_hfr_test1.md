# Project Oogie 0.3.0: High Frame Rate test 1

**This is a test build, not the main release.** It is 0.3.0 plus one experimental
feature. Keep your normal copy, and unzip this into its own folder. Copy your
`saves` folder across if you want your own saves there.

## How to try it
1. Press **F1**, go to **Display**, and set **High Frame Rate (Test)** to **On**.
2. Close the game and start it again. The setting only takes effect at a restart.
3. Keep **Frame Rate Cap** at 60. Turn on **Show Frame Rate** (F1 > Graphics) if
   you want to watch it. It counts the game's own frames, so a healthy result
   reads about 60, not 120.

With it **Off**, this build should play exactly like 0.3.0.

## What it does
The game itself still runs at 60 steps a second, so speed and physics do not
change. Between each pair of frames, the port draws an extra blended picture,
which makes 120 pictures a second.

| Your screen | What to expect |
| --- | --- |
| 120 Hz | The best case: each picture gets one refresh |
| 240 Hz | Each picture gets two refreshes |
| 144, 165 Hz and others | Runs, but less evenly. Set the screen to 120 Hz in Windows for a fair test, if it offers that |
| 60 Hz | No benefit, and the game may run slower. Leave it Off |

It needs more processing power. **If your PC can't keep up, the whole game runs in
slow motion**, because the game's speed is tied to its frame rate.

## What to check
- Is movement actually smoother, in play and in cutscenes?
- Does anything jump, smear, ghost or flicker? Watch camera cuts, characters,
  the HUD, menus, films and loading screens.
- Does the game keep its normal speed? Does the counter stay near 60?
- Do fights, Boss Rush, Chapters, save and load, and pause all still work?
- Does it work with the PS2 HD textures, if you use them?

If something goes wrong, note the chapter and attach `logs\play.log`. It records
your screen's refresh rate and how the frames were paced.

## How it has been tested
- Tested on a 60 Hz laptop only. **Nobody has seen this on a real high refresh
  screen yet**, and that is the point of this test. The 240 Hz path has never run
  at all.
- With it on, sample chapters and the start of Boss Rush ran without crashes.
- In four chapters, consecutive pictures were compared and matched in colour and
  brightness, and they looked right. This test found and fixed a bug that tinted
  every other picture.
- On that laptop, frame rates with it on were 34 to 60 fps, against 54 to 60 in the one
  chapter measured with it off. That's the slow motion warning above.
