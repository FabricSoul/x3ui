# Spec: Holographic solid hulls in the 3D sector map

Status: design, awaiting review. Branch: `feat/sector-map`.
Authoritative engine facts: `docs/x3-render-pipeline.md` (D = decompiled, L = live).

## Goal

In the overlay's **3D map** panel (`map3d_panel`, src/overlay.cpp), render each in-range
object as a **solid, engine-oriented hull** with a **holographic sensor-panel** look,
instead of (when close enough) a flat blip dot. Orientation and position are taken
directly from the engine's live per-tick transform data, so hulls point and sit exactly
as the ships/stations do in the sector. The standalone **mesh preview** panel
(`mesh_preview_panel`) is kept unchanged as a wireframe inspector.

## Decisions (locked)

- **Scope:** every object the map currently draws as a blip (in scanner range, passes the
  panel's filter) is a hull candidate.
- **Shading:** translucent faces in the faction IFF hue + brighter **fresnel rim**;
  **additive** blend, no depth write — a see-through "ghostly hologram". No hidden-surface
  removal between or within objects (that x-ray look *is* the aesthetic).
- **Color:** faction IFF (`faction_color`), reused as a per-object uniform.
- **Dot ↔ hull by apparent size:** when an object's projected radius is below a pixel
  threshold (far / tiny in the map) it stays a **dot** (today's blip); above it, it draws
  as a **hull** and the center dot/glow is suppressed (the hull is the marker).
- **Selection:** replace the circle+ticks with a **four-corner bracket box**
  (targeting-reticle) fitted to the selected object's projected screen bounds — works for
  both dot and hull.
- **Keep** drop-lines, ground ticks, and labels (position readability on a 3D plot).

## Non-goals (this iteration)

- Off-screen **LOD/visibility-selector** accuracy (open item #1). Off-screen objects'
  `+0x118&2` selector is stale, so `read_object_wireframe`'s draw-all fallback may include
  extra LOD/internal parts. Map-distance LOD mitigates it; exact off-screen selection is
  deferred. The *transform* is correct off-screen (verified), only the part *selection*
  isn't.
- Solid rendering in the **mesh preview** panel (stays wireframe).
- **Model-id instancing/dedup** across identical ship types (perf optimization, future).
- **Live turret aim** — turret/attachment orientation is frozen at geometry-read time;
  whole-body orientation is live. (Aim deltas are visually negligible at map scale.)

## Engine foundation (verified this session, see x3-render-pipeline.md)

For node `= *(obj+0x6c)` (the root transform node of a top-level object):

- `node+0x2c/0x30/0x34` LOCAL pos and `node+0x3c..0x64` LOCAL orient 3×3 (16.16) are
  written every tick by physics (`object_physics_update_transforms`@0x08105aa0) and copied
  to the world cache `+0x9c`/`+0xac` every frame by
  `sim_sector_refresh_object_world_transforms`@0x08073fe0 — **no visibility gate**, so both
  are **live off-screen**. (D)
- For a normal top-level object `world_orient == node+0x3c` (identity sector frame);
  `render_object` only overwrites `+0xac` for special-flag nodes (billboard 0x20 /
  scene-slaved 0x4000 / animated 0x10000000 / reortho 0x200), which ordinary hulls don't
  set. (D)
- Orient convention is column-major (apply the 3×3 transposed), exactly as
  `node_world_xform` already does. (D)

**Phase 0 (live confirm before building):** drive the debug socket on an *off-screen*
moving AI ship and confirm over a few seconds that (1) `node+0x2c` and `node+0x9c` change
in lockstep, (2) `node+0x3c` and `node+0xac` stay byte-identical, (3) a normal hull has
none of bits {0x20,0x200,0x4000,0x10000000} in `node+0x118`. If any fails, revisit.

## Architecture

Three pieces, each independently testable:

### 1. Geometry read — extend `read_object_wireframe` to emit triangles (sector_reader)

`MeshWire` gains `std::vector<uint32_t> tris;` (flat v0,v1,v2 triples, rebased per part
exactly like `edges`). `read_node_mesh` already walks the triangle arrays to build edges;
emit `tris` alongside in the same loop (edges stay, the preview still uses them). Cost: a
small extra index vector. Pure reads, bounded, unchanged transform pipeline.

Output of `read_object_wireframe` is, as today, **world-oriented at read time, centered at
the root world pos**. (Transform is live even off-screen per the foundation above.)

### 2. Hull cache (overlay) — read once, store body-local

A per-object cache (key = object address) holding:
`{ verts (body-local), tris, normals (per-vertex), extent_km, model_id, lod }`.

On first sighting (or when the object's root model id / map-LOD changes), with a small
**per-frame read budget** (e.g. ≤2 new objects/frame, to avoid hitches on 130k-vert
stations):
1. `wire = read_object_wireframe(obj, lod = map_lod)`.
2. Read `R_read = root+0x3c` (the root world orient) from the **same** snapshot.
3. **Un-rotate into body frame:** multiply every vertex by `R_read⁻¹` (= `R_readᵀ`, the
   assembly's overall orientation is exactly `R_read`, so this cancels it). Now the hull is
   in its own body frame, root axis-aligned.
4. Compute **per-vertex normals** from `tris` (accumulate face normals) for fresnel.
5. Upload body-local verts+normals (one VBO) + tris (one EBO) to the GPU (see piece 3).
6. Record `extent_km` (body bbox half-extent ÷ `UNITS_PER_KM`).

Cache entries are evicted when the object leaves the sector/range. A **total geometry
budget** (cap on cached hulls / summed tris) bounds memory; objects over budget fall back
to dots, and the overflow count is surfaced (no silent truncation).

### 3. GPU hull renderer (overlay) — extend `meshgl` for solid + many objects

Extend the `meshgl` namespace (today: one pos-only VBO, line draw) to:
- Vertex format **pos+normal**; a `uModel` uniform (per object) separate from `uViewProj`
  (map camera), so the shader transforms normals.
- An EBO of **triangles**; `glDrawElements(GL_TRIANGLES, …)`.
- Holographic fragment shader: `fresnel = pow(1 - max(dot(N,V),0), k)`;
  `frag = uColor * (baseAlpha + rimGain*fresnel)`; additive blend
  (`GL_SRC_ALPHA, GL_ONE`), `glDepthMask(false)`, depth test off, no cull.
- Keep multiple cached hulls as separate VBO/EBO objects; draw them in a per-frame loop
  (one draw call each) **far→near** (reuse the existing blip depth sort), each with its own
  `uModel` and `uColor`.

Per-object `uModel` = map-space placement built from **live** data each frame:
`Model = Flip · [ R_now | t ]`, where `R_now = root+0x3c` (live, column-major/transposed),
`t = (root+0x2c − player) ` mapped to km, and `Flip = diag(1,-1,-1)` is the map's existing
Y/Z screen convention (folded consistently into the view/model so positions *and*
orientations match the blip placement). `uViewProj` = the orbit camera's perspective×view.

The whole hull pass runs inside one ImGui draw callback (like the current preview),
sequenced in the map's draw list: grid+sphere (ImGui) → **hull pass (GPU)** → dots +
selection boxes + labels (ImGui) on top.

## Data flow (per frame)

```
read_current_sector  → live object list (pos, faction, model id) [cheap, already done]
for each filtered object:
    project center with the orbit camera → screen pos, depth, apparent radius
    if radius < dotThreshold:   draw blip dot (today's path)
    else:                       ensure hull cached (budgeted read); mark for hull pass
hull pass (far→near): for each hull → uModel(live R_now,pos) + uColor(faction) → draw
overlay (ImGui): drop-lines/ticks, dots for sub-threshold, labels, 4-corner selection box
```

Geometry is read at most once per object; every frame only re-reads ~9 ints
(`root+0x3c`/`+0x2c`) per hull and issues one draw call — cheap.

## Interaction / tuning (panel controls)

- **map LOD** slider (hull detail; default coarse-ish, e.g. 3) — re-reads cached hulls.
- dot↔hull pixel threshold and rim/alpha gains: start as constants, expose if needed.
- existing camera (orbit / follow / pan / focus) and filters are unchanged.

## Testing

- **Host logic tests** (`make test`): triangle emission from synthetic meshes; the
  body-local un-rotation (feed a known `R_read`, assert verts come back axis-aligned);
  per-vertex normal accumulation. (Synthetic graphs only — they miss real-game issues.)
- **Visual (the real oracle):** `./x3launch.sh`, ScrollLock, open the 3D map. On-screen and
  off-screen ships/stations show correctly oriented translucent hulls that sit on their
  blips; zoom out → hulls shrink to dots; selection shows the 4-corner box; smooth with a
  sector full of objects.
- **`owire`/socket** for objective placement/orientation checks (Phase 0 probes).

## File-by-file

- `src/sector_reader.h/.cpp` — `MeshWire::tris`; emit it in `read_node_mesh` +
  rebase in `read_object_wireframe`. (No transform changes.)
- `src/overlay.cpp` — hull cache + budget; `meshgl` extended to pos+normal / `uModel` /
  triangle draw / holographic shader; `map3d_panel` dot↔hull branch, hull pass callback,
  4-corner selection box.
- `docs/x3-render-pipeline.md` — record the off-screen-live-transform finding (done).

## Open questions for review

1. dot↔hull threshold feel — a hard switch at one pixel size, or a short fade band?
2. map-LOD: single global slider now (chosen), or jump straight to per-object
   distance-driven LOD?
3. additive-only, or add a faint depth-tested core so very dense stations don't blow out to
   solid white when zoomed in?
