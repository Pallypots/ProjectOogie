# Project Oogie: soak test. Boots into Chapter 1, then leaves Jack among the
# skeletons (taking hits, rumble, game over, Continue with A) for -Minutes,
# and reports crashes. -NoReclaim runs with RECOMP_HEAP_RECLAIM off.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\soak_hits.ps1 -Name sk1 [-Minutes 6] [-NoReclaim]
param([Parameter(Mandatory = $true)][string]$Name, [int]$Minutes = 6, [switch]$NoReclaim)
$Root = Split-Path -Parent $PSScriptRoot
$log = Join-Path $Root "logs\$Name.err.log"
$lp = Join-Path $PSScriptRoot "live_press.ps1"
if ($NoReclaim) { $env:OOGIE_NO_RECLAIM = "1" } else { Remove-Item Env:OOGIE_NO_RECLAIM -ErrorAction SilentlyContinue }
& (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name $Name -Cap 60 -Keep -TimeoutSec 200 | Out-Null   # in-process: a piped child would block while the game runs
Remove-Item Env:OOGIE_NO_RECLAIM -ErrorAction SilentlyContinue
$t0 = Get-Date
$conts = 0
$seen = 0
while (((Get-Date) - $t0).TotalMinutes -lt $Minutes) {
    Start-Sleep -Seconds 5
    if (-not (Get-Process oogie_recomp -ErrorAction SilentlyContinue)) { break }
    $n = (Select-String -Path $log -Pattern "cont2d_u").Count
    if ($n -gt $seen) {
        $seen = $n
        Start-Sleep -Seconds 4
        powershell -ExecutionPolicy Bypass -File $lp -Press "a:200" -Wait 10 | Out-Null
        powershell -ExecutionPolicy Bypass -File $lp -Press "a:200" -Wait 1 | Out-Null
        $conts++
    }
}
$alive = [bool](Get-Process oogie_recomp -ErrorAction SilentlyContinue)
& (Join-Path $PSScriptRoot "stop_test_game.ps1")
$crash = Select-String -Path $log -Pattern "^\s*in sub_" | Select-Object -First 1
"{0}: reclaim={1} minutes={2:N1} continues={3} survived={4} {5}" -f $Name, (-not $NoReclaim), ((Get-Date) - $t0).TotalMinutes, $conts, $alive, ($(if ($crash) { "crash " + $crash.Line.Trim() } else { "" }))
