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

## Orientation convention & animated attachments

### TL;DR — our overlay must TRANSPOSE the orient (D)
The engine consumes the node's 3×3 orient **column-major**. Our overlay
(`node_world_xform` / `mat_apply`) currently applies it **row-major**, i.e. the
**transpose**. The static hull looks fine only because the orbit camera re-frames a
whole-object rotation; child turret/attachment nodes carry a non-trivial *relative*
orient, so the transpose mis-positions AND mis-orients exactly those parts (bug #1).
**Fix: transpose `R` before applying** — read `R[i][j] = mem[+0xac + j*0x10 + i*4]`
(swap i/j vs the current code), or equivalently apply `out_i = Σ_j R[j][i]·v_j`.

### Exact arithmetic (D) — `set_object_modelview_matrices` @0x08160ee0 (disasm @0x08161313+)
GL matrices are column-major: `m[k]`, k = col·4 + row. The default path writes:
```
m[0]  = orient(+0xac)·f      m[4]  = orient(+0xbc)·f      m[8]  = orient(+0xcc)·f   <- row 0 of each GL col
m[1]  = orient(+0xb0)·f      m[5]  = orient(+0xc0)·f      m[9]  = orient(+0xd0)·f   <- row 1
m[2]  = orient(+0xb4)·f      m[6]  = orient(+0xc4)·f      m[10] = orient(+0xd4)·f   <- row 2
            ^GL COLUMN 0           ^GL COLUMN 1                 ^GL COLUMN 2
m[12]=pos(+0x9c)·vs   m[13]=pos(+0xa0)·vs   m[14]=pos(+0xa4)·vs    (vs = *(view+0x2c))
```
So the engine's stride-0x10 triples (`+0xac/+0xb0/+0xb4`, `+0xbc/+0xc0/+0xc4`,
`+0xcc/+0xd0/+0xd4`) are the **COLUMNS** of the GL matrix, not the rows. A column-major
matrix maps a column vector as `out[row] = Σ_col m[col·4+row]·v[col]`, hence:

> **`v_obj[i] = Σ_j  orient[+0xac + j*0x10 + i*4] · v_j`  (column-major).**
> This is the TRANSPOSE of the naive `Σ_j orient[+0xac + i*0x10 + j*4]·v_j` our overlay does.

- **Scale multiplies the COLUMNS** (not rows): `fixed16_mul(node+0x6c, 0x10000)·(node+0x70)`
  scales m[0..2] (col 0), `…·(+0x74)` scales m[4..6] (col 1), `…·(+0x78)` scales m[8..10]
  (col 2). (D, @0x081613d6/0x08161406/0x0816142c). For a pure rotation×uniform-scale this
  is the same whether you scale rows or columns, so scale is not the bug; the *orient
  transpose* is.
- **Translation is the last column** (m[12]/m[13]/m[14] = `+0x9c/+0xa0/+0xa4`). (D)

### Cross-check helpers (D)
The engine ships BOTH conventions as helpers and uses each where appropriate:
- `mat3x3_transform_vec_fixed_rowmajor` @0x080beda0: `out_i = Σ_j M[i*0x10+j*4]·v_j`.
- `mat3x3_transpose_transform_vec_fixed` → renamed `mat3x3_transform_vec_fixed_colmajor`
  @0x080bee90: `out_i = Σ_j M[j*0x10+i*4]·v_j` — **this** is the convention that matches
  how `+0xac` is laid into the GL columns. (The shadow path in `render_object` uses the
  col-major one on `mesh+8` vs `obj+0xac`, consistent with the above.)

### Turrets / attachments have NO separate aim matrix (D) — `render_object` @0x080762f0
`render_object` **recomputes the node's world orient `+0xac..+0xd4` every frame, in place,
just before drawing**, picking a branch from `node+0x118` flags:
- `&0x20` → billboard: orient derived from the world-position delta (spaceflies/sprites).
- `&0x4000 && !&0x4000000` → copy `scene+0x3c` directly, or `mat3x3_mul3x3_fixed(obj+0x3c,
  scene+0x3c, obj+0xac)` (local × root).
- `&0x10000000` → **animated**: `build_mat3x3_from_euler_fixed(scene+0x248 spin angle)` then
  `mat3x3_mul3x3_fixed(... , obj+0xac)`.
- `&0x200` → re-orthonormalize from `+0xbc/+0xc0/+0xc4`.

Key consequence: **aimable turrets/weapons do NOT get a separate turret-aim matrix composed
at draw time. Their final aimed world orientation is BAKED into `+0xac` by this per-frame
pass.** So reading `+0xac` is the right source (no extra matrix to compose) — the only
caveats are (a) it can be **one frame stale** if the engine paused/just spawned the node, and
(b) we were reading it **transposed**. Also note `scene` is passed UNCHANGED down the
recursion, so `scene+0x3c` is the **fixed ROOT orientation**, not a per-parent accumulation;
each node's `+0xac` is `local(+0x3c) × root`, already a *full world* orient — our overlay is
right to treat `+0xac` as world (no parent chaining needed), it just needs the transpose.

## Model load / why something may not render

`render_object` resolves a node's model with `model_load_resolve(node+0x12c)` **once
`node+0x118 & 2` (frustum-visible) is set** — there is **no distance/LOD gating inside the
call**; visibility alone triggers it. (D, @0x08076xxx draw gate)

`model_load_resolve` @0x08080ca0 returns the resident model only if (D):
1. **Hashmap hit** — `model_hashmap_lookup` on `*(world+0x14)` keyed by `model_id + 1`
   returns the already-resident model (the streamed-in / previously-loaded case). This is the
   normal renderer path.
2. else it **loads from disk on the spot** (`.pbb/.bob` via `bob_model_load_meshes`, or
   `.pbd/.bod` via FUN_0807e1e0) and inserts into the hashmap.

It returns **0** when: `id < 0`; the **negative-cache** `resident-slot[id]+0x10 == 1` (a prior
load FAILED — it refuses to retry, set via `model_resident_slot_set(world,id,1)` on every
failure path); or the disk load itself fails. (D)

So for the **Laser Tower not drawing**:
- "Not loaded ⇒ nothing to draw" **is expected** for objects whose node never received
  `+0x118 & 2` (off-screen / not in the active render scene) — the model is simply absent
  from memory and there is **no static fallback geometry** the engine would draw instead.
- If the id genuinely failed to load (bad/missing asset id), it is now **neg-cached** and will
  read 0 forever this session — which is exactly why we must **NOT** call `model_load_resolve`
  externally (it poisons that neg-cache).
- **What an external read-only reader CAN do:** look up the model directly in the resident
  hashmap at `*(world+0x14)` with key `model_id + 1`. If present, read its geometry
  (`+0xc`=LOD array, `+0x10`=LOD count; mesh `+8`=verts, `+0x10`=parts). If absent, the
  geometry is **not in memory** and cannot be read without forcing a load — so a distant /
  unspawned / failed-asset object legitimately has nothing for us to draw.

## Ghidra renames from the render-pipeline RE
`fixed16_mul`@0x08173e20, `node_install_model`@0x08082cb0,
`node_build_collision_mesh`@0x080778c0, `g_inv16384_vert_scale`@DAT_081999ec,
`g_fixed16_to_float`@0x08191b50, `g_identity_mat4`@DAT_081b42a0,
`mat3x3_mul3x3_fixed`@0x080bfd00, `build_mat3x3_from_euler_fixed`@0x080be130,
`mat3x3_transform_vec_fixed_rowmajor`@0x080beda0,
`mat3x3_transform_vec_fixed_colmajor`@0x080bee90 (was mat3x3_transpose_transform_vec_fixed),
`set_object_modelview_matrices_default`@0x08160eb0, `model_load_resolve`@0x08080ca0.
