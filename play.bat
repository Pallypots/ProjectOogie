@echo off
rem Project Oogie - interactive launcher (development build, not a finished port).
rem Uses the Direct3D 11 renderer.
rem Controllers: Xbox 360, Xbox One and Xbox Series pads (wired, wireless
rem adapter or Bluetooth), PlayStation 4 and 5 pads (DualShock 4, DualSense,
rem USB or Bluetooth), Switch Pro and most other pads. Connect it before or
rem while the game runs; it is picked up within a second. On a PlayStation
rem pad Cross = A, Circle = B, Square = X, Triangle = Y, L1/R1 = Black/White.
rem Keyboard (default layout):
rem   Arrow keys = the A B X Y buttons, laid out like the pad's diamond:
rem     Down = A (jump, confirm)   Right = B (back)   Left = X (Soul Robber)   Up = Y
rem   (Z X C V also work as A B X Y.)
rem   Enter = START   Backspace = BACK (map)
rem   W/A/S/D = left stick (walk)   I/J/K/L = right stick
rem   T/F/G/H = D-pad up/left/down/right (taunts)
rem   1 / 3 = left / right trigger   Q = White   E = Black
rem The keyboard only works while the game window has focus.
rem SETTINGS MENU: press F1 (or Back+Start, or the Guide / PS button on a pad)
rem at any time. It pauses the game and controls everything: fullscreen,
rem window size, 16:9 widescreen, render resolution, vsync, frame cap,
rem texture filtering, volume, rumble, and every key and pad button.
rem It saves to settings.ini next to this file.
rem Movies play in colour; press START to skip them.
rem Expect 60 fps in menus, movies and the first level (Town Square).
rem Close the window to quit. The log goes to logs\play.log.
rem For the old software renderer instead, run: play.bat cpu
cd /d "%~dp0"
if not exist logs mkdir logs
set RECOMP_PB_EXEC=1
set RECOMP_AC97_READY=1
set RECOMP_MISSING_FILES=1
set RECOMP_USB=1
set RECOMP_KEYBOARD=1
rem Reuse freed memory; without it a long session runs out (KNOWN_ISSUES #38).
set RECOMP_HEAP_RECLAIM=1
rem Saves live in the project folder (saves\UDATA), the same place whatever
rem starts the game: a program started from inside a packaged app such as the
rem Claude desktop app gets its own private copy of AppData, so saves made that
rem way were invisible here and the other way round (KNOWN_ISSUES #52). Saves
rem left in AppData by earlier builds are copied over when they are newer.
if not exist saves\UDATA mkdir saves\UDATA
if exist "%LOCALAPPDATA%\xboxrecomp\UDATA" robocopy "%LOCALAPPDATA%\xboxrecomp\UDATA" saves\UDATA /E /XO /NFL /NDL /NJH /NJS /NP >nul
set RECOMP_SAVE_DIR=%~dp0saves
rem Back up the saves before every session: saves_backup\<date_time>, the ten
rem newest kept. A crash in the middle of a save can then never cost progress.
rem To restore one, copy its UDATA folder over saves\UDATA with the game closed.
powershell -NoProfile -ExecutionPolicy Bypass -Command "if (Test-Path 'saves\UDATA') { $d = Join-Path 'saves_backup' (Get-Date -Format 'yyyyMMdd_HHmmss'); New-Item -ItemType Directory -Force $d | Out-Null; Copy-Item 'saves\UDATA' $d -Recurse; Get-ChildItem 'saves_backup' -Directory | Sort-Object Name -Descending | Select-Object -Skip 10 | Remove-Item -Recurse -Force }"
if /i "%1"=="cpu" (
    set RECOMP_FB_WINDOW=1
    set OOGIE_D3D11=
) else (
    set RECOMP_FB_WINDOW=
    set OOGIE_D3D11=1
    set OOGIE_QUIET_GPU=1
    set RECOMP_VBLANK=1
)
echo Starting Oogie's Revenge (Project Oogie dev build). Close the game window to quit.
echo Keys: arrows = A B X Y (Down A, Right B, Left X, Up Y), Enter = START, WASD = walk,
echo       T F G H = D-pad, Backspace = BACK/map, 1 / 3 = triggers, Q / E = White / Black.
echo Xbox and PlayStation controllers work too.
echo Press F1 in the game (or Back+Start on a pad) for the settings menu.
rem Keep the last three sessions' logs (play.log newest, then play.1.log, play.2.log).
if exist logs\play.1.log move /y logs\play.1.log logs\play.2.log >nul
if exist logs\play.log move /y logs\play.log logs\play.1.log >nul
echo Log: logs\play.log (previous sessions: play.1.log, play.2.log)
bin\oogie_recomp.exe > logs\play.log 2>&1
set OOGIE_EXIT=%ERRORLEVEL%
echo Exit code %OOGIE_EXIT%
rem A session that crashed (or logged a crash or a long freeze) is kept as a
rem report: reports\<date_time>\ with its log and the settings it ran with.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$bad = ($env:OOGIE_EXIT -ne '0') -or (Select-String -Path 'logs\play.log' -Pattern '^\[CRASH\]|still no frame after 15' -Quiet); if ($bad) { $d = Join-Path 'reports' (Get-Date -Format 'yyyyMMdd_HHmmss'); New-Item -ItemType Directory -Force $d | Out-Null; Copy-Item 'logs\play.log' $d; if (Test-Path 'settings.ini') { Copy-Item 'settings.ini' $d }; Write-Host ('Problem report saved in ' + $d) }"
pause
