#!/usr/bin/env bash
# x3launch.sh — relaunch X3: Albion Prelude via Steam with the x3ui overlay,
# auto-loading a save so the game (and the debug socket) comes up in flight
# without hand-clicking through the menu each dev cycle.
#
#   ./x3launch.sh            auto-load the newest save slot
#   ./x3launch.sh 13         auto-load save/X13.sav  (slot number)
#   ./x3launch.sh menu       boot to the main menu (overlay on, no auto-load)
#
# Mechanism: writes the chosen slot into x3autoload.env, then asks Steam to
# launch the game. Steam runs the configured launch option
#     /home/fabric/x3ui/x3run.sh %command%
# which routes through x3engine.sh, where -skipintro -faststart <slot> are
# applied to the engine exec. (-faststart loads save\X%02d.sav by slot number,
# verified in engine_main_bootstrap_lifecycle.)
set -u

APPID=201310                                   # X3: Albion Prelude (X3TC = 2820)
REPO=/home/fabric/x3ui
SAVEDIR="$HOME/.config/EgoSoft/X3AP/save"
conf="$REPO/x3autoload.env"

arg="${1:-}"
if [ "$arg" = menu ] || [ "$arg" = none ]; then
    printf 'X3_FASTSTART_SLOT=\n' > "$conf"
    echo "[x3launch] config: boot to main menu (overlay on, no auto-load)"
else
    slot="$arg"
    if [ -z "$slot" ]; then
        newest=$(ls -t "$SAVEDIR"/X*.sav 2>/dev/null | head -1)
        slot=$(basename "${newest:-X13.sav}" .sav); slot=${slot#X}
    fi
    slot=$((10#$slot)) 2>/dev/null || { echo "[x3launch] slot must be a number, got '$arg'" >&2; exit 1; }
    savefile=$(printf '%s/X%02d.sav' "$SAVEDIR" "$slot")
    [ -f "$savefile" ] || echo "[x3launch] warning: $savefile not found (will fall back to menu)" >&2
    printf 'X3_FASTSTART_SLOT=%s\n' "$slot" > "$conf"
    echo "[x3launch] config: auto-load slot $slot ($savefile)"
fi

# Only guard against a duplicate Albion Prelude — NEVER X3TC_main: the user plays
# X3: Terran Conflict on the same machine, concurrently with AP dev, and it must
# never be disturbed. (Steam permits AP + TC to run side by side.)
if pgrep -x X3AP_main >/dev/null; then
    echo "[x3launch] X3AP is still running — quit it in-game first, then re-run this." >&2
    exit 1
fi

# Keep X3's "Run in background" ON so the sim doesn't pause when the window is unfocused.
printf '1' > "$HOME/.config/EgoSoft/X3AP/RunInBackground" 2>/dev/null || true

echo "[x3launch] launching: steam -applaunch $APPID"
exec steam -applaunch "$APPID"
