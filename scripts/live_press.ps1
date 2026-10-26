# Project Oogie: send presses to a live session, wait, then save the newest frame as PNG.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\live_press.ps1 -Press "start" [-Wait 6] [-Out shot.png]
# -Press may hold several comma-separated presses, sent in order ("down,a").
param([string]$Press = "", [int]$Wait = 6, [string]$Out = "")
$Root = Split-Path -Parent $PSScriptRoot
$pad = Join-Path $Root "logs\pad_live.txt"
if ($Press) { foreach ($p in $Press.Split(',')) { Add-Content -Path $pad -Value $p.Trim() } }
Start-Sleep -Seconds $Wait
$f = Get-ChildItem (Join-Path $Root "logs\live") -Filter *.bmp | Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $f) { Write-Host "no frame yet"; exit }
if (-not $Out) { $Out = Join-Path $Root "logs\live\latest.png" }
Add-Type -AssemblyName System.Drawing
$b = [System.Drawing.Bitmap]::FromFile($f.FullName); $b.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
Write-Host "$($f.Name) -> $Out"
