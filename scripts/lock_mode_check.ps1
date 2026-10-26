# Project Oogie: the guest CPU lock with priorities (dev cycle 4). Runs the
# menu and stability checks with RECOMP_GUEST_LOCK=1:
#   1. 10 pause open/close cycles
#   2. the same with the thread-start race forced and the #57 semaphore fix
#      off (OOGIE_SEM_CREDIT=0, OOGIE_TASK_WAIT_LATE_MS=40): flicker here
#      without the lock (sr1_old_late: 41 lines); none means the scheduler
#      alone keeps the frame order
#   3. Chapter Select 15 s with a copy of the owner's saves
#   4. soak_hits 6 min
# Usage: powershell -ExecutionPolicy Bypass -File scripts\lock_mode_check.ps1 [-Tag lm1]
param([string]$Tag = "lm1")
$Root = Split-Path -Parent $PSScriptRoot
$env:RECOMP_GUEST_LOCK = "1"
function Flick($n) { @(Select-String -Path (Join-Path $Root "logs\$n.err.log") -Pattern 'FLICKER\]' -ErrorAction SilentlyContinue).Count }
& (Join-Path $PSScriptRoot "pause_cycles.ps1") -Name "${Tag}_pause" -Cycles 10
"   all flicker lines: $(Flick "${Tag}_pause")"
$env:OOGIE_SEM_CREDIT = "0"; $env:OOGIE_TASK_WAIT_LATE_MS = "40"
& (Join-Path $PSScriptRoot "pause_cycles.ps1") -Name "${Tag}_race" -Cycles 10
"   all flicker lines: $(Flick "${Tag}_race")"
$env:OOGIE_SEM_CREDIT = $null; $env:OOGIE_TASK_WAIT_LATE_MS = $null
& (Join-Path $PSScriptRoot "select_flicker.ps1") -Name "${Tag}_select" -Seconds 15
"   all flicker lines: $(Flick "${Tag}_select")"
& (Join-Path $PSScriptRoot "soak_hits.ps1") -Name "${Tag}_soak" -Minutes 6
$l = Get-Content (Join-Path $Root "logs\${Tag}_soak.err.log")
$h = @($l | Select-String '\[HITCH\] f=(\d+) ([0-9.]+)' | ForEach-Object { [double]$_.Matches[0].Groups[2].Value })
"   soak: hitches $($h.Count), over 50 ms $(@($h | Where-Object { $_ -gt 50 }).Count), flicker lines $(Flick "${Tag}_soak")"
$env:RECOMP_GUEST_LOCK = $null
