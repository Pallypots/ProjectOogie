# Project Oogie: does game logic run on a clock or one step per frame?
# Boots to Chapter 1 at a frame cap (OOGIE_FPS_CAP), presses nothing once the
# level file 0000.lz is open, and times how long the game takes to reach the
# next level-driven file load (default rsd_3mhd.lz, loaded when the opening
# scene hands over to gameplay in au1). Run at two caps: equal times mean a
# clock; times in the ratio of the frame rates mean one step per frame.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\timing_cutscene.ps1 -Name tc60 -Cap 60
param([Parameter(Mandatory = $true)][string]$Name, [int]$Cap = 60,
      [string]$Marker = "rsd_3mhd\.lz", [int]$TimeoutSec = 300, [switch]$Keep,
      [string]$LevelFile = "0000\.lz",
      [switch]$LoadFirst)   # open the LOAD GAME screen and back out before NEW GAME (its art, sbuty_u.stm, loaded first)   # the level file the chapter loads (OOGIE_START_CHAPTER: another one)
$Root = Split-Path -Parent $PSScriptRoot
$log = Join-Path $Root "logs\$Name.err.log"
$s = Join-Path $PSScriptRoot "live_press.ps1"
function P([string]$press, [int]$wait) { powershell -ExecutionPolicy Bypass -File $s -Press $press -Wait $wait | Out-Null }
function Has([string]$p) { (Test-Path $log) -and (Select-String -Path $log -Pattern $p -Quiet) }
function WaitFor([string]$p, [int]$t = 180) {
    $t0 = Get-Date
    while (-not (Has $p) -and ((Get-Date) - $t0).TotalSeconds -lt $t) { Start-Sleep -Milliseconds 200 }
    return (Has $p)
}
# A log left by an earlier run with this name would satisfy every wait at once.
Remove-Item $log -ErrorAction SilentlyContinue
# One game at a time: they share the pad file.
& (Join-Path $PSScriptRoot "stop_test_game.ps1"); Start-Sleep -Seconds 1
$env:OOGIE_FPS_CAP = "$Cap"
# In-process, not through powershell.exe: a child whose output is piped
# stays blocked while the game it started holds the pipe.
& (Join-Path $PSScriptRoot "live_start.ps1") -Name $Name -D3D | Out-Null
Remove-Item Env:OOGIE_FPS_CAP
Write-Host "started $(Get-Date -Format T)"
if (-not (WaitFor "mv_00\.bik")) { "no intro"; & (Join-Path $PSScriptRoot "stop_test_game.ps1"); exit 1 }
Write-Host "intro seen $(Get-Date -Format T)"
# Start until the title movie runs (a press before the virtual pad is up is lost)
$t0 = Get-Date
# After each press give the title movie 5 s to show up in the log; one press
# too many lands on the title screen and throws the menu sequence off.
Start-Sleep -Seconds 3
do { P "start:300" 0 } while (-not (WaitFor "mv_title\.bik" 5) -and ((Get-Date) - $t0).TotalSeconds -lt 90)
Start-Sleep -Seconds 9       # let the title finish fading in; earlier presses are ignored
P "start:300" 4      # PRESS START -> main menu (cursor on LOAD GAME)
if ($LoadFirst) { P "a:300" 7; P "b:300" 5 }   # LOAD GAME screen, then back to the menu
# The menu wraps (NEW GAME / LOAD GAME / OPTIONS), so a lost or doubled press
# lands elsewhere: read which line is lit (the selected one is brighter) and
# press up until it is NEW GAME.
Add-Type -AssemblyName System.Drawing
function MenuRow {
    $o = powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "live_flips.ps1") -Count 1 -Tag menu -TimeoutSec 15 | Select-Object -First 1
    if (-not $o) { return -1 }
    $b = [System.Drawing.Bitmap]::FromFile(($o -split ' ')[0])
    $sc = [Math]::Max(1.0, $b.Width / 640.0)    # dumps are larger at a render scale above 1 (also 1.5, 2.25 ...)
    $best = -1; $bv = -1
    foreach ($k in 0..2) {
        $y0 = 344 + 26 * $k; $sum = 0; $n = 0
        for ($y = $y0; $y -lt $y0 + 14; $y += 2) { for ($x = 250; $x -lt 400; $x += 3) {
            $c = $b.GetPixel([int]($x * $sc), [int]($y * $sc)); $sum += $c.R + $c.G; $n++ } }
        if ($sum / $n -gt $bv) { $bv = $sum / $n; $best = $k }
    }
    $b.Dispose()
    return $best
}
for ($i = 0; $i -lt 12; $i++) {
    $row = MenuRow
    Write-Host "menu row $row"
    if ($row -eq 0) { break }
    P "up:120" 2
}
# NEW GAME -> difficulty -> opening movie mv_04. Owner: test on EASY (enemies
# die faster). The difficulty cursor starts on NORMAL; up = EASY. Then, as a
# fallback if a press was dropped, A until the movie starts (an extra A only
# moves one screen on; it may then start on NORMAL).
P "a:300" 4          # NEW GAME -> difficulty
P "up:300" 2         # NORMAL -> EASY
P "a:300" 1          # start
$null = WaitFor "mv_04\.bik" 8
$t1 = Get-Date
do { P "a:300" 0 } while (-not (WaitFor "mv_04\.bik" 5) -and ((Get-Date) - $t1).TotalSeconds -lt 45)
if (-not (WaitFor "mv_04\.bik")) { "no mv_04"; & (Join-Path $PSScriptRoot "stop_test_game.ps1"); exit 1 }
# The game ignores Start for the first seconds of a movie: repeat until the
# level starts loading.
$t1 = Get-Date
# (jack.lz is the first load after the movie, before the Chapter 1 card,
# which a further Start would skip.)
do { Start-Sleep -Seconds 3; P "start:300" 0 } while (-not (WaitFor "jack\.lz" 4) -and ((Get-Date) - $t1).TotalSeconds -lt 60)
if (-not (WaitFor $LevelFile)) { "no level"; & (Join-Path $PSScriptRoot "stop_test_game.ps1"); exit 1 }
$t0 = Get-Date
$fps = @()
while (-not (Has $Marker) -and ((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) {
    Start-Sleep -Seconds 1
    $p = Get-Process oogie_recomp -ErrorAction SilentlyContinue
    if ($p -and $p.MainWindowTitle -match '\| ([0-9.]+) fps') { $fps += [double]$Matches[1] }
}
$el = ((Get-Date) - $t0).TotalSeconds
# -Keep: leave the game running in gameplay (profiling, screenshots)
if (-not $Keep) { & (Join-Path $PSScriptRoot "stop_test_game.ps1") }
$avg = if ($fps.Count) { ($fps | Measure-Object -Average).Average } else { 0 }
"{0}: cap {1}, marker {2} after {3:N1} s (reached: {4}), mean window fps {5:N1}" -f $Name, $Cap, $Marker, $el, (Has $Marker), $avg
