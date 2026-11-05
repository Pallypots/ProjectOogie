# Project Oogie: start a chapter (optionally in another world) with the test
# cheats settings and save a game frame every -Every seconds, pressing nothing,
# so the chapter's opening cutscene and the start of its level can be looked at.
#   powershell -File scripts\chapter_frames.ps1 -Chapter 20 -Name cf20 [-Seconds 120] [-Every 3] [-World "6,0,0,0"]
# Frames: logs\<Name>\t<sec>.png. Stops the game it started.
param([int]$Chapter = 20, [string]$Name = "cf", [int]$Seconds = 120, [double]$Every = 3,
      [string]$World = "", [string]$Settings = "logs\settings_cheats.ini", [string]$Press = "", [int]$ProbeAt = 0, [string]$LevelFile = "stm_data\\\d{4}\.lz")
# -LevelFile: the log pattern that means the level has loaded (chapter 9 loads no stm_data\NNNN.lz; use "jack\.lz")
$probed = $false
if ($ProbeAt -gt 0) { $env:OOGIE_TEXPROBE = "1" }
$Root = Split-Path -Parent $PSScriptRoot
if (Get-Process oogie_recomp -ErrorAction SilentlyContinue) { "a game is already running; not starting"; exit 1 }
$env:OOGIE_TEST_SETTINGS = (Resolve-Path (Join-Path $Root $Settings)).Path
if ($World) { $env:OOGIE_START_WORLD = $World; $env:OOGIE_WARP_LOG = "1" }
if ($Chapter -ge 2) { $env:OOGIE_START_CHAPTER = "$Chapter" }
& (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name $Name -Cap 60 -Keep -TimeoutSec 5 -LevelFile $LevelFile -Marker "no_marker_xyz" | Out-Null
$env:OOGIE_START_CHAPTER = $null; $env:OOGIE_START_WORLD = $null; $env:OOGIE_WARP_LOG = $null
$p = Get-Process oogie_recomp -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "game not running"; exit 1 }
$out = Join-Path $Root "logs\$Name"; New-Item -ItemType Directory -Force $out | Out-Null
Add-Type -AssemblyName System.Drawing
$live = Join-Path $Root "logs\live"
$t0 = Get-Date; $n = 0
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds -and -not $p.HasExited) {
    $s = [int]((Get-Date) - $t0).TotalSeconds
    Get-ChildItem $live -Filter "f*.bmp" -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^f\d+\.bmp$' } | Remove-Item
    $fl = Join-Path $Root "logs\flipdump.flag"; Set-Content $fl "1"; Start-Sleep -Milliseconds 250; Remove-Item $fl -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 400
    $b = Get-ChildItem $live -Filter "f*.bmp" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($b) { $i = [System.Drawing.Image]::FromFile($b.FullName); $i.Save((Join-Path $out ("t{0:D3}.png" -f $s)), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose(); $n++ }
    if ($ProbeAt -gt 0 -and $s -ge $ProbeAt -and -not $probed) {
        # texture probe (OOGIE_TEXPROBE=1): every draw of one frame and its textures in the log
        $pf = Join-Path $Root "logs\texprobe.flag"; Set-Content $pf "1"; Start-Sleep -Milliseconds 1500; Remove-Item $pf; $probed = $true
    }
    if ($Press) { powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "live_press.ps1") -Press $Press -Wait 0 | Out-Null }
    while (((Get-Date) - $t0).TotalSeconds -lt ($s + $Every)) { Start-Sleep -Milliseconds 100 }
}
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
"done: $n frames in $out, game $($p.Id) stopped"
