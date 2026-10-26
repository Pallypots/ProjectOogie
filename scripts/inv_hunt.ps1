# Project Oogie: repeat "load the save, play, open the inventory 3 times"
# until an opening flashes (KNOWN_ISSUES #51), with OOGIE_FRAME_LOG=1.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\inv_hunt.ps1 [-Tries 5]
param([int]$Tries = 5, [string]$Prefix = "ih", [switch]$Probe, [switch]$ProbeAlways, [switch]$Owner)
if ($Owner) { $env:OOGIE_TEST_SETTINGS = Join-Path (Split-Path -Parent $PSScriptRoot) "logs\settings_owner.ini" }   # the owner's settings
$Root = Split-Path -Parent $PSScriptRoot
$env:OOGIE_FRAME_LOG = "1"
if ($Probe -or $ProbeAlways) { $env:OOGIE_TEXPROBE = "1" }   # the probe flag file is only checked then
for ($k = 1; $k -le $Tries; $k++) {
    $name = "$Prefix$k"
    & (Join-Path $PSScriptRoot "load_save.ps1") -Name $name | Out-Null
    Start-Sleep -Seconds 25
    $r = powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "inv_cycle.ps1") -Times 3 -Tag $name
    $r
    if ($ProbeAlways -or $r -match "brightness [0-4].[0-9]..") {
        "FLASHING in $name"
        if ($Probe -or $ProbeAlways) {
            # Keep the bad session: open the inventory and dump one frame.
            Remove-Item (Join-Path $Root "logs\texprobe") -Recurse -ErrorAction SilentlyContinue
            Add-Content -Path (Join-Path $Root "logs\pad_live.txt") -Value "start:200"
            Start-Sleep -Seconds 2
            New-Item (Join-Path $Root "logs\texprobe.flag") -Force | Out-Null
            Start-Sleep -Seconds 3
            Remove-Item (Join-Path $Root "logs\texprobe.flag")
            Start-Sleep -Seconds 60
        }
        & (Join-Path $PSScriptRoot "stop_test_game.ps1")
        break
    }
    & (Join-Path $PSScriptRoot "stop_test_game.ps1")
    Start-Sleep -Seconds 2
}
Remove-Item Env:OOGIE_FRAME_LOG
"ALL DONE"
