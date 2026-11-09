# Project Oogie 0.3.0: High Frame Rate test 4

**This is a test build, not the main release.** Keep your normal copy and unzip
this into its own folder. Copy your `saves` folder across if you want your own
saves there.

## What changed since test 3

Test 3 made things worse above 60. The game slowed down, stalled for close to
half a second at a time, and still flickered and threw out black lines. Your
log from chapter 1 showed why, and every cause below is fixed:

1. **The slowdown.** The extra pictures need to know which object in one
   frame is which object in the frame before. Test 3 did this by comparing
   every repeated object with up to 48 others, number by number. Your
   chapter 1 scene had about 1,900 objects a frame, 1,800 of them copies of
   the same model, so the comparing took 12 to 20 ms a frame. The game then
   ran at 40-50 frames a second instead of 60, the extra pictures stopped,
   and the automatic render resolution dropped. This was measured on the
   laptop: test 3's comparing cost 12 ms a frame in chapter 13, and test 4's
   costs under 1 ms. It now has a fixed time budget each frame, so it can't
   slow the game down however busy the scene gets.
2. **The half-second freezes.** A second matching step ("found again by
   shape") compared every unmatched object with every other one. On your
   desktop, once every 2 seconds most objects changed their internal names
   for one frame, so this became millions of comparisons. It's now limited
   too. A stress test that forces those name changes every 2 seconds keeps
   every frame's extra pictures, and the worst frame takes under 4 ms.
3. **Things jumping ahead of the world.** Any object the extra picture
   couldn't pair up (a new effect, a shadow that couldn't be matched, an
   object whose data changed) was drawn with the *next* frame's camera while
   everything else used the in-between camera. When the camera moved, those
   pieces jumped ahead and snapped back on every extra picture: flicker and
   see-through gaps. They now get the in-between camera too.
4. **Shadow passes out of step.** Each shadow is drawn three times over the
   same shape. Each pass used to decide on its own whether to move, so one
   could move while another stayed put, and that leaves black streaks. The
   three passes now always decide together.
5. **Depth and stencil put back.** After the extra pictures, the game's depth
   and stencil buffers are restored as well as its colours, so nothing from an
   extra picture can leak into the next real frame.

6. **Objects losing their names.** Part of each object's name was its
   vertex data address. When one of an object's inputs was switched off, the
   game left an old leftover address there that changed from frame to frame,
   so the object got a new name and lost its partner. On the laptop, objects
   that had to be found again this way went from 100-350 every 2 seconds to
   none. This is probably what happened on your desktop every 2 seconds too.

Nothing else changed: same Frame Rate setting, same Vertical Sync setting.

Known small issue: a spinning pickup can sometimes turn a frame ahead in the
extra pictures. It doesn't flicker or leave lines.

## Settings

F1 > Display > **Frame Rate** (30, 60, 120, 144, 165, 240, 360, Unlimited) and
**Vertical Sync**. The game always runs at its normal speed.

**Best setup:** Vertical Sync **On** and Frame Rate **165** (your monitor's
rate) or **Unlimited**.

## How it was tested (60 Hz laptop)

- Your slowdown was reproduced on the laptop (chapter 13: test 3's matching
  cost 12 ms a frame and the game fell behind) and is gone in test 4.
- Every shown picture, real and in-between, was saved while Jack ran through
  chapters 1, 3, 5, 7, 10, 13, 16 and 19. Each in-between picture was checked
  automatically: it must lie between the real frames on either side of it.
  The only pixels that didn't were things that really move between frames
  (a spinning item, glowing windows, a cutscene camera cut).
- Pictures checked at 2x and 3x render size, Frame Rate 30, 60, 120, 165 and
  Unlimited, plus a 10-minute run at Unlimited.
- **Not tested:** a real 165 Hz screen, the PS2 HD textures, and your exact
  scene. Your desktop is still the real test.

## What to check

- Does the game keep its normal speed at 165 and Unlimited?
- Any flicker, see-through walls or black lines while you move or the camera
  turns?
- Is movement smoother than at 60?

If something goes wrong, note the chapter and send `logs\play.log`. It now
records more detail about the pairing, which will show what happened.
