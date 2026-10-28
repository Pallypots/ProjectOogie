#!/bin/bash
# Can Linux (WSL) read the disc image whose path is in file $1?
P="$(cat "$1")"
echo "path: [$P]"
ls -la "$P" 2>&1 | cut -c1-120
head -c 16 "$P" 2>&1 | od -c | head -2
export WINEPREFIX="$HOME/.wine_oogie"
echo "wine path: [$(winepath -w "$P")]"
