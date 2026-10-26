# Project Oogie: start the game for a live, screenshot-driven session.
# Presses are sent by appending lines ("start", "a", "down", "a:400") to logs\pad_live.txt
# (scripts\live_press.ps1 does that). Finished frames land in logs\live\ while
# logs\flipdump.flag exists (scripts\live_flips.ps1 manages that).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\live_start.ps1 -Name live1 [-D3D] [-ReportMs 2000]
#   -D3D   use the Direct3D 11 back end (port/src/d3d11_backend.c) instead of
#          the CPU rasteriser and the runtime's framebuffer window.
param([Parameter(Mandatory = $true)][string]$Name, [int]$ReportMs = 2000, [string]$Exe = "",
      [switch]$D3D)
$Root = Split-Path -Parent $PSScriptRoot
# Never run a test while the owner is playing (2026-10-22: a test closed the
# owner's game). owner_game_check.ps1 knows which games the tests started.
& (Join-Path $PSScriptRoot "owner_game_check.ps1")
if ($LASTEXITCODE -ne 0) { "a game the tests did not start is running (the owner's?): not starting $Name"; exit 1 }
$live = Join-Path $Root "logs\live"
New-Item -ItemType Directory -Force $live | Out-Null
Get-ChildItem $live -Filter *.bmp -ErrorAction SilentlyContinue | Remove-Item
$pad = Join-Path $Root "logs\pad_live.txt"
Set-Content -Path $pad -Value "" -NoNewline
$flag = Join-Path $Root "logs\flipdump.flag"
Remove-Item $flag -ErrorAction SilentlyContinue
$env:RECOMP_PB_EXEC = "1"; $env:RECOMP_AC97_READY = "1"
$env:RECOMP_MISSING_FILES = "1"; $env:RECOMP_USB = "1"; $env:RECOMP_KEYBOARD = "1"
# Freed memory is reused (KNOWN_ISSUES #38): without it the 64 MB arena fills
# up and a level reload (Continue after a death) crashed in XACT. OOGIE_NO_RECLAIM=1 to compare.
if ($env:OOGIE_NO_RECLAIM -eq "1") { Remove-Item Env:RECOMP_HEAP_RECLAIM -ErrorAction SilentlyContinue }
else { $env:RECOMP_HEAP_RECLAIM = "1" }
# Test runs keep their own saves (saves_test\UDATA, seeded with the owner's
# Chapter 1 save): scripted presses can answer the Mayor's save prompt, and
# must never write over the owner's saves in saves\ (KNOWN_ISSUES #52).
$env:RECOMP_SAVE_DIR = if ($env:OOGIE_TEST_SAVES) { $env:OOGIE_TEST_SAVES } else { Join-Path $Root "saves_test" }   # OOGIE_TEST_SAVES: e.g. saves_ownercopy (a copy, never saves)
# Test runs use their own settings file (defaults, rewritten if missing), so
# the owner's settings.ini edits never change what a test presses.
$env:OOGIE_SETTINGS = if ($env:OOGIE_TEST_SETTINGS) { $env:OOGIE_TEST_SETTINGS } else { Join-Path $Root "logs\settings_test.ini" }   # OOGIE_TEST_SETTINGS: e.g. a copy of the owner's settings
$env:RECOMP_PAD_LIVE = $pad
$env:RECOMP_PB_REPORT_MS = "$ReportMs"
Remove-Item Env:RECOMP_WATCHDOG_SECS -ErrorAction SilentlyContinue
if ($D3D) {
    $env:OOGIE_D3D11 = "1"
    $env:OOGIE_QUIET_GPU = "1"
    $env:RECOMP_VBLANK = "1"             # 60 Hz vertical-blank interrupt
    Remove-Item Env:RECOMP_FB_WINDOW, Env:RECOMP_FB_DUMP, Env:RECOMP_FB_DUMP_FLIPS -ErrorAction SilentlyContinue
    $env:OOGIE_SHOT_PREFIX = (Join-Path $live "f")
    $env:OOGIE_SHOT_FLAG = $flag
} else {
    Remove-Item Env:OOGIE_D3D11, Env:OOGIE_SHOT_PREFIX, Env:OOGIE_SHOT_FLAG, Env:OOGIE_QUIET_GPU, Env:RECOMP_VBLANK -ErrorAction SilentlyContinue
    $env:RECOMP_FB_WINDOW = "1"
    $env:RECOMP_FB_DUMP = (Join-Path $live "f")
    # Per-flip dumps of the finished frame while logs\flipdump.flag exists.
    $env:RECOMP_FB_DUMP_FLIPS = $flag
}
if (-not $Exe) { $Exe = Join-Path $Root "bin\oogie_recomp.exe" }
$p = Start-Process -FilePath $Exe -WorkingDirectory $Root `
     -RedirectStandardError (Join-Path $Root "logs\$Name.err.log") `
     -RedirectStandardOutput (Join-Path $Root "logs\$Name.out.log") -PassThru
Add-Content -Path (Join-Path $Root "logs\test_pids.txt") -Value $p.Id
Write-Host "started pid $($p.Id)"
