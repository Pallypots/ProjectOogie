# Project Oogie: whole-game sweep (dev cycle 4). For each chapter: New Game
# started at that chapter (OOGIE_START_CHAPTER, recomp_manual.c), then
# -Minutes of play where A is pressed every few seconds (dialogue, jumps) and
# Continue is chosen after a game over. Reports per chapter: reached the level,
# survived, crash, stalls, long frames, flicker, mean fps, a screenshot.
# Level files per chapter: docs/RE_CHAPTER_SELECT.md.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\chapter_sweep.ps1 [-First 1] [-Last 24] [-Minutes 3] [-Tag sw]
param([int]$First = 1, [int]$Last = 24, [int]$Minutes = 3, [string]$Tag = "sw")
$Root = Split-Path -Parent $PSScriptRoot
$level = @{ 1 = '0000'; 2 = '0000'; 3 = '0100'; 4 = '0100'; 5 = '0100'; 6 = '0100'; 7 = '0200'; 8 = '0207';
            9 = '020b'; 10 = '0302'; 11 = '0302'; 12 = '0304'; 13 = '0401'; 14 = '0403'; 15 = '0406';
            16 = '0500'; 17 = '0503'; 18 = '0002'; 19 = '0700'; 20 = '0600'; 21 = '0600'; 22 = '0802';
            23 = '0800'; 24 = '0901' }
$summary = Join-Path $Root "logs\${Tag}_summary.txt"
$lp = Join-Path $PSScriptRoot "live_press.ps1"
function Press([string]$p) { powershell -ExecutionPolicy Bypass -File $lp -Press $p -Wait 0 | Out-Null }
for ($ch = $First; $ch -le $Last; $ch++) {
    $name = "{0}_ch{1:D2}" -f $Tag, $ch
    $log = Join-Path $Root "logs\$name.err.log"
    if ($ch -ge 2) { $env:OOGIE_START_CHAPTER = "$ch" } else { $env:OOGIE_START_CHAPTER = $null }
    & (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name $name -Cap 60 -Keep -TimeoutSec 5 -LevelFile ("stm_data\\" + $level[$ch] + "\.lz") -Marker "no_marker_xyz" | Out-Null
    $env:OOGIE_START_CHAPTER = $null
    $p = Get-Process oogie_recomp -ErrorAction SilentlyContinue
    $reached = (Test-Path $log) -and (Select-String -Path $log -Pattern ("stm_data\\" + $level[$ch] + "\.lz") -Quiet)
    $fps = @(); $conts = 0; $seen = 0
    if ($p -and $reached) {
        $t0 = Get-Date
        $k = 0
        while (((Get-Date) - $t0).TotalMinutes -lt $Minutes) {
            Start-Sleep -Seconds 3
            if (-not (Get-Process oogie_recomp -ErrorAction SilentlyContinue)) { break }
            $k++
            $n = (Select-String -Path $log -Pattern "cont2d_u").Count
            if ($n -gt $seen) { $seen = $n; Start-Sleep -Seconds 4; Press "a:200"; Start-Sleep -Seconds 2; Press "a:200"; $conts++; continue }
            Press "a:150"
            if (($k % 4) -eq 0) {
                $q = Get-Process oogie_recomp -ErrorAction SilentlyContinue
                if ($q -and $q.MainWindowTitle -match '\| ([0-9.]+) fps') { $fps += [double]$Matches[1] }
            }
        }
        powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "window_shot.ps1") -Name $name | Out-Null
    }
    $alive = [bool](Get-Process oogie_recomp -ErrorAction SilentlyContinue)
    & (Join-Path $PSScriptRoot "stop_test_game.ps1")
    $l = if (Test-Path $log) { Get-Content $log } else { @() }
    $h = @($l | Select-String '\[HITCH\] f=\d+ ([0-9.]+)' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
    $crash = @($l | Select-String '^\[CRASH\]|EXCEPTION_|unhandled exception' ).Count
    $stall = @($l | Select-String '\[STALL\] #\d+ no frame').Count
    $flick = @($l | Select-String 'SHOWN BROKEN').Count
    $mean = if ($fps.Count) { ($fps | Measure-Object -Average).Average } else { 0 }
    $line = "ch{0,2}: level {1} reached {2}, alive at end {3}, crash lines {4}, stalls {5}, frames >100 ms {6}, broken frames {7}, continues {8}, mean fps {9:N1}" -f `
        $ch, $level[$ch], $reached, $alive, $crash, $stall, @($h | Where-Object { $_ -gt 100 }).Count, $flick, $conts, $mean
    $line
    Add-Content -Path $summary -Value $line
}
