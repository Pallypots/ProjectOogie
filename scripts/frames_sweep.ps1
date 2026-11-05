# Project Oogie: visual sweep. Every chapter started in turn (scripts\chapter_frames.ps1), a game
# frame saved every -Every seconds for -Seconds, pressing A now and then so dialogue moves on,
# and a contact sheet per chapter: logs\<Tag>_chNN\sheet.png. For checking graphics changes
# (culling, clipping, render targets) across the whole game by eye.
#   powershell -ExecutionPolicy Bypass -File scripts\frames_sweep.ps1 [-First 1] [-Last 24] [-Seconds 100] [-Every 5] [-Tag fs]
param([int]$First = 1, [int]$Last = 24, [int]$Seconds = 100, [double]$Every = 5, [string]$Tag = "fs")
$Root = Split-Path -Parent $PSScriptRoot
for ($ch = $First; $ch -le $Last; $ch++) {
    $name = "{0}_ch{1:D2}" -f $Tag, $ch
    & (Join-Path $PSScriptRoot "chapter_frames.ps1") -Chapter $ch -Name $name -Seconds $Seconds -Every $Every -Press "a:120" | Out-Null
    py -3 (Join-Path $PSScriptRoot "contact_sheet.py") (Join-Path $Root "logs\$name") 5 256 | Out-Null
    $log = Join-Path $Root "logs\$name.err.log"
    $crash = if (Test-Path $log) { (Select-String -Path $log -Pattern 'EXCEPTION|crash report|\[STALL\]' -SimpleMatch:$false).Count } else { -1 }
    "{0} chapter {1}: frames {2}, crash/stall lines {3}" -f (Get-Date -Format T), $ch, (Get-ChildItem (Join-Path $Root "logs\$name") -Filter "t*.png").Count, $crash
}
