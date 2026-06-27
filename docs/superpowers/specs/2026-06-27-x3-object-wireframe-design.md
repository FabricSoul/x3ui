# Design: render a recognizable object wireframe (scene-graph walker)

Date: 2026-06-27
Status: approved (brainstorm), ready for implementation plan
Supersedes the blocker in `docs/mesh-render-handoff.md` (which is now solved).

## Goal

When the user left-clicks a ship/station/factory/gate/asteroid in the overlay, the
mesh-preview panel shows a **recognizable orbiting wireframe** of that object,
assembled from its real loaded geometry. Pure process-memory reads only — no
engine render calls, no GL state — so it cannot crash the game. Objects whose
models are not resident (far / not yet streamed) gracefully show "no mesh".

## Background: the blocker, and how it was solved (live-verified 2026-06-27)

The handoff's geometry reader (`read_mesh_wireframe`) was correct, but it read the
model id from `obj+0x12c`, which is a **constant logical tag (289)** for every
ship/station/gate — never a resident render-model id. `wire(obj)` therefore always
returned `ok=0`.

The real render anchor is **`obj+0x6c`**, the object's transform/scene node.
`read_mesh_wireframe(*(obj+0x6c))` returns `ok=1` for everything. But the root node
is composite: for ships/stations its own `+0x12c` is a low **proxy** model (0 → a
152-vert box, 1 → a 4-vert quad). The detailed hull + parts + turrets live on the
root node's **descendant scene-graph nodes**, each with its own real resident
`+0x12c`. Simple objects (asteroids) carry their real model directly on the root.

Verified live (one socket session, in flight):

Totals below are at the code's current **coarsest-LOD** default; finest LOD is
several× larger (see the LOD section — e.g. the Nova hull id 20458 is 13,452v at
finest vs 486v coarsest).

| Object | nodes walked | real-mesh nodes | total verts (coarsest) | hull model |
|---|---|---|---|---|
| Discoverer (ship) | 57 | 23 | 1,470 | id 19011 (455v) |
| Argon Equipment Dock (station) | 211 | 80 | 27,574 | id 21418 (5,488v) |
| Factory `{17,6653}` | 143 | 59 | 17,329 | parts 21522/21523 |

## Verified engine facts (offsets, all live-confirmed unless noted)

Scene/transform node (`node = *(obj+0x6c)`; same struct for every descendant):

| offset | meaning |
|---|---|
| `+0x00` | next sibling (linked list) |
| `+0x0c` | first child |
| `+0x18` | parent back-link (do not follow) |
| `+0x1c` | shared scene singleton (`0x0d2b1e30` last session) — do not follow |
| `+0x20` | model-name string (`char*`) — set in `FUN_08082cb0` |
| `+0x2c/0x30/0x34` | local translation (relative to parent) — units TBD-in-impl |
| `+0x3c..0x64` | local orientation, 3x3 16.16 fixed (`0x10000` = 1.0) |
| `+0x70/0x74/0x78` | local scale, 16.16 (`0x10000` = 1.0) — set in `FUN_08082cb0` |
| `+0x118` | flags; bit 2 (`&4`) = view-list visibility (ONLY the root has it — NOT a per-mesh marker, do not filter on it) |
| `+0x12c` (300) | model id (resolved via the resident list; `read_mesh_wireframe` already validates it) |
| `+0x138` | per-node LOD index |

Resident model + mesh layout (already implemented in `read_mesh_wireframe`):
`world=*(0x082a0640)`; list head `*(world+0x8)`, next `*(m+0x4)`, id `*(m+0x8)`;
model `+0xc`=LOD-ptr array, `+0x10`=LOD count(i16); mesh `+0`=vcount,
`+8`=int16 xyz verts (stride 0x18, /16384), `+4`=part count, `+0x10`=parts →
material groups → triangle indices.

Engine cross-check (authoritative drawable enumeration):
`build_visible_object_list` @ `0x08074280` walks `*(view+8)` following next `+0x00`
and children `+0x0c`, exactly the traversal used here.

## LOD: decision and rationale (data-backed)

LOD is **per model**, inside `model+0xc[i]` (finest = `LOD[0]`, coarsest =
`LOD[cnt-1]`). Measured: hull id 20458 = `[13452, 6873, 2198, 486]` verts; thruster
id 19080 = `[1996, 1002, 440, 86]`. `read_mesh_wireframe`'s `coarsest=true` **default
currently draws the blocky lowest LOD** — that is the "low-poly" symptom.

Repeated model ids across nodes are **NOT LOD copies** but real separate parts:
Nova carries id 19080 on **8 nodes at 8 distinct transforms** (8 thrusters). Parts
that share an equal *local* offset still differ in world space once parent
transforms compose. So there is no reliable cross-node dedup, and none is needed.

**Decision:** draw **all** mesh-bearing nodes, each at its **finest in-model LOD
(`LOD[0]`)**. Never drops real geometry; any true duplicate just overlaps the same
lines (harmless, bounded by the vertex cap). The finest-vs-mid choice is a one-line
visual knob to tune by eye once it renders.

## Architecture

### 1. `read_node_mesh` (refactor, `src/sector_reader.cpp`)
`read_mesh_wireframe` already reads `addr+0x12c` and resolves model→mesh→edges, so
it already works when `addr` is a node (that is how `wire <tnode>` was validated).
Either reuse it directly with `coarsest=false`, or extract its model→mesh→edges core
into `read_node_mesh(mem, node, finest_lod) -> MeshWire` returning edges in the
node's **local** model space. `read_mesh_wireframe` keeps its current signature/tests.

### 2. `read_object_wireframe(mem, obj_addr) -> MeshWire` (new)
```
visit(node, M):                       # M = node's transform relative to root
    part = read_node_mesh(mem, node, finest=true)
    if part.ok: append (M * v) for every edge endpoint v in part  -> combined
    for child in child_list(node):    # head = *(node+0x0c), next = *(c+0x00)
        if child not visited and within caps:
            visit(child, M * local(child))

root = *(obj_addr + 0x6c)
visit(root, identity)                 # root defines object space (ignore its world placement)
combined.ok = !combined.edges.empty()
```
- **Caps:** node cap (~256), vertex/edge cap (~40k), depth cap — a pathological or
  cyclic graph can never run away. A **visited-set** breaks the parent/sibling cycles.
- **Safety:** every node goes through `read_mesh_wireframe`'s existing resident-list
  validation, so proxy nodes (id 0/1/33/535/566) and any garbage reached by the walk
  return `ok=false` and are skipped. All reads via the `Mem`/`pread` abstraction.

### 3. Local transform composition
`local(node) = translate(+0x2c/0x30/0x34) ∘ rotate(3x3 @ +0x3c, /65536) ∘
scale(+0x70/0x74/0x78, /65536)`. Compose parent→child down the DFS; apply each
node's accumulated `M` to that node's edge endpoints, so the assembled parts land in
the root's object frame.
**Impl gate:** confirm the translation/scale **units** live before trusting them —
assemble one known multi-part object (e.g. Nova's 8 thrusters) and check the parts
sit in the right places; adjust the unit divisor against a measured part position.

### 4. Overlay (`src/overlay.cpp`)
- `mesh_preview_panel` calls `read_object_wireframe(make_engine_mem(), g_sel_addr)`
  instead of `read_mesh_wireframe`.
- **Compute once per selection and cache** the assembled `MeshWire` (it is hundreds
  of nodes × thousands of verts — never per frame). Re-read only when `g_sel_addr`
  changes (or on an explicit refresh).
- Orbit camera auto-frames the assembled bounding box.

### 5. Debug command (`src/debug_server.cpp`)
Add `owire <obj_addr>` → returns `ok addr=.. nodes=N meshes=M v=.. tris=..` from
`read_object_wireframe`, for live iteration (mirrors existing `wire`).

### 6. Tests (`tests/test_sector_reader.cpp`)
Extend the `fake_mem` harness with a small synthetic scene graph: a root node with
two child nodes (distinct model ids + distinct local translations) and one
grandchild, plus matching resident models/meshes. Assert that `read_object_wireframe`:
unions all mesh-bearing nodes, skips proxy/`-1` nodes, terminates on a cycle
(child whose sibling points back), honors the node/vertex caps, and applies each
node's translation (a part's edges land offset by its translation).

## Data flow
`g_sel_addr` (selected logic object) → `obj+0x6c` root node → DFS subtree →
per-node `read_node_mesh` (finest LOD) → transform to object frame → unioned
`MeshWire` (cached) → `project3d` + ImGui draw list (orbit camera).

## Error handling / non-goals
- Pure reads; bounded; failure of any read drops that node, never aborts.
- Not yet resident → fewer/no parts → "no mesh" or partial wireframe (acceptable).
- **Out of scope (future):** LOD-group collapse if overdraw ever costs fps; textured
  / solid rendering; the disk-based `.bod` path (parse from cat/dat) for objects that
  are not loaded at all.

## Risks
1. **Transform units** (`+0x2c/0x30/0x34`, scale) — verify live during impl against a
   known part layout before trusting (the one real unknown).
2. **Perf** — finest LOD on a big station can be ~30–50k verts; rely on the per-frame
   projection being cheap + the per-selection cache + the vertex cap; expose a
   finest/mid LOD knob if a small panel looks too dense.
3. **Walk wandering** into non-node memory via `+0x0c/+0x00` — already bounded by the
   resident-list validation (junk ids → `ok=false`), the visited-set, and the caps.

## Acceptance
`owire <nearby ship/station>` returns `nodes>1 meshes≥1 v=<thousands>`, and selecting
that object in the overlay shows a recognizable, orbitable wireframe (hull + parts).
Far/not-loaded objects showing "no mesh" is acceptable.
