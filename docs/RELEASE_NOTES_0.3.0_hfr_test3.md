# Project Oogie 0.3.0: High Frame Rate test 3

**This is a test build, not the main release.** Keep your normal copy and unzip
this into its own folder. Copy your `saves` folder across if you want your own
saves there.

## What changed since test 2

Test 2 still flickered above 60 on a high refresh screen: see-through walls and
characters, and black streaks coming down from Jack. Those faults only showed
while things were moving, and test 2 had mostly been checked on still scenes.
This time every check was done with Jack running and the camera moving, and
three separate causes were found and fixed:

1. **Shadows.** The game draws its character shadows from hundreds of small
   pieces each frame, in three passes that must line up exactly. The extra
   pictures paired up the wrong pieces, and even the right pieces drifted apart
   because the game rebuilds every shadow from scratch each frame. That caused
   the streaks, the pale bands and the shadow smears that made walls look
   see-through. Now each extra picture draws the frame's exact shadow, moved
   smoothly along with its character, so it can't crack.
2. **Motion trails.** Some scenes (the Soul Robber swing, some cutscenes) have
   the game's own afterimage effect. The extra pictures were leaking into it
   and smearing the next frames. Everything the extra pictures draw is now put
   back before the game's next real frame.
3. **Skipped frames.** When objects appeared or disappeared, the game could
   lose track of some models for a frame, and that frame got no extra pictures
   (a small hitch). The models are now recognised by their shape too.

Nothing else changed: same Frame Rate setting, same Vertical Sync setting.

## Settings

F1 > Display > **Frame Rate** (30, 60, 120, 144, 165, 240, 360, Unlimited) and
**Vertical Sync**. The game always runs at its normal speed.

**Best setup:** Vertical Sync **On** and Frame Rate **Unlimited**, or your
monitor's rate (for example 165).

## How it was tested (60 Hz laptop)

- A measured check: an extra picture placed exactly on the previous frame must
  match that frame. Test 2's code missed by about 8,900 pixels in chapter 1;
  this build misses by 0 to about 550 pixels (out of 1.2 million) across
  chapters 1, 3, 7, 10 and 16.
- Half-way pictures were saved and looked at with Jack running in chapters 1
  (walking and the skeleton fight), 3, 5, 7, 10, 13 and 16, plus later
  chapters: no streaks, bands or see-through walls.
- **Not tested:** a real high refresh screen, render sizes above 2x on a fast
  GPU, and the PS2 HD textures with this build. Your desktop is the real test.

## What to check

- Do walls, Jack, enemies or bosses ever flicker, go see-through or throw out
  black lines when you move or the camera turns?
- Do shadows follow the characters smoothly?
- Is movement smoother than 60, in play and cutscenes?
- Try Frame Rate 165 or Unlimited with Vertical Sync on, and 60, to compare.

If something goes wrong, note the chapter and send `logs\play.log`.
