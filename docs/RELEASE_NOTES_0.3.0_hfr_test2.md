# Project Oogie 0.3.0: High Frame Rate test 2

**This is a test build, not the main release.** Keep your normal copy and unzip
this into its own folder. Copy your `saves` folder across if you want your own
saves there.

## What changed since test 1

**Test 1 was broken, and the cause has been found and fixed.** In test 1, Jack
and other characters flickered, went see-through, and showed black streaks. The
game uses a few vertex programs (Jack's among them) that the in-between pictures
need a special version of. A bug in the code that writes that version cut part of
it out, so those programs never worked, and everything they draw was missing from
every in-between picture. Since every other picture was an in-between, characters
flickered constantly. Two smaller faults were fixed too: in-between pictures
ignored the game's backface culling, and one in-between picture per frame,
always exactly halfway, only suited 120 Hz screens.

**One Frame Rate setting, like other PC ports.** F1 > Display > **Frame Rate**:

| Choice | What you get |
| --- | --- |
| 30 | Half the pictures; the game keeps its normal speed (the old 30 was slow motion) |
| 60 (Original) | The game as it always was |
| 120, 144, 165, 240, 360 | That many pictures a second |
| Unlimited | As many as your PC can make; with **Vertical Sync** on, your monitor's refresh rate |

Changes should apply straight away, with no restart (only tested at start-up so
far: please check switching in the menu). **Vertical Sync** On waits for the
monitor (no tearing, never above its refresh rate). Off doesn't wait and may tear.

**Best setup:** Vertical Sync **On** and Frame Rate **Unlimited**, or your monitor's
rate (for example 165). If you used test 1, your old setting carries over as
Unlimited.

## How it works
The game itself still runs at 60 steps a second, so its speed, physics, films and
music never change. Above 60, the extra pictures are blended between the game's
own frames, and each one is placed by the exact moment it reaches the screen, so
motion stays correct at any refresh rate, including 144 and 165 Hz.

One limit to know: no picture can be made while the game is working out its next
frame (about 6-8 ms of each 16.7 ms on a fast PC), so the screen holds the last
picture for that moment. At 165 Hz expect roughly 140-160 pictures a second, not
a perfect 165. `Show Frame Rate` (F1 > Graphics) now counts pictures, so you can
see it.

## What to check
- Do Jack, enemies and bosses ever flicker, go see-through or throw out lines?
- Is movement smoother than 60, in play and in cutscenes?
- Does the game keep its normal speed?
- Try Frame Rate 165 or Unlimited with Vertical Sync on, and 60, to compare.
- Fights, Boss Rush, Chapters, menus, films, save and load.

If something goes wrong, note the chapter and send `logs\play.log`. Every 5 seconds
it records pictures a second, the game's frames a second, and your monitor's
refresh rate.

## How it has been tested (60 Hz laptop)
- Test 1's broken pictures were reproduced on the laptop (Jack see-through in
  Chapter 1, missing in Chapter 12) and are gone in this build. In-between
  pictures were saved and looked at in chapters 1, 3, 5, 12, 17 and 20.
- Measured, with the game at exactly 60 frames a second in every case: Frame
  Rate 120 gives 120 pictures, 165 gives about 158 and Unlimited about 250 (both
  with Vertical Sync off, as this screen is 60 Hz), and 30
  gives 30 with the game at full speed. 120 with Vertical Sync on a 60 Hz screen
  correctly stays at 60.
- Window resizing and maximising work. No crashes.
- **Not tested:** a real high refresh screen (this laptop is 60 Hz) and the PS2
  HD textures with this build. Your desktop is the real test.
