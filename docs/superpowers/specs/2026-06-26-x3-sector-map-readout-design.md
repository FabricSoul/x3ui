# X3 Injected UI — Sector Map Readout (Phase 1)

- **Date:** 2026-06-26
- **Status:** Approved design, pre-implementation
- **Scope:** First concrete feature of the injected ImGui UI — a read-only panel listing the objects in the player's current sector.

## 1. Background

The project injects an ImGui UI into X3: Terran Conflict / Albion Prelude (32-bit x86
engine, non-PIE @ `0x08048000`) via `LD_PRELOAD` (`libx3ui.so`), hooking
`SDL_GL_SwapWindow` and rendering with the ImGui GL3 backend, toggled by ScrollLock.

This session reframed the architecture. The earlier plan was to author our own MSCI
script (`x3uimoney.pck`) to read/write state; that was blocked on script registration.
The reframe: **don't author scripts** — the game's own logic already exists. Two layers
were confirmed by reading the engine and the stock scripts:

- **Game state (objects, positions, the map)** lives in the **engine's C++ memory**, not
  in scripts. It is read by walking engine structs.
- **Actions / commands** ("dock at station X", "autotrade") are **MSCI scripts** bound to
  command slots via `set script command upgrade` / `global script map`. Some commands
  (e.g. autotrade) exist *only* as scripts. These are driven later via the proven
  `run_script` bridge.

The 660 stock `.pck` scripts are plain gzip and decompile to readable MSCI source
(`<sourcetext>`); they remain our reference/dictionary for what exists and how actions
compose. Script names are **not** present in the engine binary (verified: 0 hits) — they
are runtime data. The engine's own API names (`GetCurrentObjectIDInSector`,
`INS_GalaxyMap*`, `GS_SetSectorOwner`, …) **are** present as strings and map to VM script
opcodes (e.g. `GetCurrentObjectIDInSector` = opcode `0x137`, dispatched by the VM, not a
callable cdecl function).

**Decision for Phase 1:** because reads require no VM at all, Phase 1 is a pure,
read-only engine-memory walk. Every legacy blocker (`.pck` registration, `run_script`
argument marshalling, opcode dispatch) is out of scope for this feature.

## 2. Goal & success criteria

A toggleable ImGui panel that lists the objects in the player's current sector, refreshed
live, with at minimum: object name, type (ship/station/gate/etc.), and position.

**Done when:** with the game running and the overlay injected, opening the panel shows the
current sector's objects, and the list visibly matches the in-game sector map (objects
appear/disappear and move as they do in game). No game state is modified.

## 3. Non-goals (deferred)

- **Any action / mutation** (issuing commands, trading) — that is Phase 2 and uses the
  `run_script` bridge with argument marshalling.
- **Owner / race / relation tint** — the owner field was not located this session (see
  §7). Ships render without an owner column in v1.
- **Galaxy-wide / multi-sector** listing — v1 is the current sector only (the global
  object list at `scene_mgr+0x08` is available as a stretch).
- **Click-to-target / interaction** from the panel — depends on Phase 2 actions.

## 4. Verified technical findings (single source of truth)

All offsets below are backed by a concrete write/link/lookup site in the engine and are
**high confidence** unless noted. Scene-manager global pointer: `DAT_0837a0d0`.

### Walk

```
scene_mgr = *(0x0837a0d0)
player_sector = *(scene_mgr + 0x38)        // a type-7 object
payload       = *(player_sector + 0x50)
// per-sector intrusive doubly-linked list:
for (node = *(payload + 0x00); node != (payload + 0x04); node = *(node + 0x00)) {
    // node is an object
}
// global "all objects" list (stretch): head *(scene_mgr+0x08), sentinel scene_mgr+0x0c
```

### Object struct (intrusive list node)

| Field | Offset | Type | Notes |
|---|---|---|---|
| next | `+0x00` | ptr | list link; sentinel terminates the walk |
| prev | `+0x04` | ptr | list link |
| unique id | `+0x08` | u32 | also key into `scene_mgr+0x14` id→ptr hashtable |
| name | `+0x0c` | `char*` | direct C string (may be null) |
| type / class | `+0x48` | i16 | 5 = ship, 6 = station, 7 = sector (range 0..0x1f) |
| subtype | `+0x4a` | i16 | per-type sub-id |
| parent / container | `+0x54` | ptr | 0 = top-level/global |
| transform | `+0x6c` | ptr | → position |

### Position (via transform `T = *(obj+0x6c)`)

| Axis | Offset in T | Type |
|---|---|---|
| X | `+0x2c` | i32 fixed-point |
| Y | `+0x30` | i32 fixed-point |
| Z | `+0x34` | i32 fixed-point |

Coordinates are signed **fixed-point integers**, not floats. (For type-7 objects the world
transform is at `payload+0x5f4` instead; not needed for listing contained objects.)

### Key engine functions (reference)

- `FUN_080f1fb0` — object link routine; proves both global and per-sector list offsets.
- `FUN_080f0750` — master object deserializer; proves name (`+0x0c`), type (`+0x48`),
  payload (`+0x50`).
- `FUN_080e6ec0` — id→object hashtable lookup (`scene_mgr+0x14`), if an id-based path is
  ever needed.
- `FUN_081014a0` — position setter on the transform (`+0x2c/+0x30/+0x34`).
- `run_script`: `FUN_0809e340` (sync+return), `FUN_0809e270` (async); script host
  `DAT_08308880`. **Phase 2 only.**

## 5. Architecture

Three small units, integrated into the existing `src/overlay.cpp` render path (the
`SDL_GL_SwapWindow` hook, which runs on the engine main thread — safe for reads).

| Unit | Responsibility | Depends on |
|---|---|---|
| `struct_offsets` (header) | Named constants for every offset/address in §4. Single source of truth; no logic. | — |
| `sector_reader` | Given the scene-mgr global, walk the current sector's list → `std::vector<ObjInfo>`; defensive (null/sentinel/sanity-bounded). Returns empty if not in a valid game state. | `struct_offsets` |
| `map_panel` | ImGui window: table of objects (name, type label, position), type filter, throttled refresh; reads from a cached `vector<ObjInfo>`. | `sector_reader`, ImGui |

```
ObjInfo { uint32 id; std::string name; int16 type; int16 subtype; int32 x,y,z; }
```

The name is **copied** into `ObjInfo` at read time, not held as a raw `char*` — the
engine may free/move the string between refresh ticks, so the cached panel data must own
its strings.

### Data flow (per refresh tick, not per frame)

1. `map_panel` decides whether to refresh (throttle, e.g. every ~150 ms or on demand).
2. Calls `sector_reader::read_current_sector()`.
3. `sector_reader` reads `*(0x0837a0d0)`; bails if null. Reads `scene_mgr+0x38`
   (player sector); bails if null or `type != 7`. Walks the payload list, capping
   iterations (e.g. ≤ 4096) to guarantee termination even if memory is mid-mutation.
4. For each node: copy id/type/subtype, copy the name string (bounded length, guard
   null), follow `+0x6c` for position (guard null transform). Push `ObjInfo`.
5. `map_panel` renders the cached vector into an ImGui table with a type filter.

## 6. Safety & error handling

- **Read-only:** no writes to game memory anywhere in Phase 1. Zero save-corruption risk.
- **Pointer hygiene:** every dereference (scene-mgr, sector, payload, node, transform,
  name) is null-checked before use. The walk terminates on the sentinel **and** on a hard
  iteration cap.
- **Concurrency:** all reads happen inside the `SwapWindow` hook (engine main thread), so
  the object graph is not being mutated by us; a hard iteration cap protects against
  reading a list mid-update.
- **Not-in-game states:** main menu / loading screens have a null scene-mgr or no player
  sector → `sector_reader` returns empty and the panel shows "no sector".
- **Name sanity:** treat `name@+0x0c` as possibly-null and bound its length when copying.

## 7. Open questions / follow-ups

- **Owner / race (deferred):** not located this session; lives in the per-type payload
  (`*(obj+0x50)`), differs by type. Resolve by tracing the `GS_SetSectorOwner` /
  owner-getter opcode before adding a relation tint. Tracked as a small RE task, not a
  blocker for v1.
- **Type → human label:** map `type@+0x48` (and possibly `subtype`) to readable labels
  ("Ship", "Station", "Jump Gate", "Asteroid", …). Start with the known few (5/6/7) and a
  numeric fallback; refine from the type switch in `FUN_080f0750`.
- **Position units:** fixed-point → display raw initially; convert to km once the scale
  factor is confirmed against a known in-game distance.

## 8. Validation plan (crawl → walk → run)

1. **Scalar:** read just `*(0x0837a0d0)` and `scene_mgr+0x38`; show "in sector: <name>"
   in the panel. Proves the root pointers live and the panel renders.
2. **List:** walk the sector list; show count + names. Eyeball against the in-game map.
3. **Columns + polish:** add type label and position columns, a type filter, throttled
   refresh; confirm objects move/appear/disappear live.

## 9. Notes

- Repo is not currently under git; this spec is saved but not committed. Initialize git if
  version control is wanted.
