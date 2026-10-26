# Project Oogie: boot, load GAMEDATA 1 and start a chapter from Chapter Select.
# Menu path verified by screenshots (ld1, 2026-10-17): title -> START -> main
# menu (cursor on LOAD GAME) -> A -> LOAD GAME (LOAD/EXIT, cursor on LOAD) ->
# A -> GAMEDATA list (cursor on 1) -> A -> "OK to load?" (cursor on No) ->
# left, A -> "Successfully loaded, press A" -> A -> CHAPTER SELECT (cursor on
# the next chapter) -> A -> "Start this chapter?" (cursor on Yes) -> A.
# Read-only on the save: nothing here writes GAMEDATA.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\load_save.ps1 -Name ls1 [-Down 0] [-Pause]
#   -Down N / -Up N  move the chapter cursor first; -Pause  press START
#            30 s into the chapter and report whether the game survives.
param([Parameter(Mandatory = $true)][string]$Name, [int]$Down = 0, [int]$Up = 0, [switch]$Pause, [int]$PlaySec = 30, [switch]$StopAtSelect, [switch]$StopAtLoad)
$Root = Split-Path -Parent $PSScriptRoot
$log = Join-Path $Root "logs\$Name.err.log"
$pad = Join-Path $Root "logs\pad_live.txt"
function Has([string]$p) { (Test-Path $log) -and (Select-String -Path $log -Pattern $p -Quiet) }
function WaitFor([string]$p, [int]$t = 60) {
    $t0 = Get-Date
    while (-not (Has $p) -and ((Get-Date) - $t0).TotalSeconds -lt $t) { Start-Sleep -Milliseconds 300 }
    return (Has $p)
}
function P([string]$k, [double]$wait) { Add-Content -Path $pad -Value $k; Start-Sleep -Milliseconds ([int]($wait * 1000)) }
function Count([string]$p) { if (Test-Path $log) { @(Select-String -Path $log -Pattern $p).Count } else { 0 } }
# Press, then wait for the file the next screen reads (ref1, 2026-10-17):
# LOAD GAME screen -> stm_data\save.stm; GAMEDATA list -> SaveMeta.xbx;
# confirmed load -> data.sav; Chapter Select -> jack.lz then sbuty_u.stm;
# chapter start -> data\world\<n>\bg\camera. Retries a press the game missed.
function Step([string]$key, [string]$pat, [int]$t = 8, [int]$tries = 3, [double]$settle = 2) {
    for ($i = 0; $i -lt $tries; $i++) {
        $c = Count $pat
        P $key 0
        $t0 = Get-Date
        while ((Count $pat) -le $c -and ((Get-Date) - $t0).TotalSeconds -lt $t) { Start-Sleep -Milliseconds 300 }
        if ((Count $pat) -gt $c) { Start-Sleep -Milliseconds ([int]($settle * 1000)); return $true }
    }
    return $false
}
Add-Type -AssemblyName System.Drawing
function MenuRow {   # main menu: which of NEW GAME / LOAD GAME / OPTIONS is lit
    $o = powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "live_flips.ps1") -Count 1 -Tag menu -TimeoutSec 15 | Select-Object -First 1
    if (-not $o) { return -1 }
    $b = [System.Drawing.Bitmap]::FromFile(($o -split ' ')[0])
    $f = $b.Width / 640.0                   # render resolution above 100%
    $best = -1; $bv = -1
    foreach ($k in 0..2) {
        $y0 = 344 + 26 * $k; $sum = 0; $n = 0
        for ($y = $y0; $y -lt $y0 + 14; $y += 2) { for ($x = 250; $x -lt 400; $x += 3) {
            $c = $b.GetPixel([int]($x * $f), [int]($y * $f)); $sum += $c.R + $c.G; $n++ } }
        if ($sum / $n -gt $bv) { $bv = $sum / $n; $best = $k }
    }
    $b.Dispose()
    return $best
}
$ok = $false
for ($try = 1; $try -le 3 -and -not $ok; $try++) {
    Remove-Item $log -ErrorAction SilentlyContinue
    & (Join-Path $PSScriptRoot "stop_test_game.ps1"); Start-Sleep -Seconds 1
    & (Join-Path $PSScriptRoot "live_start.ps1") -Name $Name -D3D | Out-Null
    if (-not (WaitFor "mv_00\.bik")) { "no intro (try $try)"; continue }
    Start-Sleep -Seconds 3
    $t0 = Get-Date
    do { P "start:300" 0 } while (-not (WaitFor "mv_title\.bik" 5) -and ((Get-Date) - $t0).TotalSeconds -lt 90)
    Start-Sleep -Seconds 9
    P "start:300" 4                                   # PRESS START -> main menu
    for ($i = 0; $i -lt 8; $i++) {                    # cursor to LOAD GAME (row 1)
        $row = MenuRow
        if ($row -eq 1) { break }
        if ($row -eq 0) { P "down:120" 2 } elseif ($row -eq 2) { P "up:120" 2 } else { P "start:300" 4 }
    }
    if (-not (Step "a:200" "stm_data\\save\.stm" 8 2 3)) { "no LOAD GAME screen (try $try)"; continue }
    if ($StopAtLoad) { "at LOAD GAME"; exit 0 }
    if (-not (Step "a:200" "SaveMeta\.xbx" 8 3 3)) { "no GAMEDATA list (try $try)"; continue }
    $loaded = $false
    for ($i = 0; $i -lt 3 -and -not $loaded; $i++) {  # GAMEDATA 1 -> "OK?" (No) -> Yes -> load
        $c = Count "data\.sav"
        P "a:200" 3; P "left:120" 2; P "a:200" 0
        $t0 = Get-Date
        while ((Count "data\.sav") -le $c -and ((Get-Date) - $t0).TotalSeconds -lt 8) { Start-Sleep -Milliseconds 300 }
        $loaded = (Count "data\.sav") -gt $c
    }
    if (-not $loaded) { "save not loaded (try $try)"; continue }
    Start-Sleep -Seconds 3
    if (-not (Step "a:200" "stm_data\\jack\.lz" 15 3 1)) { "no Chapter Select (try $try)"; continue }
    $null = WaitFor "sbuty_u\.stm" 30; Start-Sleep -Seconds 5
    if ($StopAtSelect) { "at Chapter Select"; exit 0 }
    for ($i = 0; $i -lt $Down; $i++) { P "down:120" 1.5 }
    for ($i = 0; $i -lt $Up; $i++) { P "up:120" 1.5 }
    P "a:200" 4                                       # chapter -> "Start this chapter?" (Yes)
    $t0 = Get-Date
    $ok = Step "a:200" "world\\[0-9]+\\bg\\camera" 30 2 0
    if (-not $ok) { "no chapter start (try $try)" }
}
if (-not $ok) { & (Join-Path $PSScriptRoot "stop_test_game.ps1"); exit 1 }
"chapter loading after {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds
if ($Pause) {
    Start-Sleep -Seconds $PlaySec
    $c0 = @(Select-String -Path $log -Pattern "\[CRASH\]").Count
    P "start:200" 6
    P "start:200" 6      # unpause
    $c1 = @(Select-String -Path $log -Pattern "\[CRASH\]").Count
    $alive = [bool](Get-Process oogie_recomp -ErrorAction SilentlyContinue)
    "{0}: crashes before pause {1}, after pause {2}, process alive {3}" -f $Name, $c0, $c1, $alive
}
