# Project Oogie: start a chapter, then write values into the running game's
# memory (scripts\memscan.py write) and take pictures before and after,
# to confirm what an address holds. -Pokes "0x420C28=3,0x413188=777".
# -Press "start:120;a:120": pad presses after the pokes. -Frames: the game's
# own frames (logs\live\<Name>_before_*.bmp / _after_*.bmp, logs\flipdump.flag)
# instead of window shots, which can catch other windows. -Settings: a
# settings file for the test (OOGIE_TEST_SETTINGS). Stops the game it started.
param([int]$Chapter = 1, [string]$Name = "pk1", [string]$Pokes = "", [int]$Settle = 20, [string]$Press = "",
      [string]$Settings = "", [switch]$Frames, [string]$Reads = "")   # -Reads "0x420C64,0x402FB0": dwords shown before and after
$Root = Split-Path -Parent $PSScriptRoot
if (Get-Process oogie_recomp -ErrorAction SilentlyContinue) { "a game is already running; not starting"; exit 1 }
if ($Settings) { $env:OOGIE_TEST_SETTINGS = (Resolve-Path $Settings).Path }
$level = @{ 1 = '0000'; 2 = '0000'; 3 = '0100' }
$live = Join-Path $Root "logs\live"
$flag = Join-Path $Root "logs\flipdump.flag"      # live_start.ps1 sets OOGIE_SHOT_FLAG to it, frames go to logs\live\f*.bmp
New-Item -ItemType Directory -Force $live | Out-Null
if ($Frames) {
    Remove-Item (Join-Path $live "$Name`_*.bmp"), (Join-Path $live "f*.bmp") -ErrorAction SilentlyContinue
}
if ($Chapter -ge 2) { $env:OOGIE_START_CHAPTER = "$Chapter" }
& (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name $Name -Cap 60 -Keep -TimeoutSec 5 -LevelFile ("stm_data\\" + $level[$Chapter] + "\.lz") -Marker "no_marker_xyz" | Out-Null
$env:OOGIE_START_CHAPTER = $null
$p = Get-Process oogie_recomp -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "game not running"; exit 1 }
$ms = Join-Path $Root "scripts\memscan.py"
$shot = Join-Path $PSScriptRoot "window_shot.ps1"
function Take([string]$tag) {
    if ($Frames) {
        Set-Content $flag "x"; Start-Sleep -Milliseconds 120; Remove-Item $flag -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 300
        Get-ChildItem $live -Filter "f*.bmp" | Where-Object { $_.Name -match '^f\d+\.bmp$' } | ForEach-Object { Move-Item $_.FullName (Join-Path $live ("$Name`_$tag`_" + $_.Name.Substring(1))) -Force }
    } else {
        powershell -ExecutionPolicy Bypass -File $shot -Name "${Name}_$tag" | Out-Null
    }
}
Start-Sleep -Seconds $Settle
foreach ($a in $Reads.Split(',')) { if ($a) { "$a before: " + (py -3 $ms read $a 1) } }
Take "before"
foreach ($kv in $Pokes.Split(',')) { if ($kv) { $a, $v = $kv.Split('='); "$a before: " + (py -3 $ms read $a 1); py -3 $ms write $a $v } }
if ($Press) { foreach ($q in $Press.Split(';')) { Add-Content (Join-Path $Root "logs\pad_live.txt") $q; Start-Sleep -Milliseconds 600 } }
Start-Sleep -Seconds 3
Take "after"
foreach ($kv in $Pokes.Split(',')) { if ($kv) { $a = $kv.Split('=')[0]; "$a after: " + (py -3 $ms read $a 1) } }
foreach ($a in $Reads.Split(',')) { if ($a) { "$a after: " + (py -3 $ms read $a 1) } }
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
"done, game $($p.Id) stopped"
