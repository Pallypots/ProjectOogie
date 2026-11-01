# Project Oogie: start a chapter, then write values into the running game's
# memory (scripts\memscan.py write) and take window shots before and after,
# to confirm what an address holds. -Pokes "0x420C28=3,0x413188=777".
# Stops the game it started.
param([int]$Chapter = 1, [string]$Name = "pk1", [string]$Pokes = "", [int]$Settle = 20, [string]$Press = "")
$Root = Split-Path -Parent $PSScriptRoot
if (Get-Process oogie_recomp -ErrorAction SilentlyContinue) { "a game is already running; not starting"; exit 1 }
$level = @{ 1 = '0000'; 2 = '0000'; 3 = '0100' }
if ($Chapter -ge 2) { $env:OOGIE_START_CHAPTER = "$Chapter" }
& (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name $Name -Cap 60 -Keep -TimeoutSec 5 -LevelFile ("stm_data\\" + $level[$Chapter] + "\.lz") -Marker "no_marker_xyz" | Out-Null
$env:OOGIE_START_CHAPTER = $null
$p = Get-Process oogie_recomp -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "game not running"; exit 1 }
$ms = Join-Path $Root "scripts\memscan.py"
$shot = Join-Path $PSScriptRoot "window_shot.ps1"
Start-Sleep -Seconds $Settle
powershell -ExecutionPolicy Bypass -File $shot -Name "${Name}_before" | Out-Null
foreach ($kv in $Pokes.Split(',')) { $a, $v = $kv.Split('='); "$a before: " + (py -3 $ms read $a 1); py -3 $ms write $a $v }
if ($Press) { foreach ($q in $Press.Split(';')) { Add-Content (Join-Path $Root "logs\pad_live.txt") $q; Start-Sleep -Milliseconds 600 } }
Start-Sleep -Seconds 3
powershell -ExecutionPolicy Bypass -File $shot -Name "${Name}_after" | Out-Null
foreach ($kv in $Pokes.Split(',')) { $a = $kv.Split('=')[0]; "$a after: " + (py -3 $ms read $a 1) }
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
"done, game $($p.Id) stopped"
