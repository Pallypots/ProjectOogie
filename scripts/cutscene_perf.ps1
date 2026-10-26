# Project Oogie: the New Game opening cutscene (dt00, after 0000.lz) under a
# settings file: slow frames and window fps from the level load until the
# hand-over to gameplay (rsd_3mhd.lz). For "the cutscene skips audio and text".
# Usage: powershell -ExecutionPolicy Bypass -File scripts\cutscene_perf.ps1 -Name cp1 -Settings logs\x.ini [-Extra "OOGIE_X=1"]
param([Parameter(Mandatory = $true)][string]$Name, [Parameter(Mandatory = $true)][string]$Settings, [string]$Extra = "")
$Root = Split-Path -Parent $PSScriptRoot
$env:OOGIE_TEST_SETTINGS = (Resolve-Path $Settings).Path
if ($Extra) { foreach ($kv in $Extra -split ';') { $p = $kv -split '=', 2; Set-Item "Env:$($p[0])" $p[1] } }
$r = & (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name $Name -Cap 60 -TimeoutSec 150 | Select-Object -Last 1
$env:OOGIE_TEST_SETTINGS = $null
if ($Extra) { foreach ($kv in $Extra -split ';') { $p = $kv -split '=', 2; Set-Item "Env:$($p[0])" $null } }
$l = Get-Content (Join-Path $Root "logs\$Name.err.log")
$a = ($l | Select-String 'FILE\] d:\\stm_data\\0000\.lz' | Select-Object -First 1).LineNumber
$b = ($l | Select-String 'rsd_3mhd\.lz' | Select-Object -First 1).LineNumber
if (-not $b) { $b = $l.Count }
$seg = if ($a) { $l[$a..($b - 1)] } else { @() }
$h = @($seg | Select-String '\[HITCH\] f=\d+ ([0-9.]+)' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
"{0}: {1} | cutscene slow frames {2} (mean {3:N0} ms)" -f $Name, $r, $h.Count, $(if ($h.Count) { ($h | Measure-Object -Average).Average } else { 0 })
