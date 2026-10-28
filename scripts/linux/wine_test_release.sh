#!/bin/bash
# Project Oogie: run a release zip under Wine (in WSL2 with WSLg), as a
# stand-in for Proton on Steam Deck / SteamOS. Logs to ~/oogie_wine/<name>/.
# Usage: wine_test_release.sh <zip on /mnt/c> <name> <seconds> [setup|linkgame] [iso path]
set -u
ZIP="$1"; NAME="$2"; SECS="${3:-90}"; MODE="${4:-linkgame}"; ISO="${5:-}"
export DISPLAY="${DISPLAY:-:0}" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/mnt/wslg/runtime-dir}"
export WINEPREFIX="$HOME/.wine_oogie" WINEDEBUG="${WINEDEBUG:-err+all,warn+module,warn+seh}"
D="$HOME/oogie_wine/$NAME"
rm -rf "$D"; mkdir -p "$D"
python3 -m zipfile -e "$ZIP" "$D" >/dev/null
APP="$D/ProjectOogie"
if [ "$MODE" = linkgame ]; then ln -s /mnt/c/ProjectOogie/game "$APP/game"; fi
# SKIP_INTRO=1: a settings file with Skip Intro Logos on (reach the title without presses)
if [ "${SKIP_INTRO:-0}" = 1 ]; then printf '[Display]\r\nskip_intro = 1\r\n' > "$APP/settings.ini"; fi
[ -d "$WINEPREFIX" ] || wineboot -i >/dev/null 2>&1
cd "$APP"
# (a plain link name: winepath garbles long names with quotes and two extensions)
if [ "$MODE" = setup ] && [ -n "$ISO" ]; then ln -sf "$ISO" "$D/disc.iso"; export OOGIE_SETUP_FROM="$(winepath -w "$D/disc.iso")"; fi
# SHOT_AT=<s>: the picture is saved (shot_NNNNN.bmp) for about a second from then
if [ -n "${SHOT_AT:-}" ]; then
    export OOGIE_SHOT_PREFIX="shot_" OOGIE_SHOT_FLAG="shot.flag"
    ( sleep "$SHOT_AT"; touch "$APP/shot.flag"; sleep 1; rm -f "$APP/shot.flag" ) &
fi
timeout -k 5 "$SECS" wine "Project Oogie.exe" > "$D/wine_stdout.txt" 2> "$D/wine_stderr.txt"
echo "exit code $?"
ls "$APP"
echo "--- logs/play.log (tail)"; tail -n 25 "$APP/logs/play.log" 2>/dev/null || echo "(no play.log)"
echo "--- wine stderr (errors)"; grep -E "err:|seh|exception|Unhandled|page fault" "$D/wine_stderr.txt" | head -40
