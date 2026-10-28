#!/bin/bash
# Switch the Wine test prefix between DXVK (Direct3D 11 over Vulkan, as Proton
# on Steam Deck does) and Wine's own wined3d. Usage: wine_dxvk.sh on|off
export WINEPREFIX="$HOME/.wine_oogie"
if [ "$1" = on ]; then dxvk-setup install; else dxvk-setup uninstall; fi 2>&1 | tail -4
