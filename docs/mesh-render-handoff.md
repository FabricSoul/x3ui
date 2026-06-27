# Handoff: X3 overlay mesh rendering — status & how to continue

You are picking up `~/x3ui`: an ImGui overlay LD_PRELOAD'd into **X3: Albion Prelude**
(`X3AP_main`, 32-bit non-PIE Linux ELF @ 0x08048000), authorized RE of a game the user
owns, via Ghidra MCP + an in-game read-only debug socket. The current sub-project —
rendering a recognizable 3D wireframe of a selected ship/station — is **DONE and
engine-accurate**. This doc orients you to continue.

## Read these first (authoritative)
- **`docs/x3-render-pipeline.md`** — the verified engine render pipeline, every claim
  tagged **(D)** decompiled or **(L)** live-verified. This is the source of truth for the
  draw gates, LOD, and the transform. Trust it over any older note.
- Memory: `[[x3ui-mesh-render-parked]]` (recipe summary), `[[x3-re-methodology]]` (how to
  work — below), `[[x3ui-project]]`, `[[user-fabric-x3]]` (the user is an X3 modder, fluent
  in Ghidra/RE — talk at that level).

## What works now (branch `feat/sector-map`, ~16 commits ahead of main)
Selecting an object in the overlay shows a correct, GPU-rendered orbiting wireframe that
matches what the engine draws:
- **`read_object_wireframe`** (src/sector_reader.cpp) reconstructs the engine's render:
  walk the scene node tree from `*(obj+0x6c)`; gate drawing on the engine's `+0x118&2`
  visible flag (HYBRID: gate when on-screen, draw-all fallback off-screen) + per-part
  `part+0x50` collision gate + skip `id<1000` proxies; place each part by its CACHED WORLD
  transform — orient `+0xac` (16.16, read **transposed**/column-major), world pos `+0x9c`
  (rel root), **raw** size scalar `+0x6c`. Per-object LOD slider. (Full recipe in the doc.)
- **GPU rendering** (src/overlay.cpp `meshgl`): line geometry → GL VBO once per selection,
  drawn in one `glDrawElements` via an ImGui draw callback (MVP uniform). Smooth at finest
  LOD even on 130k-vert stations.
- **`owire <addr> [lod]`** debug command → ObjWireStats (nodes/resolved/kept/dropped/maxc).
- Host logic tests green (`make test`: 69/0 + 20/0).

## The methodology (internalize this — the user insists on it)
**Decompile + live-verify + document. Do NOT guess.** Every real fix came from RE, not from
tweaking constants; guessed heuristics each fixed one symptom and broke another. The loop:
1. Decompile the engine function(s) in Ghidra (for deep passes dispatch an RE sub-agent that
   ToolSearches `mcp__ghidra__*`, follows callees/xrefs, renames FUN_/DAT_). Get exact
   offsets/bitmasks/arithmetic with cited lines.
2. Live-verify via the debug socket: `owire` stats make bugs objective; write python probes
   over `127.0.0.1:7373` for structure. Socket = ONE connection at a time, serviced between
   frames, answers only in active flight.
3. Write findings to `docs/x3-render-pipeline.md`, tagged (D)/(L).
4. Implement one change → `nix-shell --run 'make'` → `./x3launch.sh` → **the user visually
   verifies** (the only oracle for "looks right"; unit tests use tiny synthetic graphs and
   miss every real-game bug). If the user says "还是不对", believe them and re-RE.

## Dev loop / tools
- `./x3launch.sh [slot]` — boots straight into flight (`-skipintro -faststart <slot>` via
  x3engine.sh, then `steam -applaunch 201310`). Relaunch to load a rebuilt `libx3ui.so`.
  Kill the running game first (`pkill -x X3AP_main`).
- `nix-shell --run 'make'` builds the 32-bit `.so`; `make test` runs host logic tests.
- `tools/x3dbg <cmd>` or a python socket: `read`, `bytes`, `sector`, `children <addr>`,
  `owire <addr> [lod]`, `scene`. Game must be in active 3D flight.
- **NEVER call `model_load_resolve`@0x08080ca0 live** (poisons a sticky neg-cache until
  sector reload).

## Open / next steps (in priority order)
1. **Off-screen previews fall back to draw-all** (extra LOD/internal geometry until the
   object is brought on-screen), because the `+0x118&2` selector is per-frame. To make
   off-screen previews also engine-exact, find a STATIC LOD/visibility selector: RE how the
   engine chooses among sibling LOD-variant nodes when the object is far (is there a
   LOD-group node with distance thresholds + a resolvable index?). Probe candidate: the
   parents in `diag9` (group nodes id=0 / flag 0x8100000) whose children are the LOD
   variants `901000003`/`10495`/`10496`.
2. **Solid / shaded mesh rendering** — the GPU VBO + shader plumbing in `meshgl` is the
   foundation; add a triangle index buffer + depth test + simple lighting. The user said
   "可以开始画mesh了" (ready to draw the actual mesh).
3. Optional polish: surface a "no mesh (off-screen / not loaded)" hint; expose the LOD as
   distance-driven; revisit the sector-map panel.

## Acceptance / sanity check when you resume
In flight, `./x3launch.sh`, ScrollLock for the overlay, left-click a ship and a station that
are **on-screen**: the preview should match the in-game model (hull + turrets/components,
correct orientation & scale), smooth at LOD 0. `owire <addr>` on a ship ≈ a handful of parts;
on a station, the `&2`-gated subset.
