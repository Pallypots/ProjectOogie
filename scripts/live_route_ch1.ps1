# Project Oogie: drive a live session from boot to Chapter 1 (Town Square).
# Run after scripts\live_start.ps1. Timings measured on the CPU rasteriser.
#
# Self-correcting menu presses: each step is "up" then "start".
#   title         : up does nothing, start opens the main menu (cursor on LOAD GAME)
#   main menu     : up moves to NEW GAME, start confirms -> difficulty menu
#   difficulty    : up moves NORMAL -> EASY, start confirms -> opening cutscene
#   cutscene      : up does nothing, start skips it
# so the route works whether the title's fade-in swallows a press or not.
# A fixed sequence ("start, start, up, a") broke whenever the fade was faster
# or slower than expected (live10, live13 ended in the Load Game screen).
$s = Join-Path $PSScriptRoot "live_press.ps1"
function P([string]$press, [int]$wait) {
    if ($press) { powershell -ExecutionPolicy Bypass -File $s -Press $press -Wait $wait | Out-Null }
    else        { powershell -ExecutionPolicy Bypass -File $s -Wait $wait | Out-Null }
}
P "" 50                  # boot to the intro movie
P "start:600" 25         # skip intro -> title fades in
for ($i = 0; $i -lt 4; $i++) {
    P "up:700" 6
    P "start:700" 15
}
P "" 80                  # level load (jack.lz, 0000.lz)
P "a:700" 90             # dismiss the Chapter 1 card -> Town Square, Fountain Area
P "start:1500" 60        # the opening in-engine cutscene holds a black screen
                         # (KNOWN_ISSUES #25); Start skips it and gameplay fades in
powershell -ExecutionPolicy Bypass -File $s -Wait 1
