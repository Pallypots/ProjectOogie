# Project Oogie: KNOWN_ISSUES #57, the menu task's waiter count. Runs
# pause_cycles three ways: the original bookkeeping with the thread-start race
# forced (task threads reach their first wait 40 ms late), the fix with the
# race forced, and the fix as shipped. Expect flicker only in the first.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\sem_race_check.ps1 [-Tag sr] [-Cycles 10]
param([string]$Tag = "sr", [int]$Cycles = 10)
$runs = @(
    @{ n = "${Tag}_old_late"; credit = "0"; late = "40" },
    @{ n = "${Tag}_fix_late"; credit = "1"; late = "40" },
    @{ n = "${Tag}_fix";      credit = "1"; late = "0" }
)
foreach ($r in $runs) {
    $env:OOGIE_SEM_CREDIT = $r.credit
    $env:OOGIE_TASK_WAIT_LATE_MS = $r.late
    & (Join-Path $PSScriptRoot "pause_cycles.ps1") -Name $r.n -Cycles $Cycles
    $log = Join-Path (Split-Path -Parent $PSScriptRoot) "logs\$($r.n).err.log"
    $sem = @(Select-String -Path $log -Pattern '\[SEM\] #\d+ released before' -ErrorAction SilentlyContinue).Count
    "   {0}: waiter-count corrections {1}" -f $r.n, $sem
}
$env:OOGIE_SEM_CREDIT = $null
$env:OOGIE_TASK_WAIT_LATE_MS = $null
