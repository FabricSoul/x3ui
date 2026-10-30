# Spec: Command bridge — context menu orders a ship (dock/move/attack)

Status: built (MVP), live-verification pending. Branch: `feat/sector-map`.
Authoritative RE: memory `x3-ship-set-command`.

## Goal

Right-clicking an object in the overlay's sector table (`map_panel`) or 3D map
(`map3d_panel`) opens a context menu that **orders a ship to act on that object** —
"Move to / Dock at / Attack / Follow / Stop". This is phase 2 of the project's core goal
(drive the game, not just read it); phase 1 was the target bridge.

## Decisions (locked)

- **Mechanism = write the ship's AI command struct directly** (engine has NO callable
  setter). Per RE: on the ship PAYLOAD (`*(ship+0x50)`) write `+0xC4`=command id (int16),
  `+0x98`=target ptr, params `+0xAC..`, aux `+0xBC`=-1, start-time `+0x84`; on the ship obj
  clear the notify hook `+0x78/+0x7C`. The AI tick reads `+0xC4` each frame and
  **auto-runs the bound built-in command script** — no activate call, **no custom scripts**
  (which don't load anyway), **no owner check** (only gate is class==7). Command id written
  LAST so the tick never sees a half-set command.
- **Command ids** (verified): MOVE_OBJ=0x18, DOCKAT=0x04, ATTACK=0x14, FOLLOW=0x15, NONE=0x00.
- **MVP target = the PLAYER's own ship** (`scene+0x38`). Right-click object → order *your*
  ship. Cleanest to verify (you watch your ship fly/dock); commanding other owned ships is
  the next iteration (needs a "which ship" UX that doesn't clash with selection=target).
- **Write path = a second `O_RDWR` `/proc/self/mem` fd + `pwrite`** (`mem_write_u16/u32`).
  Crash-safe (bad addr → −1, never a segfault), same safety model as the pread reads. We're
  in-process so it writes our own memory.
- **Menu items enabled by object type:** Dock at → stations/factories; Attack/Follow → ships;
  Move to / Stop → always.

## Non-goals (this iteration)

- Commanding **other / selected** owned ships (next iteration).
- The rest of the command set (jump, protect, turret modes, move-to-**position** with x,y,z).
- Replicating the game's exact context-menu options/labels — we build our own ImGui menu.

## Architecture (all in `src/overlay.cpp` + `struct_offsets.h`)

- `struct_offsets.h` — `SHIP_CMD_ID/TARGET/PAR1/AUXTGT/STARTTIME`, `SHIP_NOTIFY_OBJ/VAL`,
  `RENDER_CFG_TIME`, `CMD_NONE/DOCKAT/ATTACK/FOLLOW/MOVE_OBJ`.
- `mem_write_u16/u32` (new `g_memfd_rw` O_RDWR) — the write primitives.
- `player_ship_addr()` — `*(scene+0x38)`.
- `ship_set_command(ship, cmdId, target)` — validates class==7 + payload, writes the struct
  (id last), no-op on bad pointers.
- `command_menu_items(o)` — the shared menu, wired into both context-menu popups.

## Risk / live-verification

- **Player-ship autopilot:** writing the command field on the *player's own* ship may not
  engage the autopilot the same way it drives an NPC's AI (the player is the pilot). If
  Dock-at on your own ship doesn't visibly act, pivot to commanding a selected **owned NPC**
  ship (the "no owner check" finding means any ship works) and re-verify.
- **Offset confirm first (methodology):** before trusting the writes, live-read NPC ships'
  `payload+0xC4` (current command id) and `+0x98` (target) — they should hold sane values
  (a trader docking, a fighter attacking). Then the right-click test.

## Testing

- **Offset validation (read-only socket):** `payload+0xC4/+0x98` on several NPC ships read as
  plausible command ids / target pointers.
- **Live (the oracle):** `./x3launch.sh`; right-click a station in the table/3D map →
  "Dock at" → the player's ship engages autopilot and docks; "Move to" an object → flies
  there; "Attack" a ship → engages; "Stop" → halts. No crash on rapid right-clicks.
