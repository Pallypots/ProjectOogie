# Project Oogie: does the game step its logic per frame or by a clock?
# Boots to the title screen with the D3D11 back end at a given frame cap
# (OOGIE_FPS_CAP), records N presented frames with their write times, and
# prints the brightness of the "PRESS START BUTTON" line per frame. The blink
# period measured in frames and in seconds at two caps answers the question.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\timing_probe.ps1 -Name tp20 -Cap 20 -Count 120
param([Parameter(Mandatory = $true)][string]$Name, [int]$Cap = 60, [int]$Count = 120)
$Root = Split-Path -Parent $PSScriptRoot
$live = Join-Path $Root "logs\live"
$flag = Join-Path $Root "logs\flipdump.flag"
# A log left by an earlier run with this name would satisfy every wait at once.
Remove-Item (Join-Path $Root "logs\$Name.err.log") -ErrorAction SilentlyContinue
$env:OOGIE_FPS_CAP = "$Cap"
# In-process: a powershell.exe child with piped output stays blocked while
# the game it started holds the pipe.
& (Join-Path $PSScriptRoot "live_start.ps1") -Name $Name -D3D | Out-Null
$log = Join-Path $Root "logs\$Name.err.log"
function WaitFor([string]$p) {
    $t0 = Get-Date
    while (-not ((Test-Path $log) -and (Select-String -Path $log -Pattern $p -Quiet)) -and ((Get-Date) - $t0).TotalSeconds -lt 180) { Start-Sleep -Milliseconds 300 }
}
WaitFor "mv_00\.bik"
# Press Start until the title movie starts (a press before the virtual pad is
# up is lost).
$t0 = Get-Date
do {
    Start-Sleep -Seconds 4
    powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "live_press.ps1") -Press "start:300" -Wait 1 | Out-Null
} while (-not (Select-String -Path $log -Pattern "mv_title\.bik" -Quiet) -and ((Get-Date) - $t0).TotalSeconds -lt 90)
Start-Sleep -Seconds 8
Get-ChildItem $live -Filter *.bmp | Remove-Item
New-Item -ItemType File -Force $flag | Out-Null
$t0 = Get-Date
do { Start-Sleep -Milliseconds 250; $n = @(Get-ChildItem $live -Filter *.bmp).Count }
while ($n -lt $Count -and ((Get-Date) - $t0).TotalSeconds -lt 120)
Remove-Item $flag
Start-Sleep -Milliseconds 500
& (Join-Path $PSScriptRoot "stop_test_game.ps1")
Remove-Item Env:OOGIE_FPS_CAP
Add-Type -AssemblyName System.Drawing
$files = Get-ChildItem $live -Filter *.bmp | Sort-Object Name
$first = $files[0].LastWriteTime
$out = foreach ($f in $files) {
    $b = [System.Drawing.Bitmap]::FromFile($f.FullName)
    # the PRESS START BUTTON line: x 200-440, y 365-385 of 640x480
    $sx = $b.Width / 640.0; $sy = $b.Height / 480.0
    $sum = 0; $k = 0
    for ($y = 365; $y -le 385; $y += 2) { for ($x = 200; $x -le 440; $x += 4) {
        $c = $b.GetPixel([int]($x * $sx), [int]($y * $sy)); $sum += ($c.R + $c.G + $c.B) / 3; $k++ } }
    $b.Dispose()
    "{0} {1:N0} {2:N1}" -f $f.Name, ($f.LastWriteTime - $first).TotalMilliseconds, ($sum / $k)
}
$out | Set-Content (Join-Path $Root "logs\$Name.timing.txt")
"frames: $($files.Count)"
