# Project Oogie: boot the game -Times times for -Seconds each and report,
# per session, stalls over 2 s, [GPU] unstick actions, crashes and hitches.
# For rare boot freezes (KNOWN_ISSUES #54).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\boot_loop.ps1 [-Times 8] [-Seconds 45] [-Prefix bl]
param([int]$Times = 8, [int]$Seconds = 45, [string]$Prefix = "bl")
$Root = Split-Path -Parent $PSScriptRoot
for ($k = 1; $k -le $Times; $k++) {
    $n = "$Prefix$k"
    & (Join-Path $PSScriptRoot "stop_test_game.ps1"); Start-Sleep 1
    & (Join-Path $PSScriptRoot "live_start.ps1") -Name $n -D3D | Out-Null
    Start-Sleep -Seconds $Seconds
    $alive = [bool](Get-Process oogie_recomp -ErrorAction SilentlyContinue)
    & (Join-Path $PSScriptRoot "stop_test_game.ps1"); Start-Sleep 1
    $l = Get-Content (Join-Path $Root "logs\$n.err.log")
    $long = @($l | Where-Object { $_ -match '^\[STALL\] #\d+ still no frame after 2' }).Count
    $act = @($l | Where-Object { $_ -match '^\[GPU\]   ' }).Count
    $crash = @($l | Where-Object { $_ -match '^\[CRASH\]' }).Count
    $hit = @($l | Where-Object { $_ -match '^\[HITCH\]' }).Count
    "{0}: alive {1}, stalls>2s {2}, unstick actions {3}, crashes {4}, hitches {5}" -f $n, $alive, $long, $act, $crash, $hit
}
