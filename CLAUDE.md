# X3 injected-UI mod — project guide

An LD_PRELOAD'd ImGui overlay for **X3: Albion Prelude** that drives the game (not a
read-only HUD). Authorized reverse-engineering of a game the user owns. `X3AP_main` is a
32-bit, non-PIE Linux ELF @ image base `0x08048000`. Overlay source: `src/overlay.cpp` →
`libx3ui.so`, injected via `x3run.sh` → `x3engine.sh`.

## Build · run · test
- **Build** (32-bit; MUST be inside the nix-shell): `nix-shell --run 'make'`
- **Host logic tests** (any shell; arch-independent): `make test`
- **Launch into flight**: `./x3launch.sh [slot]` — boots straight into 3D flight.
  - Kill the running game first: `pkill -x X3AP_main`; relaunch to load a rebuilt `.so`.
  - **Claude runs the launch itself** — never ask the user to start the game.
- **Live introspection** (no rebuild): read-only debug socket on `127.0.0.1:7373`
  (`tools/x3dbg <cmd>` or a python socket): `read/reads/bytes/cstr/children/scene/sector/owire`.
  One connection at a time, serviced between frames; game must be in active flight.
- **NEVER call `model_load_resolve`@0x08080ca0 live** — it poisons a sticky neg-cache.

## Knowledge map — what exists, what to read when
Auto-loaded every session: **this file** + the **memory index**
(`~/.claude/projects/-home-fabric-x3ui/memory/MEMORY.md`). Deep references live in `docs/`
(full catalog: [docs/README.md](docs/README.md)):

| Source | What it is | Read when |
|---|---|---|
| `docs/x3-render-pipeline.md` | **Authoritative** engine render pipeline — draw gates, LOD, transform, orient convention; every claim tagged **(D)** decompiled / **(L)** live | any mesh/transform/render work — trust it over older notes |
| `docs/mesh-render-handoff.md` | handoff for the wireframe sub-project (status + dev loop) | historical context |
| `docs/superpowers/specs/*` | per-feature designs (what/why/architecture) | before changing that feature |
| `docs/superpowers/plans/*` | per-feature step-by-step implementation plans | executing/continuing that feature |
| memory `MEMORY.md` + files | cross-session RE facts, methodology, user prefs, env gotchas | **always** — it's the index; categorized |

## Conventions (see the linked memory for the *why*)
- **Methodology: decompile + live-verify + document. DON'T guess.** And **verify the DATA
  before assuming a logic bug** (a wrong-*looking* render can be perception, not math).
  → memory `x3-re-methodology`.
- **Aesthetic: this is a game MOD, not a dev tool** — translucent holographic HUD, lean
  cinematic. → memory `x3ui-holographic-aesthetic`.
- For deep RE passes, dispatch a sub-agent that ToolSearches `mcp__ghidra__*`, follows
  callees/xrefs, and **renames FUN_/DAT_** to grow the KB. → memory `re-rename-in-ghidra`.
- Commit/push only when the user asks; current work is on branch `feat/sector-map`.

## Keeping the knowledge base robust
When you learn something durable, **write it down and index it**:
- Cross-session FACT (engine layout, methodology, preference, env) → a **memory file** +
  a one-line entry in `MEMORY.md` under the right category.
- Deep verified RECIPE / finding → `docs/x3-render-pipeline.md` (tag **(D)/(L)**).
- A new FEATURE → a dated **spec + plan** under `docs/superpowers/` + a row in
  `docs/README.md`.
