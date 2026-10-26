# Project Oogie: open and close the in-game inventory (pause) menu several
# times and measure each opening for flashing (owner report 2026-10-18: the
# second opening flashed and jittered). Run after load_save.ps1 has a chapter
# running. Prints per opening the frame brightness range and black frames.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\inv_cycle.ps1 [-Times 5]
param([int]$Times = 5, [string]$Tag = "inv")
$Root = Split-Path -Parent $PSScriptRoot
$pad = Join-Path $Root "logs\pad_live.txt"
for ($k = 1; $k -le $Times; $k++) {
    Add-Content -Path $pad -Value "start:200"
    Start-Sleep -Milliseconds 300
    $o = powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "live_flips.ps1") -Count 120 -Tag "$Tag$k" -TimeoutSec 30
    $m = @($o | ForEach-Object { [double](($_ -split 'mean=')[1]) })
    if ($m.Count) {
        $stable = $m | Select-Object -Last 60
        $min = ($stable | Measure-Object -Minimum).Minimum; $max = ($stable | Measure-Object -Maximum).Maximum
        $black = @($m | Where-Object { $_ -lt 2 }).Count
        "{0} opening {1}: frames {2}, last-60 brightness {3:N1}..{4:N1}, black frames {5}" -f $Tag, $k, $m.Count, $min, $max, $black
    } else { "{0} opening {1}: no frames" -f $Tag, $k }
    Add-Content -Path $pad -Value "start:200"
    Start-Sleep -Seconds 3
}
