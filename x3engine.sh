#!/usr/bin/env bash
# x3engine.sh — INNER launcher, runs INSIDE the pressure-vessel container
# (invoked in place of testandlaunch by x3run.sh).
#
# Skips X3's flaky GL probe (test_txc) and the GTK launcher, starting the engine
# directly with the x3ui overlay preloaded. LD_PRELOAD is set ONLY here, so our
# .so loads into the engine alone — not the probe, not pressure-vessel tools.

GAME="/home/fabric/.local/share/Steam/steamapps/common/X3 Terran Conflict"
cd "$GAME" || { echo "[x3engine] cannot cd to game dir" >&2; exit 1; }

export GTK2_RC_FILES=config.gtkrc
export LD_PRELOAD="/home/fabric/x3ui/libx3ui.so${LD_PRELOAD:+:$LD_PRELOAD}"

# Terran Conflict = appid 2820, Albion Prelude = 201310.
case "$SteamAppId" in
    2820) ENGINE=X3TC_main ;;
    *)    ENGINE=X3AP_main ;;
esac
[ -x "./$ENGINE" ] || ENGINE=X3TC_main   # last-ditch fallback

# --- x3ui auto-load (optional) -------------------------------------------------
# x3launch.sh writes x3autoload.env; if X3_FASTSTART_SLOT is a save slot number,
# boot straight into save/X<NN>.sav (skip intro + main menu) so the debug socket
# comes up in flight. Empty/absent => normal boot to menu. This file is read here
# (inside the pressure-vessel container) because host env doesn't reliably cross
# the Steam -> reaper -> pressure-vessel boundary.
ARGS=()
[ -f /home/fabric/x3ui/x3autoload.env ] && . /home/fabric/x3ui/x3autoload.env
if [ -n "${X3_FASTSTART_SLOT:-}" ]; then
    ARGS+=(-skipintro -faststart "$X3_FASTSTART_SLOT")
fi

echo "[x3engine] launching $ENGINE (AppId=$SteamAppId) overlay args: ${ARGS[*]:-(none)}" >&2
exec "./$ENGINE" "${ARGS[@]}"
