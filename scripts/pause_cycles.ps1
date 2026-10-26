# Project Oogie: open and close the pause menu -Cycles times (owner: "if I
# keep opening it, sometimes it won't be good") and count the flicker
# detector's reports, the D3D lock's contention and frame-order caps after
# the first opening. Settings: OOGIE_TEST_SETTINGS.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\pause_cycles.ps1 -Name pc1 [-Cycles 15]
param([Parameter(Mandatory = $true)][string]$Name, [int]$Cycles = 15)
$Root = Split-Path -Parent $PSScriptRoot
$log = Join-Path $Root "logs\$Name.err.log"
$pad = Join-Path $Root "logs\pad_live.txt"
& (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name $Name -Cap 60 -Keep -TimeoutSec 200 | Out-Null
if (-not (Get-Process oogie_recomp -ErrorAction SilentlyContinue)) { "$Name : boot failed"; exit 1 }
Start-Sleep -Seconds 3
$before = (Get-Content $log).Count
for ($c = 1; $c -le $Cycles; $c++) {
    Add-Content -Path $pad -Value "start:150"; Start-Sleep -Seconds 3
    Add-Content -Path $pad -Value "start:150"; Start-Sleep -Seconds 2
}
& (Join-Path $PSScriptRoot "stop_test_game.ps1")
Start-Sleep -Milliseconds 500
$after = @(Get-Content $log | Select-Object -Skip $before)
$bad = @($after | Where-Object { $_ -match 'cleared after' }).Count
$con = @(Get-Content $log | Select-String 'another game thread is in D3D.*, (\d+) so far' | ForEach-Object { [int]$_.Matches.Groups[1].Value })
$cmax = if ($con.Count) { ($con | Measure-Object -Maximum).Maximum } else { 0 }
$caps = @($after | Where-Object { $_ -match 'frame overlay: a drawing task' }).Count
"{0}: {1} pause open/close cycles: {2} flicker reports, D3D contention {3}, frame-order caps {4}" -f $Name, $Cycles, $bad, $cmax, $caps
