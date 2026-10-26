# Project Oogie: run soak_hits.ps1 -Times times and print one summary line per
# run: survived, continues, hitches (all / over 100 ms / worst), long stalls
# (no frame for 15 s), GPU safety-net actions, crashes.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\soak_report.ps1 -Prefix sk61 [-Times 2] [-Minutes 10]
param([Parameter(Mandatory = $true)][string]$Prefix, [int]$Times = 2, [int]$Minutes = 10)
$Root = Split-Path -Parent $PSScriptRoot
for ($k = 1; $k -le $Times; $k++) {
    $n = "$Prefix$k"
    $r = powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "soak_hits.ps1") -Name $n -Minutes $Minutes 2>&1 | Select-Object -Last 1
    $l = Get-Content (Join-Path $Root "logs\$n.err.log")
    $h = @($l | Where-Object { $_ -match '^\[HITCH\] f=\d+ ([\d.]+) ms' } | ForEach-Object { [double]($_ -replace '^\[HITCH\] f=\d+ ([\d.]+) ms.*', '$1') })
    $worst = if ($h.Count) { [math]::Round(($h | Measure-Object -Maximum).Maximum) } else { 0 }
    "{0} | hitches {1}, >100ms {2}, worst {3} | 15s-stalls {4} | net {5} | crashes {6}" -f $r, $h.Count, @($h | Where-Object { $_ -gt 100 }).Count, $worst,
        @($l | Where-Object { $_ -match 'still no frame after 15' }).Count,
        @($l | Where-Object { $_ -match '^\[GPU\]   |commands not handed over' }).Count,
        @($l | Where-Object { $_ -match '^\[CRASH\]' }).Count
}
