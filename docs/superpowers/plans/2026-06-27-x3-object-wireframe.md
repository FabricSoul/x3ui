# X3 Object Wireframe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render a recognizable orbiting wireframe of a selected ship/station/asteroid by walking its scene graph from `obj+0x6c` and unioning every part's mesh, all via pure process-memory reads.

**Architecture:** Refactor the existing `read_mesh_wireframe` so one node's mesh can be read at any LOD. Add `read_object_wireframe` that DFS-walks the transform-node subtree (child `+0x0c`, sibling `+0x00`, visited-set, caps), reads each node's mesh at finest LOD, composes parent→child transforms, and merges into one `MeshWire`. Swap the overlay preview to it. Add an `owire` debug command. All reads go through the `Mem`/`pread` abstraction, so a stale pointer yields `{ok=false}`, never a crash.

**Tech Stack:** C++17, no new deps. Host unit tests via `tests/fake_mem.h` + `tests/check.h` (`make test`). Overlay builds 32-bit via `nix-shell --run 'make'`. Live verification via the in-game debug socket (`tools/x3dbg`, `./x3launch.sh`).

**Design reference:** `docs/superpowers/specs/2026-06-27-x3-object-wireframe-design.md`

---

## File Structure

- **Modify** `src/sector_reader.cpp` — extract `read_node_mesh` (LOD selector); add `read_object_wireframe` + transform helpers.
- **Modify** `src/sector_reader.h` — declare `read_object_wireframe`.
- **Modify** `tests/test_sector_reader.cpp` — new tests + `RUN()` registrations.
- **Modify** `src/debug_server.cpp` — add `owire` command.
- **Modify** `src/overlay.cpp` — call `read_object_wireframe` in `mesh_preview_panel`.

Verified node-struct offsets (relative to a scene/transform node; `root = *(obj+0x6c)`):
`+0x00` next sibling · `+0x0c` first child · `+0x2c/0x30/0x34` local translation (int32) ·
`+0x3c..0x64` local 3x3 orientation, 16.16, row stride `0x10` · `+0x70/0x74/0x78` local scale, 16.16 ·
`+0x12c` model id · `+0x138` per-node LOD index. Resident model list + mesh layout are already in `read_mesh_wireframe`.

---

## Task 1: Refactor LOD selection into `read_node_mesh` (no behavior change)

**Files:**
- Modify: `src/sector_reader.cpp:22-93` (split `read_mesh_wireframe`)

- [ ] **Step 1: Run the existing tests to confirm a green baseline**

Run: `make test`
Expected: ends with `N checks, 0 failures` (twice — sector + debug server).

- [ ] **Step 2: Replace `read_mesh_wireframe` with a thin wrapper over a new static `read_node_mesh`**

In `src/sector_reader.cpp`, replace the whole body of `read_mesh_wireframe` (lines 22-93) with:

```cpp
// LOD selection for one node's mesh.
enum LodSel { LOD_NODE = 0, LOD_COARSEST = 1, LOD_FINEST = 2 };

// Read ONE node/object's mesh from resident CPU model data. `node` supplies the
// model id (+0x12c) and (for LOD_NODE) the LOD index (+0x138). Pure reads, bounded.
static MeshWire read_node_mesh(const Mem& mem, uint32_t node, int lodsel,
                               size_t vcap, size_t ecap) {
    MeshWire w;
    uint32_t id = 0;
    if (!ru32(mem, node + 0x12c, id) || (int32_t)id < 0) return w;
    uint32_t world = 0;
    if (!ru32(mem, 0x082a0640u, world) || !world) return w;
    uint32_t model = 0, mm = 0;
    ru32(mem, world + 0x8, mm);
    for (int g = 0; mm && g < 20000; ++g) {
        uint32_t mid = 0;
        if (ru32(mem, mm + 0x8, mid) && (int32_t)mid == (int32_t)id) { model = mm; break; }
        if (!ru32(mem, mm + 0x4, mm)) break;
    }
    if (!model) return w;

    uint32_t lodarr = 0, lodcnt = 0, lod = 0;
    if (!ru32(mem, model + 0xc, lodarr) || !lodarr) return w;
    ru32(mem, model + 0x10, lodcnt); lodcnt &= 0xFFFF;
    if (lodsel == LOD_COARSEST)    lod = lodcnt ? lodcnt - 1 : 0;
    else if (lodsel == LOD_FINEST) lod = 0;
    else { if (!ru32(mem, node + 0x138, lod)) lod = 0;
           if (lodcnt && lod >= lodcnt) lod = lodcnt - 1; }
    uint32_t mesh = 0;
    if (!ru32(mem, lodarr + lod * 4, mesh) || !mesh) return w;

    uint32_t vcount = 0, verts = 0;
    if (!ru32(mem, mesh + 0x0, vcount) || !vcount || vcount > vcap) return w;
    if (!ru32(mem, mesh + 0x8, verts) || !verts) return w;
    w.verts.reserve(vcount * 3);
    for (uint32_t i = 0; i < vcount; ++i) {
        uint32_t a = 0, b = 0;
        if (!ru32(mem, verts + i * 0x18 + 0, a) || !ru32(mem, verts + i * 0x18 + 4, b)) {
            w.verts.clear(); return w;
        }
        int16_t x = (int16_t)(a & 0xFFFF), y = (int16_t)(a >> 16), z = (int16_t)(b & 0xFFFF);
        w.verts.push_back(x / 16384.0f);
        w.verts.push_back(y / 16384.0f);
        w.verts.push_back(z / 16384.0f);
    }

    uint32_t pcw = 0, parts = 0;
    ru32(mem, mesh + 0x4, pcw); int pcount = (int16_t)(pcw & 0xFFFF);
    ru32(mem, mesh + 0x10, parts);
    auto add_edge = [&](uint32_t i0, uint32_t i1) {
        if (i0 < vcount && i1 < vcount) { w.edges.push_back(i0); w.edges.push_back(i1); }
    };
    for (int p = 0; p < pcount && parts && w.edges.size() < ecap * 2; ++p) {
        uint32_t part = 0;
        if (!ru32(mem, parts + p * 4, part) || !part) continue;
        uint32_t gcw = 0, groups = 0;
        ru32(mem, part + 0x0, gcw); int gcount = (int16_t)(gcw & 0xFFFF);
        ru32(mem, part + 0x4, groups);
        for (int gi = 0; gi < gcount && groups && w.edges.size() < ecap * 2; ++gi) {
            uint32_t grp = groups + (uint32_t)gi * 0x20;
            uint32_t tcount = 0, tris = 0;
            ru32(mem, grp + 0x0, tcount); ru32(mem, grp + 0x4, tris);
            if (!tris || tcount > ecap) continue;
            for (uint32_t t = 0; t < tcount && w.edges.size() < ecap * 2; ++t) {
                uint32_t tri = tris + t * 0x10, v0 = 0, v1 = 0, v2 = 0;
                if (!ru32(mem, tri + 0, v0) || !ru32(mem, tri + 4, v1) || !ru32(mem, tri + 8, v2)) break;
                add_edge(v0, v1); add_edge(v1, v2); add_edge(v2, v0);
            }
        }
    }
    w.ok = !w.verts.empty();
    return w;
}

MeshWire read_mesh_wireframe(const Mem& mem, uint32_t obj, bool coarsest,
                             size_t vcap, size_t ecap) {
    return read_node_mesh(mem, obj, coarsest ? LOD_COARSEST : LOD_NODE, vcap, ecap);
}
```

- [ ] **Step 3: Run tests — existing `test_mesh_wireframe` must still pass**

Run: `make test`
Expected: `0 failures`. (`test_mesh_wireframe` calls `read_mesh_wireframe(..)` with default `coarsest=true` → `LOD_COARSEST` → same LOD[1] as before.)

- [ ] **Step 4: Commit**

```bash
git add src/sector_reader.cpp
git commit -m "refactor: extract read_node_mesh(lodsel) from read_mesh_wireframe"
```

---

## Task 2: `read_object_wireframe` — walk + union + finest LOD (no transforms yet)

**Files:**
- Modify: `src/sector_reader.h:98-99` (add declaration)
- Modify: `src/sector_reader.cpp` (add function + `#include <unordered_set>`)
- Test: `tests/test_sector_reader.cpp`

- [ ] **Step 1: Declare the function in the header**

In `src/sector_reader.h`, immediately after the `read_mesh_wireframe` declaration (line 99), add:

```cpp
// Assemble a whole object's wireframe by walking its scene graph from the
// transform node (obj+0x6c): DFS children (node+0x0c) and siblings (node+0x00),
// reading each mesh-bearing node at finest LOD and (Task 3) composing transforms.
// Pure reads + bounded (node_cap / vcap / ecap). Proxy and unloaded nodes are
// skipped. Returns {ok=false} if no part resolved.
MeshWire read_object_wireframe(const Mem& mem, uint32_t obj_addr,
                               size_t node_cap = 512, size_t vcap = 200000,
                               size_t ecap = 600000);
```

- [ ] **Step 2: Write the failing tests**

In `tests/test_sector_reader.cpp`, add these three tests just before `int main()` (line 330). They build minimal resident models (verts only — `read_node_mesh` returns `ok` on non-empty verts even with no triangles):

```cpp
// --- object wireframe (scene-graph walk) ---------------------------------------
// Register a resident model `id` with ONE LOD of `nv` verts at (vx,vy,vz) into the
// world model list, chaining onto `*chain_field`. Advances `*alloc`. No triangles.
static uint32_t reg_model(FakeMem& fm, uint32_t id, int nv,
                          float vx, float vy, float vz,
                          uint32_t& alloc, uint32_t& chain_field) {
    uint32_t model = alloc; alloc += 0x100;
    uint32_t lodarr = alloc; alloc += 0x40;
    uint32_t mesh = alloc; alloc += 0x40;
    uint32_t vbuf = alloc; alloc += (uint32_t)nv * 0x18 + 0x20;
    fm.w(chain_field, model);                 // link previous .next (or world head) -> model
    fm.w(model + 0x8, id);
    fm.w(model + 0x4, 0);                      // this model's .next = end (until another is added)
    chain_field = model + 0x4;
    fm.w(model + 0xc, lodarr);
    fm.w(model + 0x10, 1);                     // 1 LOD
    fm.w(lodarr + 0, mesh);
    fm.w(mesh + 0, (uint32_t)nv);
    fm.w(mesh + 8, vbuf);
    fm.w(mesh + 4, 0);                         // 0 parts -> no edges (fine for these tests)
    int16_t ix = (int16_t)(vx * 16384.0f), iy = (int16_t)(vy * 16384.0f), iz = (int16_t)(vz * 16384.0f);
    for (int i = 0; i < nv; ++i) {
        fm.w(vbuf + i * 0x18 + 0, ((uint32_t)(uint16_t)iy << 16) | (uint16_t)ix);
        fm.w(vbuf + i * 0x18 + 4, (uint16_t)iz);
    }
    return mesh;
}

static void test_object_walk_union() {
    FakeMem fm;
    uint32_t obj = 0x5000, alloc = 0x60000;
    fm.w(0x082a0640, 0x100);                   // world root ptr
    uint32_t chain = 0x100 + 0x8;              // list head field
    reg_model(fm, 10, 1, 1.0f, 0.0f, 0.0f, alloc, chain);   // root's model: vert (1,0,0)
    reg_model(fm, 11, 1, 0.0f, 1.0f, 0.0f, alloc, chain);   // child's model: vert (0,1,0)

    uint32_t root = 0x4000, child = 0x4200, proxy = 0x4400;
    fm.w(obj + 0x6c, root);                     // obj -> transform node
    fm.w(root + 0x12c, 10);                     // root carries model 10
    fm.w(root + 0xc, child);                    // root.firstChild -> child
    fm.w(child + 0x12c, 11);                    // child carries model 11
    fm.w(child + 0x00, proxy);                  // child.nextSibling -> proxy
    fm.w(proxy + 0x12c, 999);                   // proxy model id not resident -> skipped
    fm.w(proxy + 0x00, child);                  // proxy.nextSibling -> child (cycle: must terminate)

    auto w = x3::read_object_wireframe(fm.mem(), obj);
    CHECK(w.ok);
    CHECK_EQ(w.vcount(), (size_t)2);            // root vert + child vert; proxy skipped, no infinite loop
    // root vert (1,0,0) and child vert (0,1,0) both present (order independent)
    bool hasRoot = false, hasChild = false;
    for (size_t i = 0; i + 2 < w.verts.size() + 1; i += 3) {
        if (w.verts[i] > 0.99f && w.verts[i+1] < 0.01f) hasRoot = true;
        if (w.verts[i] < 0.01f && w.verts[i+1] > 0.99f) hasChild = true;
    }
    CHECK(hasRoot); CHECK(hasChild);
}

static void test_object_finest_lod() {
    FakeMem fm;
    uint32_t obj = 0x5000;
    fm.w(0x082a0640, 0x100);
    fm.w(0x100 + 0x8, 0x6000);                  // head -> model
    fm.w(0x6000 + 0x8, 7);                      // model id 7
    fm.w(0x6000 + 0x4, 0);
    fm.w(0x6000 + 0xc, 0x6100);                 // lodarr
    fm.w(0x6000 + 0x10, 2);                     // 2 LODs
    fm.w(0x6100 + 0, 0x6200);                   // lod[0] FINEST -> meshFine
    fm.w(0x6100 + 4, 0x6300);                   // lod[1] coarsest -> meshCoarse
    fm.w(0x6200 + 0, 2); fm.w(0x6200 + 8, 0x6280); fm.w(0x6200 + 4, 0);  // fine: 2 verts
    fm.w(0x6280 + 0 * 0x18 + 0, 0); fm.w(0x6280 + 0 * 0x18 + 4, 0);
    fm.w(0x6280 + 1 * 0x18 + 0, 0); fm.w(0x6280 + 1 * 0x18 + 4, 0);
    fm.w(0x6300 + 0, 1); fm.w(0x6300 + 8, 0x6380); fm.w(0x6300 + 4, 0);  // coarse: 1 vert
    fm.w(0x6380 + 0, 0); fm.w(0x6380 + 4, 0);

    uint32_t root = 0x4000;
    fm.w(obj + 0x6c, root);
    fm.w(root + 0x12c, 7);                       // single-node object
    auto w = x3::read_object_wireframe(fm.mem(), obj);
    CHECK(w.ok);
    CHECK_EQ(w.vcount(), (size_t)2);             // finest LOD (2 verts), not coarsest (1)
}

static void test_object_node_cap() {
    FakeMem fm;
    uint32_t obj = 0x5000, alloc = 0x60000;
    fm.w(0x082a0640, 0x100);
    uint32_t chain = 0x100 + 0x8;
    reg_model(fm, 10, 1, 0.0f, 0.0f, 0.0f, alloc, chain);
    uint32_t root = 0x4000;
    fm.w(obj + 0x6c, root);
    fm.w(root + 0x12c, 10);
    // a long sibling chain of children, all model 10
    uint32_t prev_sib_field = root + 0xc;        // root.firstChild
    for (int i = 0; i < 20; ++i) {
        uint32_t n = 0x40000 + (uint32_t)i * 0x40;
        fm.w(prev_sib_field, n);
        fm.w(n + 0x12c, 10);
        prev_sib_field = n + 0x00;               // chain via nextSibling
    }
    auto w = x3::read_object_wireframe(fm.mem(), obj, /*node_cap=*/4);
    CHECK(w.vcount() <= (size_t)4);              // bounded; never hangs
}
```

- [ ] **Step 3: Register the tests and run to verify they fail**

In `tests/test_sector_reader.cpp` `main()`, after `RUN(test_mesh_wireframe);` (line 338) add:

```cpp
    RUN(test_object_walk_union);
    RUN(test_object_finest_lod);
    RUN(test_object_node_cap);
```

Run: `make test`
Expected: FAIL to link/compile — `read_object_wireframe` is declared but not defined (undefined reference).

- [ ] **Step 4: Implement the walk + union (no transforms yet)**

In `src/sector_reader.cpp`, add `#include <unordered_set>` near the top includes (after `#include <cstdlib>`), then add this function right after `read_mesh_wireframe`:

```cpp
MeshWire read_object_wireframe(const Mem& mem, uint32_t obj_addr,
                               size_t node_cap, size_t vcap, size_t ecap) {
    MeshWire out;
    uint32_t root = 0;
    if (!ru32(mem, obj_addr + 0x6c, root) || !root) return out;

    std::unordered_set<uint32_t> seen;
    std::vector<uint32_t> stack;            // DFS over nodes
    stack.push_back(root);
    seen.insert(root);
    size_t visited = 0;

    while (!stack.empty() && visited < node_cap) {
        uint32_t node = stack.back(); stack.pop_back();
        ++visited;

        MeshWire part = read_node_mesh(mem, node, LOD_FINEST, vcap, ecap);
        if (part.ok && out.verts.size() / 3 < vcap && out.edges.size() / 2 < ecap) {
            uint32_t base = (uint32_t)(out.verts.size() / 3);
            for (float v : part.verts) out.verts.push_back(v);      // Task 3 will transform
            for (uint32_t e : part.edges) out.edges.push_back(base + e);
        }

        // enqueue children: head = node+0x0c, walk its sibling list (next = c+0x00)
        uint32_t c = 0;
        ru32(mem, node + 0x0c, c);
        for (size_t g = 0; c && g < node_cap; ++g) {
            if (seen.insert(c).second) stack.push_back(c);
            if (!ru32(mem, c + 0x00, c)) break;
        }
    }
    out.ok = !out.verts.empty();
    return out;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `make test`
Expected: `0 failures`. The three new tests pass; `test_mesh_wireframe` still passes.

- [ ] **Step 6: Commit**

```bash
git add src/sector_reader.h src/sector_reader.cpp tests/test_sector_reader.cpp
git commit -m "feat: read_object_wireframe walks scene graph + unions parts (finest LOD)"
```

---

## Task 3: Compose parent→child transforms

**Files:**
- Modify: `src/sector_reader.cpp` (transform helpers + use in the walk)
- Test: `tests/test_sector_reader.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/test_sector_reader.cpp`, add before `int main()`:

```cpp
static void test_object_transform_translation() {
    FakeMem fm;
    uint32_t obj = 0x5000, alloc = 0x60000;
    fm.w(0x082a0640, 0x100);
    uint32_t chain = 0x100 + 0x8;
    reg_model(fm, 10, 1, 0.0f, 0.0f, 0.0f, alloc, chain);   // root model: vert at origin
    reg_model(fm, 11, 1, 0.0f, 0.0f, 0.0f, alloc, chain);   // child model: vert at origin

    uint32_t root = 0x4000, child = 0x4200;
    fm.w(obj + 0x6c, root);
    fm.w(root + 0x12c, 10);
    fm.w(root + 0xc, child);
    fm.w(child + 0x12c, 11);
    // child local transform: identity rotation, unit scale, translation (0.5, -0.25, 0)
    fm.w(child + 0x3c + 0 * 0x10 + 0, 0x10000);             // R[0][0] = 1.0
    fm.w(child + 0x3c + 1 * 0x10 + 4, 0x10000);             // R[1][1] = 1.0
    fm.w(child + 0x3c + 2 * 0x10 + 8, 0x10000);             // R[2][2] = 1.0
    fm.w(child + 0x70, 0x10000); fm.w(child + 0x74, 0x10000); fm.w(child + 0x78, 0x10000); // scale 1
    fm.w(child + 0x2c, (uint32_t)(int32_t)(0.5f  * 16384)); //  8192
    fm.w(child + 0x30, (uint32_t)(int32_t)(-0.25f * 16384));// -4096
    fm.w(child + 0x34, 0);

    auto w = x3::read_object_wireframe(fm.mem(), obj);
    CHECK_EQ(w.vcount(), (size_t)2);
    // child's origin vert must land at the translation (0.5, -0.25, 0)
    bool placed = false;
    for (size_t i = 0; i + 2 < w.verts.size() + 1; i += 3)
        if (w.verts[i] > 0.49f && w.verts[i] < 0.51f &&
            w.verts[i+1] > -0.26f && w.verts[i+1] < -0.24f) placed = true;
    CHECK(placed);
}
```

Register it in `main()` after `RUN(test_object_node_cap);`:

```cpp
    RUN(test_object_transform_translation);
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — `test_object_transform_translation`: child vert is at origin (no transform applied yet), `placed` is false.

- [ ] **Step 3: Add transform helpers and apply them in the walk**

In `src/sector_reader.cpp`, add this anonymous namespace just above `read_object_wireframe`:

```cpp
namespace {
// 3x4 affine transform, row-major; column 3 is translation.
struct Mat34 { float m[12]; };
Mat34 mat_identity() { Mat34 r{}; r.m[0] = r.m[5] = r.m[10] = 1.0f; return r; }
Mat34 mat_mul(const Mat34& a, const Mat34& b) {  // a ∘ b
    Mat34 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            r.m[i*4+j] = a.m[i*4+0]*b.m[0*4+j] + a.m[i*4+1]*b.m[1*4+j] + a.m[i*4+2]*b.m[2*4+j];
        r.m[i*4+3] = a.m[i*4+0]*b.m[3] + a.m[i*4+1]*b.m[7] + a.m[i*4+2]*b.m[11] + a.m[i*4+3];
    }
    return r;
}
void mat_apply(const Mat34& m, float x, float y, float z, float& ox, float& oy, float& oz) {
    ox = m.m[0]*x + m.m[1]*y + m.m[2]*z  + m.m[3];
    oy = m.m[4]*x + m.m[5]*y + m.m[6]*z  + m.m[7];
    oz = m.m[8]*x + m.m[9]*y + m.m[10]*z + m.m[11];
}

// Translation fixed-point divisor (model-space units, same 2^14 as vertices).
// CALIBRATE LIVE — see plan Task 6. If parts sit too close/far, adjust this.
constexpr float kTransDiv = 16384.0f;

// One node's local transform = T * R * S, read from the node fields.
Mat34 node_local(const Mem& mem, uint32_t node) {
    auto gi = [&](uint32_t off) -> int32_t { uint32_t v = 0; ru32(mem, node + off, v); return (int32_t)v; };
    float R[3][3];
    int32_t rsum = 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) { int32_t r = gi(0x3c + i * 0x10 + j * 4); rsum |= (r < 0 ? -r : r); R[i][j] = r / 65536.0f; }
    if (rsum == 0) { R[0][0] = R[1][1] = R[2][2] = 1.0f; R[0][1]=R[0][2]=R[1][0]=R[1][2]=R[2][0]=R[2][1]=0.0f; } // not a transform node -> identity
    float s0 = gi(0x70) / 65536.0f, s1 = gi(0x74) / 65536.0f, s2 = gi(0x78) / 65536.0f;
    if (s0 == 0.0f && s1 == 0.0f && s2 == 0.0f) { s0 = s1 = s2 = 1.0f; }
    Mat34 m{};
    for (int i = 0; i < 3; ++i) { m.m[i*4+0] = R[i][0]*s0; m.m[i*4+1] = R[i][1]*s1; m.m[i*4+2] = R[i][2]*s2; }
    m.m[3]  = gi(0x2c) / kTransDiv;
    m.m[7]  = gi(0x30) / kTransDiv;
    m.m[11] = gi(0x34) / kTransDiv;
    return m;
}
} // namespace
```

Then change `read_object_wireframe` to carry a transform per stack frame. Replace its body with:

```cpp
MeshWire read_object_wireframe(const Mem& mem, uint32_t obj_addr,
                               size_t node_cap, size_t vcap, size_t ecap) {
    MeshWire out;
    uint32_t root = 0;
    if (!ru32(mem, obj_addr + 0x6c, root) || !root) return out;

    struct Frame { uint32_t node; Mat34 m; };
    std::unordered_set<uint32_t> seen;
    std::vector<Frame> stack;
    stack.push_back({root, mat_identity()});    // root defines the object frame
    seen.insert(root);
    size_t visited = 0;

    while (!stack.empty() && visited < node_cap) {
        Frame f = stack.back(); stack.pop_back();
        ++visited;

        MeshWire part = read_node_mesh(mem, f.node, LOD_FINEST, vcap, ecap);
        if (part.ok && out.verts.size() / 3 < vcap && out.edges.size() / 2 < ecap) {
            uint32_t base = (uint32_t)(out.verts.size() / 3);
            size_t nv = part.verts.size() / 3;
            bool bad = false;
            std::vector<float> tv; tv.reserve(nv * 3);
            for (size_t i = 0; i < nv && !bad; ++i) {
                float ox, oy, oz;
                mat_apply(f.m, part.verts[i*3+0], part.verts[i*3+1], part.verts[i*3+2], ox, oy, oz);
                if (!std::isfinite(ox) || !std::isfinite(oy) || !std::isfinite(oz) ||
                    std::fabs(ox) > 1e6f || std::fabs(oy) > 1e6f || std::fabs(oz) > 1e6f) bad = true;
                tv.push_back(ox); tv.push_back(oy); tv.push_back(oz);
            }
            if (!bad) {
                for (float v : tv) out.verts.push_back(v);
                for (uint32_t e : part.edges) out.edges.push_back(base + e);
            }
        }

        uint32_t c = 0;
        ru32(mem, f.node + 0x0c, c);
        for (size_t g = 0; c && g < node_cap; ++g) {
            if (seen.insert(c).second)
                stack.push_back({c, mat_mul(f.m, node_local(mem, c))});
            if (!ru32(mem, c + 0x00, c)) break;
        }
    }
    out.ok = !out.verts.empty();
    return out;
}
```

`std::isfinite`/`std::fabs` come from `<cmath>` (already included).

- [ ] **Step 4: Run tests to verify all pass**

Run: `make test`
Expected: `0 failures`. `test_object_transform_translation` now places the child vert at (0.5, -0.25, 0); `test_object_walk_union` still passes (its child has no transform words → `node_local` returns identity).

- [ ] **Step 5: Commit**

```bash
git add src/sector_reader.cpp tests/test_sector_reader.cpp
git commit -m "feat: compose parent->child node transforms in read_object_wireframe"
```

---

## Task 4: `owire` debug command

**Files:**
- Modify: `src/debug_server.cpp` (add handler next to `wire`)

- [ ] **Step 1: Add the `owire` handler**

In `src/debug_server.cpp`, right after the `wire` handler block (the `if (op == "wire") { ... }` ending around line 118), add:

```cpp
    if (op == "owire") {   // whole-object wireframe: owire <obj_addr>
        std::string at; is >> at;
        uint32_t a = parse_addr(at);
        MeshWire w = read_object_wireframe(mem, a);
        snprintf(b, sizeof b, "ok addr=0x%x | obj.ok=%d v=%zu tris=%zu",
                 a, (int)w.ok, w.vcount(), w.edges.size() / 6);
        return b;
    }
```

(`read_object_wireframe` and `MeshWire` are already visible via `#include "sector_reader.h"`.)

- [ ] **Step 2: Verify it still compiles + tests pass**

Run: `make test`
Expected: `0 failures` (the debug-server test target compiles `debug_server.cpp`; no behavior change to existing commands).

- [ ] **Step 3: Commit**

```bash
git add src/debug_server.cpp
git commit -m "feat(debug): owire <addr> returns whole-object wireframe stats"
```

---

## Task 5: Overlay integration

**Files:**
- Modify: `src/overlay.cpp:1221`

- [ ] **Step 1: Swap the preview reader to the object walker**

In `src/overlay.cpp`, in `mesh_preview_panel`, change line 1221 from:

```cpp
        wire = x3::read_mesh_wireframe(make_engine_mem(), g_sel_addr);
```

to:

```cpp
        wire = x3::read_object_wireframe(make_engine_mem(), g_sel_addr);
```

No other changes: the result is the same `MeshWire` type, and the surrounding code already (a) caches per `g_sel_addr`, (b) computes the bounding box / center / extent, and (c) projects `wire.edges` indexing into `wire.verts`.

- [ ] **Step 2: Build the 32-bit overlay**

Run: `nix-shell --run 'make'`
Expected: ends with `=== built libx3ui.so ===` and the inspect output (bare sonames, `SDL_GL_SwapWindow` GLOBAL DEFAULT). No compile errors.

- [ ] **Step 3: Commit**

```bash
git add src/overlay.cpp
git commit -m "feat(overlay): mesh preview renders whole-object wireframe"
```

---

## Task 6: Live calibration + acceptance

This task runs the real game. The overlay reads geometry from process memory, so it
must be rebuilt (Task 5) and the game relaunched to load the new `.so`.

- [ ] **Step 1: Relaunch into flight with the new overlay**

If the game is running, quit it (or `pkill -x X3AP_main`), then:

Run: `./x3launch.sh`
Expected: boots straight into a sector; `tools/x3dbg sector` shows `in_sector=1`.

- [ ] **Step 2: Sanity-check `owire` on a ship and a station**

Get a ship/station address: `tools/x3dbg scene` then `tools/x3dbg children 0x<sector_addr> 80`.
Run: `tools/x3dbg owire 0x<ship_addr>` and `tools/x3dbg owire 0x<station_addr>`.
Expected: `obj.ok=1 v=<thousands> tris=<hundreds+>` (far more than the old proxy box/quad).

- [ ] **Step 3: Eyeball the wireframe and calibrate the translation scale**

In-game: ScrollLock to show the overlay, left-click the ship/station, open "x3ui - mesh preview", drag to orbit.
- If parts sit roughly in the right places → `kTransDiv` is correct; done.
- If parts are bunched at the center or flung far out → adjust `kTransDiv` in `src/sector_reader.cpp` (try `1024.0f` or `65536.0f`), `nix-shell --run 'make'`, `./x3launch.sh`, recheck. Pick the value where a known ship (e.g. a Nova's thrusters at the rear, wings to the sides) looks right.
- If the wireframe is too dense to read in the small panel, change `LOD_FINEST` to a mid level in `read_object_wireframe`'s `read_node_mesh(..., LOD_FINEST, ...)` call (this is the documented finest-vs-mid visual knob) and rebuild.

- [ ] **Step 4: Commit any calibration change**

```bash
git add src/sector_reader.cpp
git commit -m "tune: calibrate node translation scale for object wireframe"
```

- [ ] **Step 5: Acceptance**

Confirm: selecting a nearby ship and a nearby station each shows a recognizable,
orbitable wireframe (hull + parts), and a far/not-loaded object still shows
"no mesh for #N" (graceful). `tools/x3dbg owire <addr>` reports `obj.ok=1` with
thousands of verts for loaded objects.

---

## Notes / out of scope (future)
- LOD-group collapse (one detail level per group) only if overdraw ever costs fps — caps already bound it.
- Solid/textured rendering and the on-disk `.bod` path for not-yet-loaded objects.
