# Project Oogie: start a chapter (as scripts\chapter_sweep.ps1 does), fight
# with attack and move presses, and take a memory snapshot plus a window shot
# every -Every seconds (build_out\mem\<Name>_NNN.bin, logs\live\<Name>_NNN.png).
# Used to find health and money (scripts\memdiff.py). Stops the game it started.
param([int]$Chapter = 2, [string]$Name = "mc2", [int]$Minutes = 3, [int]$Every = 6, [string]$Settings = "", [switch]$NoShot)
$Root = Split-Path -Parent $PSScriptRoot
if ($Settings) { $env:OOGIE_TEST_SETTINGS = (Resolve-Path $Settings).Path }   # e.g. logs\settings_cheats.ini
if (Get-Process oogie_recomp -ErrorAction SilentlyContinue) { "a game is already running; not starting"; exit 1 }
$level = @{ 1 = '0000'; 2 = '0000'; 3 = '0100'; 4 = '0100'; 5 = '0100'; 6 = '0100'; 7 = '0200'; 10 = '0302'; 16 = '0500' }
Remove-Item (Join-Path $Root "build_out\mem\$Name`_*.bin") -ErrorAction SilentlyContinue
if ($Chapter -ge 2) { $env:OOGIE_START_CHAPTER = "$Chapter" }
& (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name $Name -Cap 60 -Keep -TimeoutSec 5 -LevelFile ("stm_data\\" + $level[$Chapter] + "\.lz") -Marker "no_marker_xyz" | Out-Null
$env:OOGIE_START_CHAPTER = $null
$p = Get-Process oogie_recomp -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "game not running"; exit 1 }
$pad = Join-Path $Root "logs\pad_live.txt"
$seq = @("x:120", "x:120", "x:120", "lleft:300", "x:120", "x:120", "a:120", "lright:300", "x:120", "x:120", "x:120", "lup:300", "x:120", "x:120", "ldown:300")
$t0 = Get-Date; $k = 0; $i = 0; $next = 0
while (((Get-Date) - $t0).TotalMinutes -lt $Minutes -and -not $p.HasExited) {
    if (((Get-Date) - $t0).TotalSeconds -ge $next) {
        $next += $Every
        $tag = "{0}_{1:D3}" -f $Name, $i
        py -3 (Join-Path $Root "scripts\memscan.py") snap $tag 0x3B2000 0x4000000 | Out-Null
        if (-not $NoShot) { powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "window_shot.ps1") -Name $tag | Out-Null }
        "{0,4}s {1}" -f [int]((Get-Date) - $t0).TotalSeconds, $tag
        $i++
    }
    Add-Content -Path $pad -Value $seq[$k % $seq.Count] -ErrorAction SilentlyContinue; $k++
    Start-Sleep -Milliseconds 350
}
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
"done: $i snaps, game $($p.Id) stopped"
