# Project Oogie: boot to Chapter 1 gameplay at real speed (D3D11 back end, 60 Hz pacing).
# Waits on log milestones instead of fixed timings. Run after
#   scripts\live_start.ps1 -Name <name> -D3D
# Usage: powershell -ExecutionPolicy Bypass -File scripts\live_route_fast.ps1 -Name <name>
param([Parameter(Mandatory = $true)][string]$Name, [int]$TimeoutSec = 240)
$Root = Split-Path -Parent $PSScriptRoot
$log = Join-Path $Root "logs\$Name.err.log"
$s = Join-Path $PSScriptRoot "live_press.ps1"
function P([string]$press, [int]$wait) { powershell -ExecutionPolicy Bypass -File $s -Press $press -Wait $wait | Out-Null }
function WaitFor([string]$pattern) {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) {
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern $pattern -Quiet)) { return $true }
        Start-Sleep -Milliseconds 500
    }
    Write-Host "timeout waiting for $pattern"; return $false
}
if (-not (WaitFor "mv_00\.bik")) { exit 1 }
Start-Sleep -Seconds 2
P "start:300" 1      # skip the intro movie
if (-not (WaitFor "mv_title\.bik")) { exit 1 }
Start-Sleep -Seconds 5  # title fades in
P "start:300" 4      # PRESS START -> main menu (cursor on LOAD GAME)
P "up:300" 2         # -> NEW GAME
P "a:300" 4          # -> difficulty (cursor on NORMAL)
P "up:300" 2         # -> EASY (owner: always test on Easy, enemies die faster)
P "a:300" 1          # -> opening cutscene mv_04
if (-not (WaitFor "mv_04\.bik")) { exit 1 }
Start-Sleep -Seconds 3
P "start:300" 3      # skip it
if (-not (WaitFor "0000\.lz")) { exit 1 }
Start-Sleep -Seconds 15
for ($i = 0; $i -lt 8; $i++) { P "a:300" 5 }   # through the opening dialogue
Write-Host "route done"
