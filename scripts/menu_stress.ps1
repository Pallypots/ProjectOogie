# Project Oogie: KNOWN_ISSUES #50 check. Load the save, start Chapter 2, then
# open the pause menu and leave it with EXIT -Cycles times; after each cycle
# check that the game still reads the pad (a hang stops the reads).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\menu_stress.ps1 -Name ms1 [-Cycles 15]
param([Parameter(Mandatory = $true)][string]$Name, [int]$Cycles = 15)
$Root = Split-Path -Parent $PSScriptRoot
$pad = Join-Path $Root "logs\pad_live.txt"
$log = Join-Path $Root "logs\$Name.err.log"
& (Join-Path $PSScriptRoot "load_save.ps1") -Name $Name | Out-Null
if ($LASTEXITCODE -eq 1) { "$Name : chapter never started"; exit 1 }
Start-Sleep -Seconds 25
function Reads { @(Select-String -Path $log -Pattern 'PAD: live').Count }
for ($c = 1; $c -le $Cycles; $c++) {
    foreach ($k in "start:200", "up:150", "a:200") { Add-Content -Path $pad -Value $k; Start-Sleep -Seconds 3 }
    Start-Sleep -Seconds 2
    $before = Reads
    Add-Content -Path $pad -Value "lleft:300"
    Start-Sleep -Seconds 3
    if ((Reads) -le $before) { "${Name}: HANG after cycle $c (pad no longer read)"; exit 2 }
}
"${Name}: $Cycles cycles, still responsive"
