# Project Oogie: start the game exactly as play.bat does (same environment),
# but with a copy of the owner's settings and saves, for -Seconds, then count
# the flicker detector's reports. For "it only flickers when I play".
# Usage: powershell -ExecutionPolicy Bypass -File scripts\boot_like_play.ps1 -Name bp1 [-Seconds 25] [-Extra "OOGIE_X=1"]
param([Parameter(Mandatory = $true)][string]$Name, [int]$Seconds = 25, [string]$Extra = "")
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root
& (Join-Path $PSScriptRoot "owner_game_check.ps1")
if ($LASTEXITCODE -ne 0) { "the owner's game is running: not starting $Name"; exit 1 }
$env:RECOMP_PB_EXEC = "1"; $env:RECOMP_AC97_READY = "1"; $env:RECOMP_MISSING_FILES = "1"
$env:RECOMP_USB = "1"; $env:RECOMP_KEYBOARD = "1"; $env:RECOMP_HEAP_RECLAIM = "1"
$env:RECOMP_SAVE_DIR = Join-Path $Root "saves_ownercopy"       # a copy, never saves\
$env:OOGIE_SETTINGS = Join-Path $Root "logs\settings_owner_now.ini"
$env:OOGIE_D3D11 = "1"; $env:OOGIE_QUIET_GPU = "1"; $env:RECOMP_VBLANK = "1"
foreach ($v in 'RECOMP_PB_REPORT_MS','OOGIE_SHOT_PREFIX','OOGIE_SHOT_FLAG','RECOMP_PAD_LIVE','RECOMP_FB_WINDOW','RECOMP_FB_DUMP','RECOMP_FB_DUMP_FLIPS') { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
if ($Extra) { foreach ($kv in $Extra -split ';') { $p = $kv -split '=', 2; Set-Item "Env:$($p[0])" $p[1] } }
$log = Join-Path $Root "logs\$Name.log"
$p = Start-Process -FilePath (Join-Path $Root "bin\oogie_recomp.exe") -WorkingDirectory $Root -RedirectStandardError $log -RedirectStandardOutput (Join-Path $Root "logs\$Name.out.log") -PassThru
Add-Content -Path (Join-Path $Root "logs\test_pids.txt") -Value $p.Id
Start-Sleep -Seconds $Seconds
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
$l = Get-Content $log
$bad = @($l | Where-Object { $_ -match 'cleared after .* SHOWN BROKEN' }).Count
$fr = @($l | Where-Object { $_ -match 'FLICKER\] f=\d+ frame draws' }).Count
"{0}: {1} s: {2} frames shown after their clear, {3} sparse-frame reports" -f $Name, $Seconds, $bad, $fr
