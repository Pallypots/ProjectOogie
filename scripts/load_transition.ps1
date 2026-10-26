# Project Oogie: capture every frame of the loading transition (the orange
# shapes that sweep in, owner photo 2026-10-19) when a chapter starts from
# Chapter Select. Frames land in logs\live as <Name>_<n>.png with their mean
# brightness; a contact sheet is up to the caller.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\load_transition.ps1 -Name lt1 [-Seconds 10] [-Owner]
param([Parameter(Mandatory = $true)][string]$Name, [int]$Seconds = 10, [switch]$Owner)
$Root = Split-Path -Parent $PSScriptRoot
$live = Join-Path $Root "logs\live"
$pad = Join-Path $Root "logs\pad_live.txt"
$flag = Join-Path $Root "logs\flipdump.flag"
if ($Owner) { $env:OOGIE_TEST_SETTINGS = Join-Path $Root "logs\settings_owner.ini" }
& (Join-Path $PSScriptRoot "load_save.ps1") -Name $Name -StopAtSelect | Out-Null
Remove-Item Env:OOGIE_TEST_SETTINGS -ErrorAction SilentlyContinue
$before = @(Get-ChildItem $live -Filter *.bmp | ForEach-Object Name)
Add-Content -Path $pad -Value "a:200"; Start-Sleep -Seconds 4      # chapter -> "Start this chapter?" (Yes)
New-Item -ItemType File -Force $flag | Out-Null
Add-Content -Path $pad -Value "a:200"                              # Yes: the load starts
Start-Sleep -Seconds $Seconds
Remove-Item $flag -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
& (Join-Path $PSScriptRoot "stop_test_game.ps1")
$new = @(Get-ChildItem $live -Filter *.bmp | Where-Object { $before -notcontains $_.Name } | Sort-Object LastWriteTime)
Add-Type -AssemblyName System.Drawing
$i = 0
foreach ($f in $new) {
    $b = [System.Drawing.Bitmap]::FromFile($f.FullName)
    $out = Join-Path $live ("{0}_{1:D4}.png" -f $Name, $i)
    $b.Save($out, [System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
    Remove-Item $f.FullName
    $i++
}
"$Name : $i frames captured"
