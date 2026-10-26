# Project Oogie: run bin\oogie_recomp.exe with the verified settings and capture its log.
# Usage: powershell -ExecutionPolicy Bypass -File C:\ProjectOogie\scripts\run_capture.ps1 -Name win_run3 [-Seconds 120] [-Watchdog 100]
# Writes logs\<Name>.err.log (game/runtime log) and logs\<Name>.out.log (console banner).
param([Parameter(Mandatory = $true)][string]$Name, [int]$Seconds = 130, [int]$Watchdog = 100,
      [switch]$IcallStop, [switch]$FbDump, [int]$Sample = 0,
      [string]$PadPress = "", [string]$PadScript = "")
$Root = Split-Path -Parent $PSScriptRoot
$env:RECOMP_PB_EXEC = "1"
$env:RECOMP_AC97_READY = "1"
$env:RECOMP_FB_WINDOW = "1"
$env:RECOMP_MISSING_FILES = "1"
$env:RECOMP_USB = "1"            # emulated USB controller (host XInput pad / keyboard / scripted)
$env:RECOMP_WATCHDOG_SECS = "$Watchdog"
if ($IcallStop) { $env:OOGIE_ICALL_STOP = "1" } else { Remove-Item Env:OOGIE_ICALL_STOP -ErrorAction SilentlyContinue }
if ($FbDump) { $env:RECOMP_FB_DUMP = "1" } else { Remove-Item Env:RECOMP_FB_DUMP -ErrorAction SilentlyContinue }
if ($Sample -gt 0) { $env:OOGIE_SAMPLE_SECS = "$Sample" } else { Remove-Item Env:OOGIE_SAMPLE_SECS -ErrorAction SilentlyContinue }
# RECOMP_PAD_PRESS=mask[,period_ms[,hold_ms]] pulses buttons (0x10 = Start);
# RECOMP_PAD_SCRIPT="ms:button,..." presses in order (see usb_gamepad.c).
if ($PadPress) { $env:RECOMP_PAD_PRESS = $PadPress } else { Remove-Item Env:RECOMP_PAD_PRESS -ErrorAction SilentlyContinue }
if ($PadScript) { $env:RECOMP_PAD_SCRIPT = $PadScript } else { Remove-Item Env:RECOMP_PAD_SCRIPT -ErrorAction SilentlyContinue }
$err = Join-Path $Root "logs\$Name.err.log"
$out = Join-Path $Root "logs\$Name.out.log"
$p = Start-Process -FilePath (Join-Path $Root "bin\oogie_recomp.exe") -WorkingDirectory $Root `
     -RedirectStandardError $err -RedirectStandardOutput $out -PassThru
if (-not $p.WaitForExit($Seconds * 1000)) { $p.Kill(); Write-Host "stopped after $Seconds s" }
else { Write-Host "exited with code $($p.ExitCode)" }
