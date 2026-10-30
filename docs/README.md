# docs/ — knowledge catalog

Navigation for the X3 injected-UI project's deep references. See also the repo front door
[`../CLAUDE.md`](../CLAUDE.md) (build/run + conventions) and the cross-session **memory
index** (`~/.claude/projects/-home-fabric-x3ui/memory/MEMORY.md`).

## Authoritative engine references
| File | What | Read when |
|---|---|---|
| [x3-render-pipeline.md](x3-render-pipeline.md) | **The** verified engine render pipeline: drawable enumeration & gates (`+0x118&2`, `part+0x50`), LOD, the modelview transform + column-major orient convention, the off-screen sim-refresh of `+0xac/+0x9c`, parent→child compose formula. Every claim tagged **(D)** decompiled / **(L)** live. | Any mesh/transform/render/orientation work. Trust over older notes. |
| [mesh-render-handoff.md](mesh-render-handoff.md) | Handoff for the object-wireframe sub-project — status, methodology recap, dev loop, open items. | Historical context for the render work. |
| [x3-msci-codearray-format.md](x3-msci-codearray-format.md) | How to **read/hand-write** compiled MSCI `.pck` scripts: the array-10 wrapper, opcode table, value-pair tags, START-vs-inline call, `if`/`goto` model, signature=omit, clean-gzip. Statically decoded from stock scripts. | Hand-building a `.pck` (dock bridge, any injected script) instead of the in-game SE. |

## Per-feature specs & plans (`superpowers/`)
Each feature gets a **spec** (what/why/architecture, from brainstorming) and a **plan**
(bite-sized implementation steps). Build on the matching pair before changing a feature.

| Feature | Spec | Plan | Status |
|---|---|---|---|
| Sector-map readout | `superpowers/specs/2026-06-26-x3-sector-map-readout-design.md` | `superpowers/plans/2026-06-26-x3-sector-map-readout.md` | done |
| Object wireframe | `superpowers/specs/2026-06-27-x3-object-wireframe-design.md` | `superpowers/plans/2026-06-27-x3-object-wireframe.md` | done |
| Holographic solid hulls (3D map) | `superpowers/specs/2026-06-27-holographic-solid-hulls-design.md` | `superpowers/plans/2026-06-27-holographic-solid-hulls.md` | done (`feat/sector-map`) |
| Target bridge (overlay ⇄ game target) | `superpowers/specs/2026-06-27-target-bridge-design.md` | _(pending)_ | design |

## Where new docs go
- A deep, verified engine recipe/finding → append to **x3-render-pipeline.md**, tagged **(D)/(L)**.
- A new feature → a dated **spec + plan** under `superpowers/`, then add a row above.
- A cross-session fact / preference / methodology note → a **memory file** + `MEMORY.md`
  (don't bury it here; docs/ is for repo-scoped recipes & feature designs).
