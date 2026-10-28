#!/bin/bash
# Install the official DXVK release (github.com/doitsujin/dxvk) into the Wine
# test prefix: its 64-bit d3d11/dxgi/d3d10core DLLs, set to "native" so Wine
# uses them, as Proton does. Usage: wine_dxvk_official.sh <version, e.g. 3.1.1>
set -e
V="$1"
export WINEPREFIX="$HOME/.wine_oogie"
mkdir -p "$HOME/dxvk"; cd "$HOME/dxvk"
[ -f "dxvk-$V.tar.gz" ] || curl -sSLo "dxvk-$V.tar.gz" "https://github.com/doitsujin/dxvk/releases/download/v$V/dxvk-$V.tar.gz"
ls -la "dxvk-$V.tar.gz" | awk '{print $5, $9}'
rm -rf "dxvk-$V"; tar xzf "dxvk-$V.tar.gz"
for d in d3d11 dxgi d3d10core; do
    cp "dxvk-$V/x64/$d.dll" "$WINEPREFIX/drive_c/windows/system32/$d.dll"
    wine reg add 'HKCU\Software\Wine\DllOverrides' /v "$d" /d native /f >/dev/null 2>&1
done
wine reg query 'HKCU\Software\Wine\DllOverrides' 2>/dev/null | grep -iE 'd3d11|dxgi|d3d10core'
