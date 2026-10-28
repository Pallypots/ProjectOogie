# Project Oogie: build the player download in dist\ (local only, nothing is uploaded).
#   dist\ProjectOogie\Project Oogie.exe   the game (bin\oogie_recomp.exe)
#   dist\ProjectOogie\oogie_recomp.pdb    symbols, so a crash log names the function
#   dist\ProjectOogie\SDL2.dll            PlayStation / Switch / other pads
#   dist\ProjectOogie\assets\fonts\       the settings menu's font (Apache 2.0)
#   dist\ProjectOogie\README.txt, LICENSE.txt, THIRD-PARTY-NOTICES.md
#   dist\ProjectOogie-<date>.zip
# No game files: the program copies them from the player's disc image on the first start.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\package_release.ps1
param([string]$Version = "")   # e.g. v0.1: the zip's name
$Root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $Root "dist\ProjectOogie"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $out "assets\fonts") | Out-Null
Copy-Item (Join-Path $Root "bin\oogie_recomp.exe") (Join-Path $out "Project Oogie.exe")
$pdb = Get-ChildItem (Join-Path $Root "port\build\Release") -Filter "oogie_recomp*.pdb" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($pdb) { Copy-Item $pdb.FullName (Join-Path $out "oogie_recomp.pdb") }
Copy-Item (Join-Path $Root "bin\SDL2.dll") $out
# Microsoft's HLSL compiler (Windows SDK redistributable): used instead of
# Wine's under Proton / Steam Deck (port/src/pc_d3dc.c)
$d3dc = Get-Item (Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Redist\D3D\x64\d3dcompiler_47.dll") -ErrorAction SilentlyContinue
if (-not $d3dc) { throw "d3dcompiler_47.dll not found in the Windows SDK (Windows Kits\10\Redist\D3D\x64)" }
Copy-Item $d3dc.FullName $out
Copy-Item (Join-Path $Root "assets\fonts\*") (Join-Path $out "assets\fonts")
Copy-Item (Join-Path $Root "LICENSE") (Join-Path $out "LICENSE.txt")
Copy-Item (Join-Path $Root "THIRD-PARTY-NOTICES.md") $out
@"
PROJECT OOGIE - The Nightmare Before Christmas: Oogie's Revenge for Windows
(an unofficial port of the original Xbox game; not affiliated with Capcom or Disney)

YOU NEED YOUR OWN COPY OF THE GAME: the Xbox version, USA, as a disc image (.iso).
The program contains the game's code translated to native code; the game's art,
sound, films and levels are not included, they come from your disc.

FIRST START
  Run "Project Oogie.exe". It asks for your disc image, checks it is the supported
  version, and copies the game files into the "game" folder here (about 3.5 GB, once).
  Your disc image is only read, never changed.

PLAYING
  F1 (or Back+Start on a pad)   settings: resolution, widescreen, graphics, sound,
                                controls, mods
  F11 or Alt+Enter              fullscreen / window
  F12                           screenshot (saved in "screenshots")
  Keyboard: arrow keys = A B X Y (Down A, Right B, Left X, Up Y), Enter = Start,
            W A S D = walk, I J K L = camera, Backspace = Back (map),
            T F G H = D-pad, 1 / 3 = triggers, Q / E = White / Black.
  Xbox controllers and the PlayStation 5 DualSense are tested; other pads SDL2
  supports (PlayStation 4 and more) should work. Every key and button can be
  changed in the settings.

FILES
  saves\         your saves (backed up to saves_backup\ at every start, ten kept)
  settings.ini   your settings
  mods\          mods and texture packs (F1 > Mods explains)
  logs\play.log  the last session's log; please attach it when reporting a problem
"@ | Set-Content (Join-Path $out "README.txt") -Encoding ascii
$zip = Join-Path $Root ("dist\ProjectOogie-" + $(if ($Version) { $Version } else { Get-Date -Format "yyyyMMdd" }) + ".zip")
if (Test-Path $zip) { Remove-Item $zip -Force }
# Not Compress-Archive: Windows PowerShell 5.1 stores paths with backslashes
# ("ProjectOogie\Project Oogie.exe"), which Linux and SteamOS unzip as single
# files with backslashes in their names, so under Proton nothing could start
# (GitHub issue #1, Steam Machine). Windows own tar writes standard "/" paths.
Push-Location (Join-Path $Root "dist")
& (Join-Path $env:SystemRoot "System32\tar.exe") -a -c -f $zip ProjectOogie
Pop-Location
$files = Get-ChildItem $out -Recurse -File
"{0} files, {1:N1} MB unpacked; {2} ({3:N1} MB)" -f $files.Count, (($files | Measure-Object Length -Sum).Sum / 1MB), $zip, ((Get-Item $zip).Length / 1MB)
