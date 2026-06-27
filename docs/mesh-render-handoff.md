# Handoff: make the X3 overlay mesh preview actually render models

You are picking up a focused sub-project in `~/x3ui`: rendering real ship/station
3D models (as wireframe) in the overlay's **mesh preview** panel. The geometry
reader is **done and correct**; one indirection is missing. Your job is to find
it and make a recognizable wireframe appear when the user left-clicks an object.

This is authorized RE of X3: Terran Conflict / Albion Prelude (32-bit, non-PIE @
0x08048000), a game I own, via Ghidra MCP + an in-game read-only debug socket.

## The approach (decided, don't change it)
Read the model's **CPU geometry from process memory** and project it with our own
camera (`project3d`) into the ImGui draw list. **No engine render calls, no GL
state** — pure reads, so it can't crash. The fragile "drive the engine renderer"
path was abandoned after the scout recipe was wrong in ~6 places.

## What already works (verified + unit-tested)
- `x3::read_mesh_wireframe(mem, obj_addr)` in `src/sector_reader.cpp`: walks
  `obj+0x12c` (model id) → resident model → LOD mesh → int16 vertices (÷16384) →
  triangle list → edges. All via the `Mem`/`pread` abstraction. Test:
  `test_mesh_wireframe` in `tests/test_sector_reader.cpp` (passes).
- `mesh_preview_panel()` in `src/overlay.cpp`: orbit camera, projects the verts,
  draws edges. Shows "no mesh for #N" when the read returns `ok=false`.
- Debug command: `wire <obj_addr>` (in `src/debug_server.cpp`) returns
  `read_mesh_wireframe`'s result (ok / vcount / tris) for a live object.

## Verified engine facts (do NOT re-derive — confirmed via disasm + live reads)
- `g_world_root = *(0x082a0640)` (0x082a0640 is a POINTER to the world struct).
- **Resident model list**: head `*(world+0x8)`; per model: next `*(m+0x4)`, id
  `*(m+0x8)`. Model struct: `+0xc` = LOD/mesh-ptr array, `+0x10` = LOD count(i16).
- Model hashmap (same models): `*(world+0x14)`, keyed by **id+1**
  (`model_hashmap_lookup` @ 0x08079ae0).
- Mesh struct: `+0`=vcount, `+8`=vertex array (stride 0x18; int16 x/y/z at
  +0/+2/+4, ÷16384), `+4`=part count(i16), `+0x10`=part array. Part: `+0`=group
  count(i16), `+4`=groups (stride 0x20). Group: `+0`=tri count, `+4`=tris (stride
  0x10; ints v0/v1/v2 at +0/+4/+8).
- `render_object` @ 0x080762f0: `model = model_load_resolve(*(obj+0x12c)); mesh =
  *(*(model+0xc) + *(obj+0x138)*4)`; then iterates **the active scene's view
  object list**, not the sector's logic list.
- **NEVER call `model_load_resolve` @ 0x08080ca0** from outside the render pass —
  it sets a sticky negative-cache flag `*(world+0xbc)+id*0x1c+0x10 = 1` that
  blocks the engine from loading that model until the sector reloads.

## THE BLOCKER (this is the whole task)
The objects the overlay can select come from the **sector logic list**
(`read_sector_objects` walks `*(sector+0x50)` → child list). Live finding: of 67
ships/stations there, **none had `obj+0x12c` equal to a loaded render-model id**
(e.g. a station reads model id 289, which is not in the 188-entry resident list,
yet the station renders fine in-game). So:

> The selectable LOGIC object's `+0x12c` is a logical/macro id, NOT the resident
> render-model id. The renderer uses a DIFFERENT object (a render node) whose
> `+0x12c` is a real loaded id.

You must find the link from a logic object → its render model (or render node).

## What to investigate (in priority order)
1. **The active scene's view object list** = the renderer's objects, which DO have
   loaded model ids. Resolve it: `world=*(0x082a0640)`; player view `pview =
   *(world+0x64)`; the active scene is the renderable one whose `+0x1c == pview`
   (scan the sector list `*(world+0x18)`, each sector node's scene list at
   `node+0x1c`, pick scene with `(*(scene+0x244)&1)` and valid `*(scene+0x24c)`).
   Its object list head = `*(pview+8)`. Walk it (next `*o`), and for each render
   node read `+0x12c` (should be a loaded id) and `+0x08` (object id?). **Compare
   render nodes to logic objects**: do they share `+0x08` (object id)? Is there a
   pointer on the logic object to its render node (scan logic obj fields for a
   pointer whose `+0x12c` is a loaded id)? Find the correspondence.
2. **Macro→model id transform.** The model-RE agent found `model_name_to_id_meshgrp`
   @ 0x08064a70 and a `(group-1)*100000 + local` component-id scheme, plus
   `model_id_to_name` @ 0x08064530 (id→filename, index = id if <1000). Decompile
   these + the render path to see if the logic id 289 maps to a render model id by
   a transform you can compute.
3. **Decompile `render_object`'s callers** — `render_layer_objects` @ 0x080775a0
   and `build_visible_object_list` @ 0x08074280 — to see exactly which object set
   the renderer walks and how those nodes relate to the logic objects (shared id?
   a parallel array? a `+0x6c` transform-node link? logic obj has a render-node
   pointer?).

## How to verify live (the game must be in active 3D flight)
Debug socket on `127.0.0.1:7373`. Use `tools/x3dbg <cmd>` or a python socket
(send `"<cmd>\n"`, read the reply; for multi-line replies like `sector`, drain
until idle). Commands: `read 0x<addr>` (→ `ok 0x<hex> <dec>`), `bytes`, `cstr`,
`sector`, `children <addr>`, `wire <addr>`, `meshscene`. The socket is serviced on
the engine MAIN thread between frames, so it's race-free but ONLY answers while
the game is rendering (it returns `err timeout` in menus / when unfocused).

Concrete experiment to start: in flight, run `meshscene` to get the active scene,
walk `*(view+8)` (view from the scene), dump each render node's `+0x08` (id) and
`+0x12c` (model id); separately walk the sector logic list and dump each logic
object's `+0x08` and `+0x12c`. Find the join (id match? pointer? offset). Once you
can go logic-obj → render-model-id, plug it into `read_mesh_wireframe` (it already
walks model→mesh→verts correctly from the id).

## Acceptance
`wire <a nearby station/ship addr>` returns `wire.ok=1 v=<hundreds> tris=<...>`,
and selecting that object in the overlay shows a recognizable wireframe (drag to
orbit). Far/not-yet-loaded objects showing "no mesh" is acceptable.

## Fallback if the static link can't be found
Hook the engine render to snapshot the mapping: interpose / patch around
`set_object_modelview_matrices` (@ 0x08160ee0, called per object with the mesh) or
capture `model_load_resolve`'s return per object during a render frame, recording
`logic_obj → model_ptr` into a side table the overlay reads. Higher-risk
(in-render hooking) but definitive.

## Pointers
- Memory: `memory/x3ui-mesh-render-parked.md`, `x3-mesh-geometry-wireframe.md`,
  `x3ui-project.md`. Ghidra has the renames (model_hashmap_lookup,
  model_id_to_name, model_resident_slot_get/set, etc.).
- Rule: when you RE a `FUN_`/`DAT_`, rename it via Ghidra MCP (build the knowledge
  base). Verify every offset live before trusting it — past scout recipes were
  repeatedly wrong about this subsystem.
