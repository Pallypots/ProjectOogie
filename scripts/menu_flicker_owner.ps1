# Project Oogie: pause-menu flicker check with the flicker detector only (no
# frame dumps, so any render scale). Boots into Chapter 1, opens the pause
# menu, waits -Seconds, and counts the detector's "cleared after" reports
# logged after START (KNOWN_ISSUES #57). Settings: OOGIE_TEST_SETTINGS.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\menu_flicker_owner.ps1 -Name mo1 [-Seconds 10]
param([Parameter(Mandatory = $true)][string]$Name, [int]$Seconds = 10)
$Root = Split-Path -Parent $PSScriptRoot
$log = Join-Path $Root "logs\$Name.err.log"
$pad = Join-Path $Root "logs\pad_live.txt"
& (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name $Name -Cap 60 -Keep -TimeoutSec 200 | Out-Null
if (-not (Get-Process oogie_recomp -ErrorAction SilentlyContinue)) { "$Name : boot failed"; exit 1 }
Start-Sleep -Seconds 3
$before = (Get-Content $log).Count
Add-Content -Path $pad -Value "start:200"
Start-Sleep -Seconds $Seconds
& (Join-Path $PSScriptRoot "stop_test_game.ps1")
Start-Sleep -Milliseconds 500
$after = @(Get-Content $log | Select-Object -Skip $before)
$bad = @($after | Where-Object { $_ -match 'cleared after .* SHOWN BROKEN' }).Count
$fr = @($after | Where-Object { $_ -match 'FLICKER\] f=\d+ frame draws' }).Count
"{0}: in the pause menu for {1} s: {2} frames shown after their clear, {3} sparse-frame reports" -f $Name, $Seconds, $bad, $fr
