# Project Oogie: where do busy moments go? For -Seconds, every second: find
# the test game's threads that used over half a core in the last second,
# sample each (tools\sampler, 1 s at 5 ms), and add up the functions seen.
# Prints the top functions over the whole window. Only the test game (pid in
# logs\test_pids.txt), never the owner's.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\busy_profile.ps1 -Seconds 90 [-Out logs\bp.txt]
param([int]$Seconds = 90, [string]$Out = "")
$Root = Split-Path -Parent $PSScriptRoot
$sampler = Join-Path $Root "tools\sampler\sampler.exe"
$pidFile = Join-Path $Root "logs\test_pids.txt"
$t0 = Get-Date
$p = $null
while (-not $p -and ((Get-Date) - $t0).TotalSeconds -lt 60) {
    $tp = Get-Content $pidFile -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($tp) { $p = Get-Process -Id $tp -ErrorAction SilentlyContinue }
    if (-not $p) { Start-Sleep -Milliseconds 500 }
}
if (-not $p) { "no test game"; exit 1 }
$hits = @{}; $total = 0; $windows = 0
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end -and -not $p.HasExited) {
    $p.Refresh()
    $a = @{}; foreach ($t in $p.Threads) { try { $a[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
    Start-Sleep -Milliseconds 1000
    $p.Refresh()
    $busy = @()
    foreach ($t in $p.Threads) { try { if ($a.ContainsKey($t.Id) -and $t.TotalProcessorTime.TotalMilliseconds - $a[$t.Id] -gt 500) { $busy += $t.Id } } catch {} }
    if (-not $busy) { continue }
    $windows++
    $jobs = foreach ($tid in $busy) { Start-Job -ScriptBlock { param($s, $pp, $tt) & $s $pp $tt 200 5 2>&1 } -ArgumentList $sampler, $p.Id, $tid }
    foreach ($j in $jobs) {
        foreach ($ln in (Receive-Job $j -Wait)) {
            if ("$ln" -match '^\s*([0-9.]+)%\s+(\S+)') {
                $n = [double]$Matches[1] * 2; $f = $Matches[2]
                $hits[$f] = $hits[$f] + $n; $total += $n
            }
        }
        Remove-Job $j
    }
}
$rep = @("busy windows: $windows, samples: $total")
$rep += $hits.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 30 | ForEach-Object { "{0,6:N1}%  {1}" -f (100.0 * $_.Value / [Math]::Max(1, $total)), $_.Key }
if ($Out) { $rep | Set-Content (Join-Path $Root $Out) }
$rep
