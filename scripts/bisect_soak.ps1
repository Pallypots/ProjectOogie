# Project Oogie: soak with groups of today's (2026-10-19) changes switched back
# to the morning behaviour, to find which one makes runs unstable.
# -Old lists the groups kept OLD: log, dsound, ack, nets, render, stall
# Usage: powershell -ExecutionPolicy Bypass -File scripts\bisect_soak.ps1 -Prefix bs1 -Old log,dsound,ack,nets,render,stall [-Times 2] [-Minutes 8]
param([Parameter(Mandatory = $true)][string]$Prefix, [string[]]$Old = @(), [int]$Times = 2, [int]$Minutes = 8)
$map = @{
    log    = @{ OOGIE_SYNC_LOG = "1" }                                   # direct log writes (no pipe)
    dsound = @{ OOGIE_DS_STOP_OLD = "1" }                                # original voice-stop poll
    ack    = @{ OOGIE_ACK_THREAD = "0" }                                 # no kickoff ack thread
    nets   = @{ OOGIE_NO_UNSTICK = "1" }                                 # no GPU safety nets
    render = @{ OOGIE_FULL_CLEARS = "1"; OOGIE_SHOW_SPARSE = "1"; OOGIE_CLEAR_SNAPSHOT = "1" }
    stall  = @{ OOGIE_STALL_LOG = "0" }                                  # no stall sampler (no thread suspends)
}
$Old = @($Old | ForEach-Object { $_ -split "," } | Where-Object { $_ })   # -File passes "a,b" as one string
foreach ($g in $Old) { foreach ($k in $map[$g].Keys) { Set-Item -Path "Env:$k" -Value $map[$g][$k] } }
"old groups: $($Old -join ',')"
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "soak_report.ps1") -Prefix $Prefix -Times $Times -Minutes $Minutes
foreach ($g in $map.Keys) { foreach ($k in $map[$g].Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue } }
