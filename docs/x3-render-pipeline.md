# X3AP engine render pipeline — verified analysis

Authoritative notes for matching the overlay's `read_object_wireframe` to what the
engine actually draws. Every claim below is either (D) decompiled in Ghidra or (L)
live-verified via the debug socket. Image base 0x08048000. DO NOT call
`model_load_resolve` @0x08080ca0 live (poisons a neg-cache); read memory only.

## Two object representations (reconcile this first)
- **Logic object** (sector payload list, what the overlay selects): `obj+0x12c` is a
  constant logical tag (289), NOT a model id. (L)
- **Transform/scene node** = `*(obj+0x6c)`. THIS is the thing the engine's
  `render_object` walks. Its `+0x12c` is a real resident model id; its `+0xc` is the
  child-node list; it carries the per-node world transform. Below, "node" = this. (D,L)

## Drawable enumeration — `render_object` @0x080762f0 (D)
```
render_object(node):
  if (node+0x118 & 2):                         # per-frame VISIBLE flag (see note)
     model = model_load_resolve(node+0x12c)    # we read the resident list instead
     if model and (int)node+0x12c >= 0:
        mesh = *(*(model+0xc) + (node+0x138)*4) # LOD mesh, idx = node+0x138 (cached)
        for p in *(short*)(mesh+4) parts at *(mesh+0x10):
           render_mesh_part(node, p)            # gated, see below
  for child in node+0xc list (next=*child):    # recurse ALWAYS (even if not drawn)
     render_object(child)
```
- **Node visible gate `node+0x118 & 2`:** per-FRAME visibility (frustum/cull). (L:
  on-screen station hull=0x1001002 has it; off-screen ships have it CLEAR → if we gate
  on it, off-screen objects vanish.) **For a static orbit preview we must NOT use it.**
- **LOD:** the engine uses the cached index `node+0x138` into `(model+0xc)[idx]`; it
  does not recompute per draw. Clamp to `[0, *(short*)(model+0x10))`. `node+0x138 == -1`
  ⇒ no mesh. (D)

## Per-part gate — `render_mesh_part` @0x081665b0 (D)
A loader-part (`mesh+0x10[p]`, 0x58 bytes) is DRAWN only if:
- `part+0x50 != 0` (the render-part group; **NULL ⇒ collision/invisible ⇒ skipped**), AND
- `(part+0x4c & 0x8000) == 0` AND `(part+0x4c & 0x20) == 0`. (D)
(L: station collision parts id 20055/10496/535 have `part+0x50==0`; real meshes have a ptr.)
This is a STATIC property (unlike `+0x118&2`), so it is our correct "is-real-mesh" gate.

## Transform / modelview — `set_object_modelview_matrices` @0x08160ee0 (D) + (L)
Fixed-point: vertices ÷16384 (2^14, `DAT_081999ec`=1/16384); orientation & per-axis
scale are 16.16 (÷65536, `g_fixed16_to_float`@0x08191b50). `fixed16_mul(a,b)=(a*b+0x8000)>>16`.

Per node the engine builds: `worldVert = worldOrient × (axisScale ⊙ vert) + worldPos × view_scale`,
- **worldOrient** = `node+0xac .. +0xd4` (3x3, 16.16, row stride 0x10) — the CACHED world
  orientation (`render_object` pre-bakes `local(+0x3c) × parent` into `+0xac`). (D)
- **worldPos** = `node+0x9c / +0xa0 / +0xa4` (cached world position). (D) NOT the local
  `+0x2c/0x30/0x34`.
- **axisScale** = `fixed16_mul(node+0x6c, 0x10000) × (node+0x70|74|78 / 65536) × view_scale`;
  `node+0x6c` = the model SIZE SCALAR (`model.LOD[0]+0x14`, set in `node_install_model`
  @0x08082cb0); `node+0x70/74/78` (per-axis scale) are usually 1.0. (D)

### LIVE VERIFICATION of the size scalar (the station-scatter bug) (L)
Equipment Dock, root world pos = (1885144, 314098, 4088928):

| model | vert ext (raw) | +0x6c raw | +0x6c/65536 | worldPos − root |
|---|---|---|---|---|
| 21418 (hull) | 16384 (=1.0) | 1,133,444 | 17.29 | (0,0,0) |
| 21419 | 8657 | 150,544 | 2.29 | (−83k, 146k, −997k) |
| 901000003 | 1375 | 22,099 | 0.34 | (−84k, −246k, 1070k) |
| 10496 (collision) | 16384 | 4,406 | 0.07 | (−120k, −274k, 1051k) |

With `+0x6c` applied **RAW**: hull spans `±1.0 × 1,133,444 ≈ ±1.13M`; components sit at
worldPos ≈ ±1.0M → ON the hull ⇒ combined. With `+0x6c/65536`: hull ±17 vs components
±1M ⇒ floats (the current bug). **Therefore `+0x6c` is the raw size multiplier**, and
worldPos `+0x9c` is in the same (game-world) unit as `vert/16384 × +0x6c_raw`.

## Recipe for the overlay (object-local wireframe)
For the selected object, `root = *(obj+0x6c)`, `rwp = root+0x9c/a0/a4`. Walk the node tree
(child `+0x0c`, sibling `+0x00`; visited-set + caps). For each node:
1. **Draw gate (static):** read the model's LOD mesh `(model+0xc)[lod]` (lod = our slider
   or `node+0x138`); for each part keep it only if `part+0x50 != 0` and
   `(part+0x4c & 0x8020)==0`. (Skip the per-frame `+0x118&2` gate.) Also skip primitive
   proxies `node+0x12c < 1000`.
2. **Transform** each kept part's verts:
   `v_obj = worldOrient(+0xac,÷65536) × ( (rawvert/16384) × (+0x6c raw) ) + (node+0x9c − rwp)`.
3. Union into the wireframe; the overlay's bbox-normalize handles the large magnitudes
   (so the finiteness guard must allow ≥ ~1e8, not 1e6).

OPEN: some ships still over-draw — likely multiple whole-object LOD subtrees in the node
graph; the engine draws one (the `+0x138`-selected). To dedup, only descend the LOD child
the engine would (TODO: confirm the LOD-group selector field on the parent node).

## Ghidra renames from the render-pipeline RE
`fixed16_mul`@0x08173e20, `node_install_model`@0x08082cb0,
`node_build_collision_mesh`@0x080778c0, `g_inv16384_vert_scale`@DAT_081999ec,
`g_fixed16_to_float`@0x08191b50, `g_identity_mat4`@DAT_081b42a0,
`mat3x3_mul3x3_fixed`@0x080bfd00, `build_mat3x3_from_euler_fixed`@0x080be130.
