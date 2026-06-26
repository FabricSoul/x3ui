#!/usr/bin/env bash
# x3run.sh — OUTER wrapper, runs on the HOST as Steam's launch command.
#
# Steam launch option:   /home/fabric/x3ui/x3run.sh %command%
#
# %command% expands to the full chain Steam would normally run, e.g.
#   .../reaper SteamLaunch AppId=201310 -- .../pressure-vessel-wrap ... -- \
#       .../X3\ Terran\ Conflict/testandlaunch X3AP_config
#
# X3's testandlaunch runs a GL probe (test_txc) that intermittently dbus-aborts
# on this NixOS + pressure-vessel setup, blocking launch. We keep the entire
# pressure-vessel prefix UNCHANGED (the 32-bit engine can only run inside that
# container) and replace just the trailing "testandlaunch <arg>" with our inner
# launcher, which starts the engine directly.

inner="/home/fabric/x3ui/x3engine.sh"

newargs=()
found=0
for a in "$@"; do
    case "$a" in
        */testandlaunch|testandlaunch)
            newargs+=("$inner")   # swap testandlaunch -> our inner launcher
            found=1
            break                 # drop testandlaunch's trailing engine arg too
            ;;
        *)
            newargs+=("$a")
            ;;
    esac
done

if [ "$found" = 1 ]; then
    exec "${newargs[@]}"
else
    # Structure not as expected — don't break anything, launch normally.
    echo "[x3run] testandlaunch not found in command; launching unchanged" >&2
    exec "$@"
fi
