# Project Oogie: high frame rate prototype check (docs/HIGH_FRAME_RATE.md).
# Starts a chapter with OOGIE_INTERP set, lets it run, and prints the pacing
# and [INTERP] cost lines plus the presents per second. Stops the game it started.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\ip_test.ps1 -Name ipO -Chapter 3 -Cap 30 -Interp 1
param([Parameter(Mandatory = $true)][string]$Name, [int]$Chapter = 3, [int]$Cap = 60, [string]$Interp = "1",
      [int]$Seconds = 60, [string]$Env = "", [switch]$Shots, [switch]$Pan)   # -Shots: save a few frames (with OOGIE_INTERP_SHOTS=1 the in-between ones too) to logs\live\f*.bmp   # -Env "OOGIE_INTERP_WAIT=0;X=1"
$Root = Split-Path -Parent $PSScriptRoot
if (Get-Process oogie_recomp -ErrorAction SilentlyContinue) { "a game is already running; not starting"; exit 1 }
$level = @{ 1 = '0000'; 2 = '0000'; 3 = '0100'; 4 = '0100'; 5 = '0100'; 6 = '0100'; 7 = '0200'; 10 = '0302'; 13 = '0[1-9]\d\d'; 15 = '0[1-9]\d\d'; 16 = '0500' }
$env:OOGIE_INTERP = $Interp
foreach ($kv in ($Env -split ';')) { if ($kv -match '^(\w+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] } }
if ($Chapter -ge 2) { $env:OOGIE_START_CHAPTER = "$Chapter" }
& (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name $Name -Cap $Cap -Keep -TimeoutSec 5 -LevelFile ("stm_data\\" + $level[$Chapter] + "\.lz") -Marker "no_marker_xyz" | Out-Null
$env:OOGIE_START_CHAPTER = $null; $env:OOGIE_INTERP = $null
$p = Get-Process oogie_recomp -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "game not running"; exit 1 }
$fps = @(); $t0 = Get-Date; $shot_done = 0
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds -and -not $p.HasExited) {
    Start-Sleep -Seconds 2
    $q = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if ($Shots -and -not $shot_done -and ((Get-Date) - $t0).TotalSeconds -ge 30) {
        $shot_done = 1
        Get-ChildItem (Join-Path $Root "logs\live") -Filter "f*.bmp" -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^f\d+\.bmp$' } | Remove-Item
        if ($Pan) { Add-Content (Join-Path $Root "logs\pad_live.txt") "lleft:2000"; Start-Sleep -Milliseconds 700 }   # Jack running (the camera here is fixed; the right stick does not turn it)
        $fl = Join-Path $Root "logs\flipdump.flag"; Set-Content $fl "1"; Start-Sleep -Milliseconds 250; Remove-Item $fl
    }
    if ($q -and $q.MainWindowTitle -match '\| ([0-9.]+) fps') { $fps += [double]$Matches[1] }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 2
$log = Join-Path $Root "logs\$Name.err.log"
Select-String -Path $log -Pattern "\[PACE\]|GPU replay|crash|PURECALL|bugcheck" | Select-Object -First 12 | ForEach-Object { $_.Line }
$ip = Select-String -Path $log -Pattern "^\[INTERP\] \d+ flips" | Select-Object -Last 6 | ForEach-Object { $_.Line }
$ip
if ($fps.Count) { "window fps: mean {0:N1}, samples {1}" -f ($fps | Measure-Object -Average).Average, ($fps -join ' ') }
