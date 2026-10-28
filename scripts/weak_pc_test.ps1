# Project Oogie 0.2: how the port runs on a weak PC. Each case runs one
# chapter (chapter_sweep.ps1) with a settings file and, optionally, the whole
# program limited to some logical processors (OOGIE_TEST_AFFINITY; 0x30 = two
# of the Core Ultra 7 268V's low-power cores, as a stand-in for a two-core
# PC). Prints mean fps, slow frames and the game's CPU time per case.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\weak_pc_test.ps1 -Chapter 17 -Minutes 2 -Tag wk1
param([int]$Chapter = 17, [int]$Minutes = 2, [string]$Tag = "wk",
      [string[]]$Cases = @("all:set_low", "0x30:set_low", "0x30:set_high"))
$Root = Split-Path -Parent $PSScriptRoot
$out = @()
foreach ($c in $Cases) {
    $aff, $set = $c -split ':'
    $name = "{0}_{1}_{2}" -f $Tag, ($aff -replace '0x', 'm'), $set
    $env:OOGIE_TEST_SETTINGS = (Resolve-Path (Join-Path $Root "logs\$set.ini")).Path
    if ($aff -ne 'all') { $env:OOGIE_TEST_AFFINITY = $aff } else { Remove-Item Env:OOGIE_TEST_AFFINITY -ErrorAction SilentlyContinue }
    Remove-Item (Join-Path $Root "logs\${name}_summary.txt") -ErrorAction SilentlyContinue
    powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "chapter_sweep.ps1") -First $Chapter -Last $Chapter -Tag $name -Minutes $Minutes | Out-Null
    $env:OOGIE_TEST_SETTINGS = $null; Remove-Item Env:OOGIE_TEST_AFFINITY -ErrorAction SilentlyContinue
    $log = Join-Path $Root ("logs\{0}_ch{1:D2}.err.log" -f $name, $Chapter)
    $sum = Get-Content (Join-Path $Root "logs\${name}_summary.txt") -ErrorAction SilentlyContinue
    $fps = if ($sum -match 'mean fps ([0-9.]+)') { $Matches[1] } else { '?' }
    $h = @(Select-String $log -Pattern '\[HITCH\] f=\d+ ([0-9.]+) ms' -ErrorAction SilentlyContinue | % { [double]$_.Matches[0].Groups[1].Value })
    $held = @(Select-String $log -Pattern 'CPU held (\d+) ms' -ErrorAction SilentlyContinue | % { [int]$_.Matches[0].Groups[1].Value })
    $aff_ok = [bool](Select-String $log -Pattern '\[TEST\] process affinity' -Quiet -ErrorAction SilentlyContinue)
    $out += "{0,-26} fps {1,5}  slow frames {2,4} (over 50 ms {3,3})  game CPU per 10 s {4,5:N0} ms  affinity set: {5}" -f $c, $fps, $h.Count, @($h | ? { $_ -gt 50 }).Count, ($held | Measure-Object -Average).Average, $aff_ok
}
$out
