# Project Oogie: repeat scripts\load_save.ps1 until a log matches a pattern.
# For rare events (KNOWN_ISSUES #43: the GP scratch SGE table changing).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\repro_loop.ps1 -Prefix rp -Tries 10
#        [-Pattern "maps page 0x0073C000"] [-PlaySec 90]
param([string]$Prefix = "rp", [int]$Tries = 10, [string]$Pattern = "maps page 0x0073C000", [int]$PlaySec = 90)
$Root = Split-Path -Parent $PSScriptRoot
for ($k = 1; $k -le $Tries; $k++) {
    $name = "$Prefix$k"
    & (Join-Path $PSScriptRoot "load_save.ps1") -Name $name -Pause -PlaySec $PlaySec
    & (Join-Path $PSScriptRoot "stop_test_game.ps1"); Start-Sleep 2
    if (Select-String -Path (Join-Path $Root "logs\$name.err.log") -Pattern $Pattern -Quiet) {
        "HIT in $name"; exit 0
    }
    "${name}: no hit"
}
"no hit in $Tries tries"
