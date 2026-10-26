# Project Oogie: Chapter Select flicker check with a copy of the owner's
# current saves. Boots, loads GAMEDATA 1, stops at Chapter Select, stays
# -Seconds, and counts the flicker detector's reports and the D3D lock's
# contention from there on. Never touches saves\: copies it to
# saves_ownercopy_now first.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\select_flicker.ps1 -Name cs1 [-Seconds 15]
param([Parameter(Mandatory = $true)][string]$Name, [int]$Seconds = 15)
$Root = Split-Path -Parent $PSScriptRoot
$copy = Join-Path $Root "saves_ownercopy_now"
if (-not (Test-Path (Join-Path $copy "UDATA"))) {
    New-Item -ItemType Directory -Force $copy | Out-Null
    Copy-Item (Join-Path $Root "saves\UDATA") $copy -Recurse
}
$env:OOGIE_TEST_SAVES = $copy
& (Join-Path $PSScriptRoot "load_save.ps1") -Name $Name -StopAtSelect | Out-Null
$env:OOGIE_TEST_SAVES = $null
$log = Join-Path $Root "logs\$Name.err.log"
if (-not (Get-Process oogie_recomp -ErrorAction SilentlyContinue)) { "$Name : did not reach Chapter Select"; exit 1 }
$before = (Get-Content $log).Count
Start-Sleep -Seconds $Seconds
& (Join-Path $PSScriptRoot "stop_test_game.ps1")
Start-Sleep -Milliseconds 500
$after = @(Get-Content $log | Select-Object -Skip $before)
$bad = @($after | Where-Object { $_ -match 'cleared after' }).Count
$con = @(Get-Content $log | Select-String 'another game thread is in D3D.*, (\d+) so far' | ForEach-Object { [int]$_.Matches.Groups[1].Value })
$cmax = if ($con.Count) { ($con | Measure-Object -Maximum).Maximum } else { 0 }
$caps = @($after | Where-Object { $_ -match 'frame overlay: a drawing task' }).Count
"{0}: at Chapter Select {1} s: {2} flicker reports, D3D contention {3}, frame-order caps {4}" -f $Name, $Seconds, $bad, $cmax, $caps
