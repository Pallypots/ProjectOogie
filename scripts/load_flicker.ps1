# Project Oogie: boot to the LOAD GAME screen (owner, 2026-10-19: it flickers
# "at boot") and count flashing frames there, -Tries times, with
# OOGIE_FRAME_LOG=1. -Owner uses a copy of the owner's settings.ini
# (logs\settings_owner.ini: fullscreen, stretch, vsync off, 200% scale).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\load_flicker.ps1 [-Tries 4] [-Owner] [-Prefix lf]
param([int]$Tries = 4, [switch]$Owner, [string]$Prefix = "lf", [int]$Frames = 120)
$Root = Split-Path -Parent $PSScriptRoot
$env:OOGIE_FRAME_LOG = "1"
if ($Owner) { $env:OOGIE_TEST_SETTINGS = Join-Path $Root "logs\settings_owner.ini" }
else { Remove-Item Env:OOGIE_TEST_SETTINGS -ErrorAction SilentlyContinue }
for ($k = 1; $k -le $Tries; $k++) {
    $name = "$Prefix$k"
    & (Join-Path $PSScriptRoot "load_save.ps1") -Name $name -StopAtLoad | Out-Null
    Start-Sleep -Seconds 3
    $r = powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "flash_count.ps1") -Tag $name -Count $Frames
    $log = Join-Path $Root "logs\$name.err.log"
    $snaps = @(Select-String -Path $log -Pattern "showing the finished frame").Count
    "$r | snapshots $snaps"
    & (Join-Path $PSScriptRoot "stop_test_game.ps1")
    Start-Sleep -Seconds 2
}
Remove-Item Env:OOGIE_FRAME_LOG, Env:OOGIE_TEST_SETTINGS -ErrorAction SilentlyContinue
