# Project Oogie: one line per test log: flicker detector reports, D3D lock
# contention, frame-order waits that hit their cap, command-ring desyncs.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\flicker_summary.ps1 fp1 fp2 ...
$Root = Split-Path -Parent $PSScriptRoot
foreach ($n in $args) {
    $f = Join-Path $Root "logs\$n.err.log"
    if (-not (Test-Path $f)) { $f = Join-Path $Root "logs\$n.log" }
    $l = Get-Content $f
    $clr = @($l | Where-Object { $_ -match 'cleared after' }).Count
    $con = @($l | Select-String 'another game thread is in D3D.*, (\d+) so far' | ForEach-Object { [int]$_.Matches.Groups[1].Value })
    $cmax = if ($con.Count) { ($con | Measure-Object -Maximum).Maximum } else { 0 }
    $cap = @($l | Select-String 'frame overlay: a drawing task.*\((\d+) of (\d+) waits')
    $capl = if ($cap.Count) { "$($cap[-1].Matches.Groups[1].Value) of $($cap[-1].Matches.Groups[2].Value)" } else { "0" }
    $calls = @($l | Where-Object { $_ -match '\[PB\] CALL word' }).Count
    "{0}: cleared-after {1}, D3D contention {2}, frame-order caps {3}, ring desyncs {4}" -f $n, $clr, $cmax, $capl, $calls
}
