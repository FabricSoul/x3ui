# Community question (Discord) — replicating docked "Dock at" from injected C

> To the modder who suggested `START [PLAYERSHIP] -> call script '!ship.cmd.movestation.std'`.
> Discord tone: casual, no subject line, fits ~1 message. Reframes from script-level → engine-level.

---

hey, thanks for the `movestation.std` tip earlier 🙏 — I should explain what I'm actually doing though, cause I don't think writing a script is the layer I can work at, and that's probably why it didn't quite fit.

I'm building an **LD_PRELOAD overlay for X3AP that drives the game from injected C** — I read/write engine memory and call its functions directly, in-process, **not via MSCI scripts**. My overlay's "Dock at" should make my *own* ship undock → fly → dock at another station, like the docked-menu button.

already working from C:
• flight "Dock at" — I replay Shift+D and the ship autopilots + docks ✅
• physically launching a docked ship — found the engine's undock leaf (writes world-pos out of the bay + outward velocity + relinks the ship's container station→sector), my ship really ejects into space ✅

where I'm stuck — **docked→dock**: after my physical launch the game's input **still thinks the player is docked**, so my Shift+D replay bails. traced it: the "Input" handler runs but reads **zero ship-object fields** before giving up → it's gating on some **script-side state**, not an object field (the ship object is byte-identical docked vs after-my-launch except the container ptr). raw ship-command pokes just overshoot/abort (no multi-leg task). and running `!move.movetostation` from C via the run-script call gives `find_id = -1` (the name won't resolve).

so my real questions are engine/mechanism level, not "write a script":
1. on a normal undock/launch, what actually flips the player from "docked" → "flying a ship" so flight input works again? (I already ruled out ship flags, the cockpit/gravidar flight flag, IsPlayerLand, and the script-context key — all identical docked vs my-launched)
2. does the docked-menu "Dock at" undock and then fire the **same** command as flight Shift+D, or a different one? is it `!ship.cmd.movestation.std` set as the ship's command + run as a task — and if so, **what starts that task for the player's ship** on the menu path?
3. how's that command-script identified internally (script id / name-hash)? the plain filename string won't resolve for me.

basically I just need to understand what the docked "Dock at" does **under the hood** so I can reproduce it by calling the engine directly, not by injecting a script 🙏

---

## Our own crib notes (do NOT send — for us)
- **Physical launch leaf:** `ship_launch_from_dock_physical@0x080f3a80(ship, station)` — ejects ship (world pos out of bay + velocity, `payload+0xc4=0x21` undock-anim, relink→sector via `object_relink_container_attach_scene@0x080fa4f0`). Verified live. Distinct from `object_set_container_relink@0x080f31c0` (logical-only when container class==1 sector).
- **Flight-dock (works):** `input_dispatch_key_script@0x08053690(0x1064)` (Shift+D) after `settarget`.
- **The bail is script-space:** `tracedock 0x1064` post-launch → 0 SA-ops; docked vs launched player object identical except `obj+0x54`. Ruled out: IsPlayerLand (`class7 && payload+0xc4==0x1B`), cockpit widget `+0x10`/`+0x138`, `obj+0x90` scriptKey.
- **Route B blocker:** `run_script_async(..., "!move.movetostation", ...)` → `msci_find_id = -1`; needs the id/name-hash, not the string. Fallback: read it off a live NPC dock task's `*(int*)(*(task+0x3c))`.
