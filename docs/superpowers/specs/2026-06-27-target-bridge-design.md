# Spec: Target bridge — overlay selection ⇄ game target

Status: design, awaiting review. Branch: `feat/sector-map`.
Authoritative RE: memory `x3ui-player-target`; scripts: memory `x3-pck-scripts-gzip`.

## Goal

Clicking an object in the overlay's **sector table** (`map_panel`) or **3D map**
(`map3d_panel`) makes the **game itself** target that object — the in-flight HUD targeting
brackets jump to it, exactly as if the player had selected it in-game. Conversely, when the
player targets something through the game's own controls, the overlay reflects it (the live
game target is highlighted in the table and map). This is the **first write-path** from the
overlay into the engine; everything before this was read-only.

## Decisions (locked)

- **Mechanism = call the engine setter, NOT a raw memory write.** Verified this session:
  `gravidar_set_target@0x08131d10` does bookkeeping a 2-field write would miss (lock
  timestamp `+0x1c8`, scratch `+0x1f4` clear, target-acquired SFX, HUD UI-bit toggles, and
  the **`NotifyTargetLock` script callback** that AI/wingman logic listens for), and the HUD
  re-derives bracket state every frame so a bare write fights the renderer. We call the
  function in-process.
- **Lock mode = 3** (brackets + lock). Mode 3 is sticky — it holds across the per-frame HUD
  refresh until the target dies/leaves, so we set it once per click (no per-frame re-assert).
- **Two distinct indicators, kept separate:** `g_sel_addr` stays the overlay's *click
  selection* (it also drives the mesh-preview panel). The *game's live target* is read back
  and drawn as its own marker. Clicking sets both to the same object; pressing the game's
  target key moves only the game-target marker (the mesh viewer does not chase it). Rationale
  for keeping them separate: inspecting object A's hull while the game targets B is a valid
  state, and the user asked specifically to "highlight the game target."
- **Purity boundary:** the gravidar-object *lookup* is a pure `Mem`-based read in
  `sector_reader` (testable). The engine-function *call* lives in `overlay.cpp` (in-process,
  real pointers, no `Mem` abstraction). `sector_reader` never calls engine code.
- **No writable mem fd needed.** Because we call the function rather than poke memory, the
  existing `O_RDONLY` `/proc/self/mem` path is untouched.

## Non-goals (this iteration)

- **Ship commands (dock / attack / move / follow).** Verified to be **script logic** (the
  command system dispatches to MSCI scripts: `plugin.com.*`, `!fight.attack.object.*`,
  `lib.le.move.*`). That is the *next* phase (the right-click "command bridge") and needs a
  separate spec — the context-menu stub in `map3d_panel` stays disabled.
- **Sub-target / turret target, "set current sector object"** (`+0x1d8`, a separate field
  from the combat target) — not touched.
- **Re-asserting the lock every frame** — unnecessary given mode-3 stickiness; revisit only
  if live testing shows the target gets stomped.

## Engine foundation (verified this session — see memory `x3ui-player-target`)

- Setter: `void __cdecl gravidar_set_target(void* gravidarObj, int16_t mode, void* targetObjPtr)`
  @0x08131d10. **cdecl** (gravidar is the first *stack* arg, not `this`/ECX; bare `RET`,
  caller cleans). `targetObjPtr` is a real world-object **pointer**, not an id. mode: 0 clear,
  3 lock+brackets. (D)
- The gravidar/HUD-state object holds the current target at `+0x1bc` (pointer) and mode at
  `+0x1c0`. It lives in the `g_hud_mgr`@0x0837a100 hashmap — the **same object**
  `sector_reader.cpp:resolve_range_ctx` already walks to read the scanner range
  (`+0x10`==player ship, `+0x240`==range). (D/L)
- Reachable from both engine input (keyboard cycle / cursor pick / per-frame bracket render)
  and MSCI script opcode 0x26; the engine HUD path owns per-frame truth. Targeting is
  therefore **engine logic**; we integrate by calling the engine setter. (D)

### Phase 0 (live confirm before wiring the call)

With the game in active flight and a target selected in-game, drive the debug socket to:
1. Resolve the gravidar object via the hashmap walk; read `+0x1bc` and confirm it equals the
   address of the object currently bracketed in-game; cycle target (T) and confirm `+0x1bc`
   tracks. Confirms the read-back field for the two-way highlight.
2. (Cannot socket-test the *write* — the socket is read-only.) The call is validated by the
   build+launch visual test below.

## Architecture

Three pieces:

### 1. `find_player_gravidar` (sector_reader) — factor out the existing walk

Extract the `g_hud_mgr` hashmap walk currently inlined in `resolve_range_ctx`
(`sector_reader.cpp:552-574`) into:

```cpp
// Address of the player's gravidar/HUD-state object (scanner source == player ship),
// or 0 if not resolvable. Pure reads via Mem.
uint32_t find_player_gravidar(const Mem& mem, uint32_t player_ship, uint32_t scene_global = off_SCENE_GLOBAL);
```

`resolve_range_ctx` calls it, then reads `+0x240` for range (behavior unchanged — same
walk, just named). New consumers (target read-back + the overlay setter) reuse it. Single
source of truth for "where is the gravidar."

### 2. Two-way read-back (sector_reader → SectorView)

`SectorView` gains `uint32_t current_target_addr = 0;`. `RangeCtx` gains
`uint32_t gravidar_addr = 0;` (set in `resolve_range_ctx`, which already resolves it). In
`read_current_sector` — which already builds a `RangeCtx` for the range filter — read
`rc.gravidar_addr + 0x1bc` into `current_target_addr` (0 if the gravidar didn't resolve).
Both panels already hold a `SectorView`, so they get the live game target for free — no extra
plumbing, one extra read per refresh.

Add the offsets/address to `struct_offsets.h` (single source of truth):
`GRAVIDAR_TARGET = 0x1bc`, `GRAVIDAR_TARGET_MODE = 0x1c0`, `GRAVIDAR_SET_TARGET_FN = 0x08131d10`,
`TARGET_MODE_LOCK = 3`, `TARGET_MODE_CLEAR = 0`.

### 3. The write path (overlay) — `select_target` body + game-target highlight

`select_target(o)` (overlay.cpp:805), after setting `g_sel_*`:
1. Resolve the player ship (`scene+0x38`) and `find_player_gravidar(make_engine_mem(), …)`.
2. If found and `o.addr` looks valid, call the engine setter:
   ```cpp
   typedef void (__cdecl* set_target_t)(void*, int16_t, void*);
   ((set_target_t)off::GRAVIDAR_SET_TARGET_FN)((void*)gravidar, off::TARGET_MODE_LOCK, (void*)o.addr);
   ```
3. If the gravidar can't be resolved, keep the UI selection only (no crash, no-op into game).

Game-target highlight: in `map_panel` (table) and `map3d_panel` (3D), the row/blip whose
`o.addr == view.current_target_addr` gets a distinct **game-target marker** — table: a small
reticle glyph / accent in the name cell; map: a targeting-reticle ring around the blip/hull,
visually distinct from the existing click-selection box. (Selection box = `g_sel_addr`;
reticle = game target. They coincide right after a click.)

Optionally expose a "clear target" affordance (calls the setter with mode 0) — small, can be
in the same change or deferred.

## Data flow

```
click in table/map ─▶ select_target(o)
                        ├─ g_sel_id/g_sel_addr = o            (overlay selection, mesh viewer)
                        └─ gravidar = find_player_gravidar(); ((set_t)0x08131d10)(gravidar,3,o.addr)
                                                              → engine sets brackets + NotifyTargetLock
read_current_sector (≈6×/s) ─▶ view.current_target_addr = *(gravidar+0x1bc)
panels ─▶ highlight row/blip where o.addr == view.current_target_addr   (game-target reticle)
```

## Thread-safety

Overlay code runs inside the `SDL_GL_SwapWindow` hook on the game's **main thread, between
frames** — the sim is not mid-update there, so calling `gravidar_set_target` is safe (it is
the same thread the engine itself calls it on). Confirm no crash live.

## Testing

- **Host logic tests** (`make test`): `find_player_gravidar` on a synthetic `Mem` (fake
  hud-mgr → hashmap → gravidar with `+0x10`==player) returns the gravidar address; returns 0
  when the source doesn't match / the chain is broken. (Pure; the engine *call* can't be
  host-tested — synthetic mem only.)
- **Live (the real oracle):** `./x3launch.sh`, open the table and 3D map; click an object →
  in-flight HUD brackets jump to it, no crash; click another → brackets move; press the game's
  target key → the overlay's game-target reticle follows. Try a station, a ship, an asteroid.
  Verify the lock holds (mode 3 sticky) while flying.

## File-by-file

- `src/struct_offsets.h` — add `GRAVIDAR_TARGET`, `GRAVIDAR_TARGET_MODE`,
  `GRAVIDAR_SET_TARGET_FN`, `TARGET_MODE_LOCK/CLEAR`.
- `src/sector_reader.h/.cpp` — `find_player_gravidar`; `SectorView::current_target_addr`;
  read it in `read_current_sector`; `resolve_range_ctx` reuses the factored walk.
- `src/overlay.cpp` — `select_target` body (resolve gravidar + call setter); game-target
  reticle in `map_panel` and `map3d_panel`.
- `src/test_*` — `find_player_gravidar` unit test.
- memory `x3ui-player-target` — updated (cdecl signature). `docs/README.md` — add the row.

## Open questions for review

1. Game-target marker style — a reticle ring around the blip (proposed), or just reuse the
   selection-box accent in a different color? (Distinct shapes read clearest.)
2. Include "clear target" (setter mode 0) in this iteration, or defer with the command bridge?
3. On game-target change (player presses T), should the **table auto-scroll** to reveal the
   targeted row, or only highlight it if already visible? (Proposed: highlight only.)
