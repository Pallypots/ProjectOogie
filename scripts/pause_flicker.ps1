# Project Oogie: pause-menu flicker check (KNOWN_ISSUES #51/#57). Boots into
# Chapter 1 (timing_cutscene), opens the pause menu with START, dumps every
# shown frame for -Seconds, and reports how many are black (mean < 2) between
# normal ones. Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\pause_flicker.ps1 -Name pf1 [-Seconds 3]
param([Parameter(Mandatory = $true)][string]$Name, [int]$Seconds = 3)
$Root = Split-Path -Parent $PSScriptRoot
$live = Join-Path $Root "logs\live"
$pad = Join-Path $Root "logs\pad_live.txt"
$flag = Join-Path $Root "logs\flipdump.flag"
& (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name $Name -Cap 60 -Keep -TimeoutSec 200 | Out-Null
if (-not (Get-Process oogie_recomp -ErrorAction SilentlyContinue)) { "$Name : boot failed"; exit 1 }
Start-Sleep -Seconds 3
Add-Content -Path $pad -Value "start:200"
Start-Sleep -Seconds 3
$before = @(Get-ChildItem $live -Filter *.bmp | ForEach-Object Name)
New-Item -ItemType File -Force $flag | Out-Null
Start-Sleep -Seconds $Seconds
Remove-Item $flag -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
& (Join-Path $PSScriptRoot "stop_test_game.ps1")
$new = @(Get-ChildItem $live -Filter *.bmp | Where-Object { $before -notcontains $_.Name } | Sort-Object LastWriteTime)
Add-Type -AssemblyName System.Drawing
$black = 0; $means = @()
foreach ($f in $new) {
    $b = [System.Drawing.Bitmap]::FromFile($f.FullName)
    $s = 0; $n = 0
    for ($y = 10; $y -lt $b.Height; $y += 40) { for ($x = 10; $x -lt $b.Width; $x += 40) { $p = $b.GetPixel($x, $y); $s += $p.R + $p.G + $p.B; $n++ } }
    if ($means.Count -eq 0) { $b.Save((Join-Path $live "$Name`_first.png"), [System.Drawing.Imaging.ImageFormat]::Png) }
    $b.Dispose()
    $m = $s / $n / 3
    $means += $m
    if ($m -lt 2) { $black++ }
    Remove-Item $f.FullName
}
"{0}: {1} frames, {2} black, median brightness {3:N1}" -f $Name, $new.Count, $black, (($means | Sort-Object)[[int]($means.Count / 2)])
