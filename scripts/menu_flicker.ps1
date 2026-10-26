# Project Oogie: menu flicker check (KNOWN_ISSUES #51). Boot to the main menu,
# open LOAD GAME and its GAMEDATA list, and save -Frames frames there; report
# frames whose brightness differs from both neighbours (scripts\video_flashes.py).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\menu_flicker.ps1 -Name mf1 [-Frames 600]
param([Parameter(Mandatory = $true)][string]$Name, [int]$Frames = 600)
$Root = Split-Path -Parent $PSScriptRoot
$log = Join-Path $Root "logs\$Name.err.log"
$pad = Join-Path $Root "logs\pad_live.txt"
$live = Join-Path $Root "logs\live"
function Has([string]$p) { (Test-Path $log) -and (Select-String -Path $log -Pattern $p -Quiet) }
function WaitFor([string]$p, [int]$t = 60) {
    $t0 = Get-Date
    while (-not (Has $p) -and ((Get-Date) - $t0).TotalSeconds -lt $t) { Start-Sleep -Milliseconds 300 }
    return (Has $p)
}
function P([string]$k, [double]$wait) { Add-Content -Path $pad -Value $k; Start-Sleep -Milliseconds ([int]($wait * 1000)) }
Remove-Item $log -ErrorAction SilentlyContinue
& (Join-Path $PSScriptRoot "stop_test_game.ps1"); Start-Sleep -Seconds 1
& (Join-Path $PSScriptRoot "live_start.ps1") -Name $Name -D3D | Out-Null
if (-not (WaitFor "mv_00\.bik")) { "no intro"; exit 1 }
Start-Sleep -Seconds 3
$t0 = Get-Date
do { P "start:300" 0 } while (-not (WaitFor "mv_title\.bik" 5) -and ((Get-Date) - $t0).TotalSeconds -lt 90)
Start-Sleep -Seconds 9
P "start:300" 4
P "a:200" 5          # LOAD GAME (main menu cursor starts there)
if (-not (WaitFor "stm_data\\save\.stm" 10)) { "no LOAD GAME screen" }
P "a:200" 5          # LOAD -> GAMEDATA list
Get-ChildItem $live -Filter "$Name*.png" -ErrorAction SilentlyContinue | Remove-Item
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "live_flips.ps1") -Count $Frames -Tag $Name -TimeoutSec ([int]($Frames / 15) + 30) | Select-Object -Last 1
& (Join-Path $PSScriptRoot "stop_test_game.ps1")
$py = if ($env:OOGIE_PYTHON) { $env:OOGIE_PYTHON } else { (Get-Command python -ErrorAction SilentlyContinue).Source }   # Python 3 with numpy and opencv
& $py (Join-Path $PSScriptRoot "video_flashes.py") $live (Join-Path $Root "logs\$Name") --tag $Name --max 5 | Select-Object -First 3
