# Project Oogie 0.2: A/B on a simulated weak PC. Starts Chapter -Chapter on
# -Mask processors with -Settings, waits -Settle s in the level, then reads
# the window's fps once a second for -Seconds and the busiest threads' CPU.
# -Env "NAME=value" sets an extra switch (the B side).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\weak_pc_ab.ps1 -Name ab1 [-Env OOGIE_VBLANK_SPIN=1]
param([Parameter(Mandatory = $true)][string]$Name, [int]$Chapter = 17, [string]$Level = '0503',
      [string]$Mask = '0x30', [string]$Settings = 'set_low', [int]$Settle = 40, [int]$Seconds = 40, [string]$Env = '')
$Root = Split-Path -Parent $PSScriptRoot
$j = Start-Job -ScriptBlock { param($r, $n, $c, $l, $m, $s, $e)
    Set-Location $r
    $env:OOGIE_START_CHAPTER = "$c"; $env:OOGIE_TEST_AFFINITY = $m
    $env:OOGIE_TEST_SETTINGS = (Resolve-Path "logs\$s.ini").Path
    if ($e) { $k, $v = $e -split '=', 2; Set-Item "Env:$k" $v }
    powershell -ExecutionPolicy Bypass -File scripts\timing_cutscene.ps1 -Name $n -Cap 60 -Keep -TimeoutSec 5 -LevelFile ("stm_data\\" + $l + "\.lz") -Marker no_marker_xyz | Out-Null
} -ArgumentList $Root, $Name, $Chapter, $Level, $Mask, $Settings, $Env
$log = Join-Path $Root "logs\$Name.err.log"
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt 300) {
    if ((Test-Path $log) -and (Select-String $log -Pattern ("stm_data\\" + $Level) -Quiet)) { break }
    Start-Sleep 3
}
Start-Sleep $Settle
$p = Get-Process -Id (Get-Content (Join-Path $Root "logs\test_pids.txt") | Select-Object -Last 1)
$a = @{}; $p.Threads | ForEach-Object { $a[$_.Id] = $_.TotalProcessorTime.TotalMilliseconds }
$fps = @()
for ($i = 0; $i -lt $Seconds; $i++) {
    Start-Sleep 1; $p.Refresh()
    if ($p.MainWindowTitle -match '\| ([0-9.]+) fps') { $fps += [double]$Matches[1] }
}
$p.Refresh()
$top = $p.Threads | ForEach-Object { [int](($_.TotalProcessorTime.TotalMilliseconds - $a[$_.Id]) / $Seconds * 10) } | Sort-Object -Descending | Select-Object -First 5
$s = $fps | Sort-Object
"{0}: fps mean {1:N1}, median {2:N1}, min {3:N1} ({4} samples); busiest threads, ms per 10 s: {5}; extra: {6}" -f $Name, ($fps | Measure-Object -Average).Average, $s[[int]($s.Count / 2)], $s[0], $fps.Count, ($top -join ' '), $Env
& (Join-Path $PSScriptRoot "stop_test_game.ps1") | Out-Null
Get-Job | Remove-Job -Force
