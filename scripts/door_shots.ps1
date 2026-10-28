# Project Oogie: catch the area-loading transition (#60, door.stm) on screen.
# Starts Chapter -Chapter with -Settings, waits until the game opens door.stm
# after the menus, then saves window screenshots every -Every ms for -Seconds
# to logs\live\<Name>_NNN.png (window_shot.ps1), and a contact sheet.
param([string]$Name = "door1", [int]$Chapter = 3, [string]$Level = '0100', [string]$Settings = 'settings_owner_0724',
      [int]$Seconds = 14, [int]$Every = 250)
$Root = Split-Path -Parent $PSScriptRoot
$log = Join-Path $Root "logs\$Name.err.log"
Remove-Item (Join-Path $Root "logs\live\${Name}_*.png") -ErrorAction SilentlyContinue
$j = Start-Job -ScriptBlock { param($r, $n, $c, $l, $s)
    Set-Location $r; $env:OOGIE_START_CHAPTER = "$c"
    $env:OOGIE_TEST_SETTINGS = (Resolve-Path "logs\$s.ini").Path
    powershell -ExecutionPolicy Bypass -File scripts\timing_cutscene.ps1 -Name $n -Cap 60 -Keep -TimeoutSec 5 -LevelFile ("stm_data\\" + $l + "\.lz") -Marker no_marker_xyz | Out-Null
} -ArgumentList $Root, $Name, $Chapter, $Level, $Settings
# door.stm is opened at boot too: wait for the difficulty film (mv_04) first, then for door.stm
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt 240 -and -not ((Test-Path $log) -and (Select-String $log -Pattern 'mv_04\.bik' -Quiet))) { Start-Sleep -Milliseconds 300 }
$n0 = @(Select-String $log -Pattern 'door\.stm').Count
while (((Get-Date) - $t0).TotalSeconds -lt 300 -and @(Select-String $log -Pattern 'door\.stm').Count -le $n0) { Start-Sleep -Milliseconds 100 }
"door.stm at $([int]((Get-Date) - $t0).TotalSeconds) s"
# a video of the window (trailer_capture.ps1, 60 fps), then frames from it
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "trailer_capture.ps1") -Name $Name -Seconds $Seconds -NoResize | Out-Null
& (Join-Path $PSScriptRoot "stop_test_game.ps1") | Out-Null; Get-Job | Remove-Job -Force
$ff = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
if (-not $ff) { $ff = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue | Select-Object -First 1).FullName }
$vid = Join-Path $Root "trailer\raw\$Name.mp4"
& $ff -hide_banner -loglevel error -y -i $vid -vf ("fps=1000/" + $Every + ",scale=320:-1,tile=8x7") -frames:v 1 (Join-Path $Root "logs\live\${Name}_sheet.png")
"sheet: logs\live\${Name}_sheet.png (one frame every $Every ms)"
