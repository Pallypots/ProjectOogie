# Project Oogie: fetch SDL2 (zlib licence) for PlayStation / other pad support.
# Puts the official SDL2 2.30.9 Visual C++ development package in
# third_party\SDL2 (git-ignored) and checks its SHA-256. regen.ps1 and the
# CMake build copy lib\x64\SDL2.dll beside oogie_recomp.exe; the game loads
# it at run time (port/src/pc_input.c) and runs without it (Xbox pads only).
$Root = Split-Path -Parent $PSScriptRoot
$Url  = "https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-VC.zip"
$Sha  = "8c91d91e5bcb997d062ec2b553c53832ebf95654d4aa35e8c02a954d4ce752ae"
$Dest = Join-Path $Root "third_party\SDL2"
if (Test-Path (Join-Path $Dest "lib\x64\SDL2.dll")) { Write-Host "SDL2 already in $Dest"; exit 0 }
$Zip = Join-Path $env:TEMP "SDL2-devel-2.30.9-VC.zip"
Invoke-WebRequest -Uri $Url -OutFile $Zip -UseBasicParsing
$got = (Get-FileHash $Zip -Algorithm SHA256).Hash.ToLower()
if ($got -ne $Sha) { Remove-Item $Zip; throw "SDL2 download hash mismatch ($got)" }
$Tmp = Join-Path $env:TEMP "sdl2_extract"
Remove-Item $Tmp -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive $Zip -DestinationPath $Tmp
Move-Item (Join-Path $Tmp "SDL2-2.30.9") $Dest
Remove-Item $Tmp -Recurse -Force; Remove-Item $Zip
Write-Host "SDL2 2.30.9 in $Dest"
