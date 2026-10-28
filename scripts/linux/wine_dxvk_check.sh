#!/bin/bash
export WINEPREFIX="$HOME/.wine_oogie"
dxvk-setup install --symlink 2>&1 | tail -5
echo "--- overrides:"
wine reg query 'HKCU\Software\Wine\DllOverrides' 2>/dev/null | grep -iE 'd3d11|dxgi'
ls -la "$WINEPREFIX/drive_c/windows/system32/d3d11.dll" "$WINEPREFIX/drive_c/windows/system32/dxgi.dll" 2>&1 | cut -c1-160
