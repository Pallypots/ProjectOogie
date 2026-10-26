# Project Oogie: boot to the title screen N times and check the title movie
# (owner, 2026-10-17: on one launch the title background and its music were
# missing). Per boot: did the game open mv_title.bik, and the mean brightness
# of a background patch of the title frame (the movie: ~40; missing: ~0).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\title_check.ps1 [-Runs 5]
param([int]$Runs = 5)
$Root = Split-Path -Parent $PSScriptRoot
$lp = Join-Path $PSScriptRoot "live_press.ps1"
Add-Type -AssemblyName System.Drawing
for ($r = 1; $r -le $Runs; $r++) {
    $name = "tc_title$r"
    $log = Join-Path $Root "logs\$name.err.log"
    & (Join-Path $PSScriptRoot "stop_test_game.ps1"); Start-Sleep -Seconds 1
    Remove-Item $log -ErrorAction SilentlyContinue
    & (Join-Path $PSScriptRoot "live_start.ps1") -Name $name -D3D | Out-Null
    $t0 = Get-Date
    while (-not ((Test-Path $log) -and (Select-String -Path $log -Pattern "mv_00\.bik" -Quiet)) -and ((Get-Date) - $t0).TotalSeconds -lt 90) { Start-Sleep -Milliseconds 300 }
    Start-Sleep -Seconds 3
    $t0 = Get-Date
    do {
        powershell -ExecutionPolicy Bypass -File $lp -Press "start:300" -Wait 4 | Out-Null
    } while (-not (Select-String -Path $log -Pattern "mv_title\.bik" -Quiet) -and ((Get-Date) - $t0).TotalSeconds -lt 60)
    $opened = Select-String -Path $log -Pattern "mv_title\.bik" -Quiet
    Start-Sleep -Seconds 8
    $o = powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "live_flips.ps1") -Count 1 -Tag $name -TimeoutSec 15 | Select-Object -First 1
    $bg = -1
    if ($o) {
        $b = [System.Drawing.Bitmap]::FromFile(($o -split ' ')[0])
        $sum = 0; $n = 0
        for ($y = 60; $y -lt 300; $y += 6) { for ($x = 20; $x -lt 160; $x += 6) { $c = $b.GetPixel($x, $y); $sum += ($c.R + $c.G + $c.B) / 3; $n++ } }
        $b.Dispose(); $bg = [math]::Round($sum / $n, 1)
    }
    "boot $r : mv_title opened=$opened background=$bg frame=$o"
}
& (Join-Path $PSScriptRoot "stop_test_game.ps1")
