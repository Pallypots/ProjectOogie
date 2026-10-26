# Project Oogie: KNOWN_ISSUES #46 hunt. Load the save, start Chapter 2, then
# walk and attack by script while every presented frame is saved, and list
# single-frame glitches with scripts\video_flashes.py.
# -NoCapture: save no frames (saving stalls the renderer, which made the
# glitches rarer); run with OOGIE_FOG_TRACE=1 OOGIE_FLASH_MEASURE=1 and the
# renderer itself writes logs\drawdiff_<frame>.txt for each glitch.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\flash_hunt.ps1 -Name fh1 [-Frames 1800] [-Wait 25] [-NoCapture]
param([Parameter(Mandatory = $true)][string]$Name, [int]$Frames = 1800, [int]$Wait = 25,
      [switch]$NoCapture)
$Root = Split-Path -Parent $PSScriptRoot
$pad = Join-Path $Root "logs\pad_live.txt"
$live = Join-Path $Root "logs\live"
& (Join-Path $PSScriptRoot "load_save.ps1") -Name $Name | Out-Null
if ($LASTEXITCODE -eq 1) { "$Name : chapter never started"; exit 1 }
Start-Sleep -Seconds $Wait      # chapter card, then control (0: capture the level start too)
Get-ChildItem $live -Filter "$Name*.png" -ErrorAction SilentlyContinue | Remove-Item
$secs = if ($NoCapture) { [int]($Frames / 60) } else { [int]($Frames / 20) + 10 }
$job = Start-Job -ScriptBlock {
    param($pad, $secs)
    $moves = "lup:1500", "x:150", "x:150", "lleft:800", "x:150", "ldown:1500", "x:150", "x:150",
             "lright:800", "x:150", "a:150", "lup:1000", "x:150"
    $t0 = Get-Date; $i = 0
    while (((Get-Date) - $t0).TotalSeconds -lt $secs) {
        Add-Content -Path $pad -Value $moves[$i % $moves.Count]; $i++
        Start-Sleep -Milliseconds 900
    }
} -ArgumentList $pad, $secs
if ($NoCapture) {
    Wait-Job $job | Out-Null
    Remove-Job $job
    $log = Join-Path $Root "logs\$Name.err.log"
    $n = @(Select-String -Path $log -Pattern "^\[FLASH\]").Count
    "$Name : $secs s played, $n flash frame(s) with draw records"
    Select-String -Path $log -Pattern "^\[FLASH\]" | ForEach-Object { "  " + $_.Line }
    exit 0
}
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "live_flips.ps1") -Count $Frames -Tag $Name -TimeoutSec ([int]($Frames / 15) + 30) | Select-Object -Last 1
Stop-Job $job; Remove-Job $job
$py = if ($env:OOGIE_PYTHON) { $env:OOGIE_PYTHON } else { (Get-Command python -ErrorAction SilentlyContinue).Source }   # Python 3 with numpy and opencv
& $py (Join-Path $PSScriptRoot "video_flashes.py") $live (Join-Path $Root "logs\$Name") --tag $Name
