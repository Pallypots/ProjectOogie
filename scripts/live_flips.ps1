# Project Oogie: capture the next N presented (finished) frames of a live session as PNGs.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\live_flips.ps1 [-Count 3] [-Tag name] [-TimeoutSec 120]
param([int]$Count = 3, [string]$Tag = "flip", [int]$TimeoutSec = 120)
$Root = Split-Path -Parent $PSScriptRoot
$live = Join-Path $Root "logs\live"
$flag = Join-Path $Root "logs\flipdump.flag"
$before = @(Get-ChildItem $live -Filter *.bmp | ForEach-Object Name)
New-Item -ItemType File -Force $flag | Out-Null
$t0 = Get-Date
do {
    Start-Sleep -Milliseconds 500
    $new = @(Get-ChildItem $live -Filter *.bmp | Where-Object { $before -notcontains $_.Name })
} while ($new.Count -lt $Count -and ((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec)
Remove-Item $flag -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300
Add-Type -AssemblyName System.Drawing
$i = 0
foreach ($f in ($new | Sort-Object LastWriteTime)) {
    $b = [System.Drawing.Bitmap]::FromFile($f.FullName)
    $sum = 0; $n = 0
    for ($y = 0; $y -lt $b.Height; $y += 16) { for ($x = 0; $x -lt $b.Width; $x += 16) { $c = $b.GetPixel($x, $y); $sum += ($c.R + $c.G + $c.B) / 3; $n++ } }
    $out = Join-Path $live ("{0}_{1}.png" -f $Tag, $i)
    $b.Save($out, [System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
    "{0} {1} mean={2:N1}" -f $out, $f.Name, ($sum / $n)
    $i++
}
