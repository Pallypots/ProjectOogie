# Project Oogie: start a chapter with the test cheats settings (Infinite
# Health, so the run is not cut short) and run scripts\hitscan.py while it is
# played. Stops the game it started.
#   powershell -File scripts\hitscan.ps1 [-Chapter 1] [-Seconds 150]
param([int]$Chapter = 1, [string]$Name = "hs1", [int]$Seconds = 150, [string]$Settings = "logs\settings_cheats.ini",
      [string]$Record = "")   # e.g. "0x3F6000:0x3F7000 0x420E00:0x421400": scripts\hitrec.py instead of hitscan.py
$Root = Split-Path -Parent $PSScriptRoot
if (Get-Process oogie_recomp -ErrorAction SilentlyContinue) { "a game is already running; not starting"; exit 1 }
$env:OOGIE_TEST_SETTINGS = (Resolve-Path (Join-Path $Root $Settings)).Path
$level = @{ 1 = '0000'; 2 = '0000'; 3 = '0100' }
if ($Chapter -ge 2) { $env:OOGIE_START_CHAPTER = "$Chapter" }
& (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name $Name -Cap 60 -Keep -TimeoutSec 5 -LevelFile ("stm_data\\" + $level[$Chapter] + "\.lz") -Marker "no_marker_xyz" | Out-Null
$env:OOGIE_START_CHAPTER = $null
$p = Get-Process oogie_recomp -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "game not running"; exit 1 }
Start-Sleep -Seconds 5
if ($Record -eq "move") { py -3 (Join-Path $PSScriptRoot "moverec.py") $Name $Seconds }
elseif ($Record) { py -3 (Join-Path $PSScriptRoot "hitrec.py") $Name $Seconds $Record.Split(' ') }
else { py -3 (Join-Path $PSScriptRoot "hitscan.py") $Seconds }
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
"done, game $($p.Id) stopped"
