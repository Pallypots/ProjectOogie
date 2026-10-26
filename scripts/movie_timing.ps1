# Project Oogie: time the intro film (mv_00.bik) from its open to the title
# film (mv_title.bik), with no presses, and read the frame rate in the title
# bar while the title film plays. For checking that a change does not alter
# film or game speed. Usage: powershell -File scripts\movie_timing.ps1 -Name mt1
param([Parameter(Mandatory = $true)][string]$Name)
$Root = Split-Path -Parent $PSScriptRoot
$log = Join-Path $Root "logs\$Name.err.log"
& (Join-Path $PSScriptRoot "stop_test_game.ps1"); Start-Sleep 1
& (Join-Path $PSScriptRoot "live_start.ps1") -Name $Name -D3D | Out-Null
$t0 = Get-Date
while (-not (Select-String -Path $log -Pattern "mv_00\.bik" -Quiet -ErrorAction SilentlyContinue)) {
    Start-Sleep -Milliseconds 100; if (((Get-Date) - $t0).TotalSeconds -gt 120) { "no intro"; exit 1 } }
$a = Get-Date
while (-not (Select-String -Path $log -Pattern "mv_title\.bik" -Quiet)) {
    Start-Sleep -Milliseconds 100; if (((Get-Date) - $a).TotalSeconds -gt 300) { "no title"; exit 1 } }
$b = Get-Date
Start-Sleep 6
$title = (Get-Process oogie_recomp).MainWindowTitle -replace '.*\| ', ''
& (Join-Path $PSScriptRoot "stop_test_game.ps1")
"{0}: intro film {1:N1} s, title film at {2}" -f $Name, ($b - $a).TotalSeconds, $title
