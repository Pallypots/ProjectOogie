@echo off
rem Project Oogie - Windows test launcher (development build, not a playable release)
rem Runs the recompiled game with the settings verified in the cloud test box,
rem shows the game's screen in a window, and writes a log for Claude to read.
cd /d "%~dp0"
if not exist logs mkdir logs
set RECOMP_PB_EXEC=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_WATCHDOG_SECS=120
set RECOMP_MISSING_FILES=1
set RECOMP_USB=1
set RECOMP_KEYBOARD=1
set RECOMP_HEAP_RECLAIM=1
echo Starting Oogie's Revenge (Project Oogie dev build). Close the window or wait 2 minutes.
echo Log: logs\windows_run.log
bin\oogie_recomp.exe > logs\windows_run.log 2>&1
echo Exit code %ERRORLEVEL%
pause
