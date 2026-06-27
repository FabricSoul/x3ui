# Holographic Solid Hulls — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render every in-range object in the overlay's 3D map as a solid, engine-oriented, holographic translucent hull (falling back to a blip dot when small on screen), placed every frame from the engine's live transform.

**Architecture:** Read each object's geometry once (`read_object_wireframe`, extended to emit triangles), un-rotate it into body frame by the root orientation, cache it on the GPU; each frame place it with the live `root+0x3c` orientation + `root+0x2c` position. Pure geometry helpers live in `sector_reader` (host-tested); GL + map integration live in `overlay.cpp` (visually verified — the only reliable oracle for this game, per the methodology).

**Tech Stack:** C++17, Dear ImGui, OpenGL 3.0/GLSL 130 via `SDL_GL_GetProcAddress`, custom host test harness (`tests/`).

**Spec:** `docs/superpowers/specs/2026-06-27-holographic-solid-hulls-design.md`
**Engine facts:** `docs/x3-render-pipeline.md`

---

### Task 1: Emit triangle indices from the mesh reader

**Files:**
- Modify: `src/sector_reader.h` (`MeshWire` struct ~line 87)
- Modify: `src/sector_reader.cpp` (`read_node_mesh` ~line 74-103; `read_object_wireframe` rebase ~line 263-267)
- Test: `tests/test_sector_reader.cpp`

- [ ] **Step 1: Add the failing test** — append to `tests/test_sector_reader.cpp` before `int main()`:

```cpp
// --- triangle index emission (for solid rendering) -----------------------------
static void test_object_tri_emission() {
    FakeMem fm;
    uint32_t obj = 0x5000, alloc = 0x60000;
    fm.w(0x082a0640, 0x100);
    uint32_t chain = 0x100 + 0x8;
    reg_model(fm, 20, 3, 0.0f, 0.0f, 0.0f, alloc, chain);   // root: 3 verts + 1 tri
    reg_model(fm, 21, 3, 0.0f, 0.0f, 0.0f, alloc, chain);   // child: 3 verts + 1 tri
    uint32_t root = 0x4000, child = 0x4200;
    fm.w(obj + 0x6c, root);
    fm.w(root + 0x12c, 20);
    fm.w(root + 0xc, child);
    fm.w(child + 0x12c, 21);
    auto w = x3::read_object_wireframe(fm.mem(), obj);
    CHECK(w.ok);
    CHECK_EQ(w.tris.size(), (size_t)6);     // 2 tris * 3 indices
    bool rebased = false;                   // child's tri indices offset into [3,6)
    for (uint32_t t : w.tris) { CHECK(t < w.vcount()); if (t >= 3) rebased = true; }
    CHECK(rebased);
}
```

Register it in `main()` after `RUN(test_object_edge_rebase);`:

```cpp
    RUN(test_object_tri_emission);
```

- [ ] **Step 2: Run it, verify it fails to compile** — `tris` doesn't exist yet.

Run: `make test`
Expected: compile error `'struct x3::MeshWire' has no member named 'tris'`.

- [ ] **Step 3: Add `tris` to `MeshWire`** — in `src/sector_reader.h`, inside `struct MeshWire` after the `edges` line:

```cpp
    std::vector<uint32_t> tris;    // flat index triples into verts (v0,v1,v2,...) for solid fill
```

- [ ] **Step 4: Emit tris in `read_node_mesh`** — in `src/sector_reader.cpp`, in the triangle loop (currently only `add_edge`), add tri index emission. Replace the inner tri loop body (the `add_edge(v0,v1); add_edge(v1,v2); add_edge(v2,v0);` line at ~line 100) with:

```cpp
                add_edge(v0, v1); add_edge(v1, v2); add_edge(v2, v0);
                if (v0 < vcount && v1 < vcount && v2 < vcount) {
                    w.tris.push_back(v0); w.tris.push_back(v1); w.tris.push_back(v2);
                }
```

- [ ] **Step 5: Rebase tris when unioning parts** — in `read_object_wireframe`, where edges are rebased (~line 266, `for (uint32_t e : part.edges) out.edges.push_back(base + e);`), add directly after it:

```cpp
            for (uint32_t t : part.tris) out.tris.push_back(base + t);
```

- [ ] **Step 6: Run the tests** — Run: `make test`
Expected: PASS (all prior tests + `test_object_tri_emission`), `0 failures`.

- [ ] **Step 7: Commit**

```bash
git add src/sector_reader.h src/sector_reader.cpp tests/test_sector_reader.cpp
git commit -m "feat(mesh): emit triangle indices alongside wireframe edges"
```

---

### Task 2: Read a node's world orientation 3×3 (live, app convention)

This is read once per object (to un-rotate to body frame) and once per frame (live placement). It reads the 3×3 at a node offset with the **column-major/transpose** convention the engine uses for `+0xac` (= `+0x3c` for a top-level object), matching `node_world_xform`.

**Files:**
- Modify: `src/sector_reader.h` (declaration, after `read_object_wireframe`)
- Modify: `src/sector_reader.cpp` (definition; place after `read_object_wireframe`)
- Test: `tests/test_sector_reader.cpp`

- [ ] **Step 1: Add the failing test** — append before `int main()`:

```cpp
// --- node world orientation read (16.16, transposed/col-major) ------------------
static void test_read_node_orient() {
    FakeMem fm;
    uint32_t node = 0x4000;
    // Stored 3x3 at +0x3c, row stride 0x10, 16.16. Lay out a 90° rotation about Z as
    // the engine STORES it (columns): so reading transposed yields the row-major R.
    // Identity first: only diagonal set.
    fm.w(node + 0x3c + 0 * 0x10 + 0 * 4, 0x10000);   // [0][0]=1
    fm.w(node + 0x3c + 1 * 0x10 + 1 * 4, 0x10000);   // [1][1]=1
    fm.w(node + 0x3c + 2 * 0x10 + 2 * 4, 0x10000);   // [2][2]=1
    float R[9];
    bool nonident = x3::read_node_orient(fm.mem(), node, 0x3c, R);
    CHECK(!nonident);                                 // all-but-diagonal zero => identity
    CHECK(R[0] > 0.99f && R[0] < 1.01f);
    CHECK(R[4] > 0.99f && R[4] < 1.01f);
    CHECK(R[8] > 0.99f && R[8] < 1.01f);
    CHECK(R[1] > -0.01f && R[1] < 0.01f);

    // Off-diagonal: store value at stored[i=0][j=1]; transposed read => R[j=1][i=0]=R[3].
    FakeMem f2;
    f2.w(node + 0x3c + 0 * 0x10 + 1 * 4, 0x10000);   // stored [0][1] = 1.0
    float R2[9];
    bool nn = x3::read_node_orient(f2.mem(), node, 0x3c, R2);
    CHECK(nn);
    CHECK(R2[3] > 0.99f && R2[3] < 1.01f);            // R[1][0] (transpose of stored[0][1])
    CHECK(R2[1] > -0.01f && R2[1] < 0.01f);           // R[0][1] stays 0
}
```

Register in `main()`:

```cpp
    RUN(test_read_node_orient);
```

- [ ] **Step 2: Run it, verify it fails** — Run: `make test`
Expected: compile error `'read_node_orient' is not a member of 'x3'`.

- [ ] **Step 3: Declare it** — in `src/sector_reader.h`, after the `read_object_wireframe` declaration (before `} // namespace x3`):

```cpp
// Read a node's 3x3 orientation at byte offset `orient_off` (16.16 fixed, /65536),
// in the overlay's row-major convention (transposed from how the engine stores it,
// matching node_world_xform's +0xac read). out[i*3+j] = R[i][j]. Returns true if the
// matrix is non-identity (any off-diagonal or non-1 diagonal); false (and identity in
// `out`) when every field reads 0. Use orient_off=0x3c for a top-level object's live
// world orientation (== +0xac, sim-maintained even off-screen).
bool read_node_orient(const Mem& mem, uint32_t node, uint32_t orient_off, float out[9]);
```

- [ ] **Step 4: Define it** — in `src/sector_reader.cpp`, after `read_object_wireframe`'s closing brace:

```cpp
bool read_node_orient(const Mem& mem, uint32_t node, uint32_t orient_off, float out[9]) {
    auto gi = [&](uint32_t off) -> int32_t { uint32_t v = 0; ru32(mem, node + off, v); return (int32_t)v; };
    uint32_t rbits = 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            // transposed read (swap i/j in the offset), as node_world_xform does for +0xac
            int32_t r = gi(orient_off + (uint32_t)j * 0x10 + (uint32_t)i * 4);
            rbits |= (uint32_t)r;
            out[i * 3 + j] = r / 65536.0f;
        }
    if (rbits == 0) {
        for (int k = 0; k < 9; ++k) out[k] = 0.0f;
        out[0] = out[4] = out[8] = 1.0f;
        return false;
    }
    return true;
}
```

- [ ] **Step 5: Run the tests** — Run: `make test`
Expected: PASS, `0 failures`.

- [ ] **Step 6: Commit**

```bash
git add src/sector_reader.h src/sector_reader.cpp tests/test_sector_reader.cpp
git commit -m "feat(mesh): read_node_orient — node world orientation 3x3 (live)"
```

---

### Task 3: Per-vertex normals from triangles (for fresnel)

**Files:**
- Modify: `src/sector_reader.h` (declaration)
- Modify: `src/sector_reader.cpp` (definition)
- Test: `tests/test_sector_reader.cpp`

- [ ] **Step 1: Add the failing test** — append before `int main()`:

```cpp
// --- per-vertex normals from triangles -----------------------------------------
static void test_vertex_normals() {
    // One triangle in the z=0 plane (CCW): normal should be +Z (or -Z; we accept |z|=1).
    std::vector<float> verts = { 0,0,0,  1,0,0,  0,1,0 };
    std::vector<uint32_t> tris = { 0, 1, 2 };
    std::vector<float> n = x3::compute_vertex_normals(verts, tris);
    CHECK_EQ(n.size(), verts.size());
    for (int v = 0; v < 3; ++v) {
        float nx = n[v*3+0], ny = n[v*3+1], nz = n[v*3+2];
        CHECK(nx > -0.01f && nx < 0.01f);
        CHECK(ny > -0.01f && ny < 0.01f);
        CHECK(nz > 0.99f || nz < -0.99f);                 // unit, along Z
    }
}
```

Register in `main()`:

```cpp
    RUN(test_vertex_normals);
```

- [ ] **Step 2: Run it, verify it fails** — Run: `make test`
Expected: compile error `'compute_vertex_normals' is not a member of 'x3'`.

- [ ] **Step 3: Declare it** — in `src/sector_reader.h`, after `read_node_orient`:

```cpp
// Per-vertex normals (flat x,y,z triples, same count as verts) from a triangle index
// list, by accumulating face normals and normalizing. Degenerate/zero normals become
// (0,0,1). Pure (no engine reads) — for the holographic fresnel shader.
std::vector<float> compute_vertex_normals(const std::vector<float>& verts,
                                          const std::vector<uint32_t>& tris);
```

- [ ] **Step 4: Define it** — in `src/sector_reader.cpp`, after `read_node_orient`:

```cpp
std::vector<float> compute_vertex_normals(const std::vector<float>& verts,
                                          const std::vector<uint32_t>& tris) {
    std::vector<float> n(verts.size(), 0.0f);
    size_t vc = verts.size() / 3;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        uint32_t a = tris[t], b = tris[t + 1], c = tris[t + 2];
        if (a >= vc || b >= vc || c >= vc) continue;
        float ax = verts[a*3], ay = verts[a*3+1], az = verts[a*3+2];
        float bx = verts[b*3], by = verts[b*3+1], bz = verts[b*3+2];
        float cx = verts[c*3], cy = verts[c*3+1], cz = verts[c*3+2];
        float ux = bx-ax, uy = by-ay, uz = bz-az;
        float vx = cx-ax, vy = cy-ay, vz = cz-az;
        float fx = uy*vz - uz*vy, fy = uz*vx - ux*vz, fz = ux*vy - uy*vx;  // face normal (area-weighted)
        for (uint32_t idx : {a, b, c}) { n[idx*3]+=fx; n[idx*3+1]+=fy; n[idx*3+2]+=fz; }
    }
    for (size_t i = 0; i < vc; ++i) {
        float x = n[i*3], y = n[i*3+1], z = n[i*3+2];
        float L = std::sqrt(x*x + y*y + z*z);
        if (L > 1e-12f) { n[i*3]=x/L; n[i*3+1]=y/L; n[i*3+2]=z/L; }
        else { n[i*3]=0; n[i*3+1]=0; n[i*3+2]=1; }
    }
    return n;
}
```

- [ ] **Step 5: Run the tests** — Run: `make test`
Expected: PASS, `0 failures`.

- [ ] **Step 6: Commit**

```bash
git add src/sector_reader.h src/sector_reader.cpp tests/test_sector_reader.cpp
git commit -m "feat(mesh): compute_vertex_normals for solid shading"
```

---

### Task 4: Extend `meshgl` with a solid (pos+normal, multi-hull) renderer

Add a second GL program + per-hull buffers to the existing `meshgl` namespace
(src/overlay.cpp ~line 1220-1331). Keep the existing line program/path untouched (the
preview still uses it).

**Files:**
- Modify: `src/overlay.cpp` (`meshgl` namespace)

- [ ] **Step 0 (ORDERING — required): move `meshgl` above `map3d_panel`.** `map3d_panel`
  (≈line 968) precedes the `meshgl` namespace (≈line 1220), but Task 5/6 make it use
  `meshgl::Hull`/`meshgl::solid_cb`. Cut the whole `namespace meshgl { ... }` block and
  paste it **immediately before** `static void map3d_panel(void)`. (The preview panel is
  after `map3d_panel`, so both panels still see it.) Build once (`nix-shell --run 'make'`)
  to confirm the pure move compiles before adding the solid path.

- [ ] **Step 1: Load the extra GL entrypoints** — `meshgl` already loads buffer/shader procs. Add these statics near the other `PFNGL...` declarations:

```cpp
static PFNGLDELETEBUFFERSPROC           pDeleteBuffers;
static PFNGLDELETEVERTEXARRAYSPROC      pDeleteVertexArrays;
static PFNGLUNIFORMMATRIX3FVPROC        pUniformMatrix3fv;
```

In `init()`, add to the `&&` chain that sets `ok`:

```cpp
        && load(pDeleteBuffers,"glDeleteBuffers") && load(pDeleteVertexArrays,"glDeleteVertexArrays")
        && load(pUniformMatrix3fv,"glUniformMatrix3fv")
```

- [ ] **Step 2: Build the solid program in `init()`** — after the line program is linked (after `g_uMVP`/`g_uColor` are fetched, before `g_ok = true;`), add:

```cpp
    // Holographic solid program: pos+normal, fresnel rim, additive. uMV gives view-space
    // pos+normal (camera at origin in view space) for the fresnel term.
    const char* svs =
        "#version 130\n"
        "in vec3 aPos; in vec3 aNormal;\n"
        "uniform mat4 uMVP; uniform mat4 uMV;\n"
        "out vec3 vN; out vec3 vP;\n"
        "void main(){ vP=(uMV*vec4(aPos,1.0)).xyz; vN=normalize(mat3(uMV)*aNormal);\n"
        "             gl_Position=uMVP*vec4(aPos,1.0); }\n";
    const char* sfs =
        "#version 130\n"
        "in vec3 vN; in vec3 vP; uniform vec4 uColor; out vec4 frag;\n"
        "void main(){ vec3 N=normalize(vN); vec3 V=normalize(-vP);\n"
        "  float f=pow(1.0-clamp(abs(dot(N,V)),0.0,1.0),2.5);\n"   // abs: faces toward AND away rim-glow
        "  float i=0.18+0.9*f;\n"                                   // base fill + rim gain
        "  frag=vec4(uColor.rgb*i, uColor.a*i); }\n";
    GLuint sv = pCreateShader(GL_VERTEX_SHADER);   pShaderSource(sv,1,&svs,nullptr); pCompileShader(sv);
    GLuint sf = pCreateShader(GL_FRAGMENT_SHADER); pShaderSource(sf,1,&sfs,nullptr); pCompileShader(sf);
    g_sprog = pCreateProgram(); pAttachShader(g_sprog,sv); pAttachShader(g_sprog,sf);
    pBindAttribLocation(g_sprog,0,"aPos"); pBindAttribLocation(g_sprog,1,"aNormal");
    pLinkProgram(g_sprog); pDeleteShader(sv); pDeleteShader(sf);
    g_suMVP = pGetUniformLocation(g_sprog,"uMVP");
    g_suMV  = pGetUniformLocation(g_sprog,"uMV");
    g_suColor = pGetUniformLocation(g_sprog,"uColor");
```

Add the matching statics near `g_prog`:

```cpp
static GLuint g_sprog = 0;
static GLint  g_suMVP = -1, g_suMV = -1, g_suColor = -1;
```

- [ ] **Step 3: Add a `Hull` type + create/destroy/draw** — add inside `meshgl`, after `upload()`:

```cpp
// One cached object's GPU geometry (body-local verts+normals, triangle indices).
struct Hull { GLuint vao=0, vbo=0, ebo=0; GLsizei icount=0; };

// interleaved [px,py,pz, nx,ny,nz] * vcount; tris = flat index triples.
static Hull hull_create(const std::vector<float>& verts, const std::vector<float>& normals,
                        const std::vector<uint32_t>& tris) {
    Hull h;
    if (!g_ok || verts.empty() || tris.empty()) return h;
    size_t vc = verts.size() / 3;
    std::vector<float> inter(vc * 6);
    for (size_t i = 0; i < vc; ++i) {
        inter[i*6+0]=verts[i*3+0]; inter[i*6+1]=verts[i*3+1]; inter[i*6+2]=verts[i*3+2];
        inter[i*6+3]=normals[i*3+0]; inter[i*6+4]=normals[i*3+1]; inter[i*6+5]=normals[i*3+2];
    }
    pGenVertexArrays(1,&h.vao); pGenBuffers(1,&h.vbo); pGenBuffers(1,&h.ebo);
    pBindVertexArray(h.vao);
    pBindBuffer(GL_ARRAY_BUFFER,h.vbo);
    pBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(inter.size()*sizeof(float)),inter.data(),GL_STATIC_DRAW);
    pEnableVertexAttribArray(0);
    pVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(const void*)0);
    pEnableVertexAttribArray(1);
    pVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(const void*)(3*sizeof(float)));
    pBindBuffer(GL_ELEMENT_ARRAY_BUFFER,h.ebo);
    pBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)(tris.size()*sizeof(uint32_t)),tris.data(),GL_STATIC_DRAW);
    pBindVertexArray(0);
    h.icount = (GLsizei)tris.size();
    return h;
}
static void hull_destroy(Hull& h) {
    if (h.vbo) pDeleteBuffers(1,&h.vbo);
    if (h.ebo) pDeleteBuffers(1,&h.ebo);
    if (h.vao) pDeleteVertexArrays(1,&h.vao);
    h = Hull{};
}
```

- [ ] **Step 4: Add a batched solid draw callback** — a per-frame list of (hull, MVP, MV, color), drawn in one callback with additive blend + depth off. Add after `draw_cb`:

```cpp
struct SolidItem { const Hull* h; float mvp[16]; float mv[16]; float col[4]; };
static std::vector<SolidItem> g_solids;          // filled each frame (far->near), drawn in cb
static int g_svx, g_svy, g_svw, g_svh;           // viewport (framebuffer px), set per panel
static void solid_cb(const ImDrawList*, const ImDrawCmd*) {
    if (!g_ok || g_solids.empty()) return;
    GLint old_vp[4]; glGetIntegerv(GL_VIEWPORT, old_vp);
    glViewport(g_svx,g_svy,g_svw,g_svh);
    glEnable(GL_SCISSOR_TEST); glScissor(g_svx,g_svy,g_svw,g_svh);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);   // additive
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_CULL_FACE);
    pUseProgram(g_sprog);
    for (const auto& s : g_solids) {
        if (!s.h || !s.h->icount) continue;
        pUniformMatrix4fv(g_suMVP,1,GL_FALSE,s.mvp);
        pUniformMatrix4fv(g_suMV,1,GL_FALSE,s.mv);
        pUniform4f(g_suColor,s.col[0],s.col[1],s.col[2],s.col[3]);
        pBindVertexArray(s.h->vao);
        glDrawElements(GL_TRIANGLES,s.h->icount,GL_UNSIGNED_INT,(const void*)0);
    }
    pBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    glViewport(old_vp[0],old_vp[1],old_vp[2],old_vp[3]);
}
```

- [ ] **Step 5: Build to verify it compiles** — Run: `nix-shell --run 'make'`
Expected: `=== built libx3ui.so ===`, no errors. (No behavior change yet — nothing calls the solid path.)

- [ ] **Step 6: Commit**

```bash
git add src/overlay.cpp
git commit -m "feat(meshgl): solid pos+normal program + per-hull buffers + batched draw"
```

---

### Task 5: Hull cache + budgeted read in the overlay

A cache keyed by object address: read geometry once (budgeted), un-rotate to body frame,
compute normals, upload. Add near the top of the 3D-map section in `src/overlay.cpp`
(after the `Camera3D`/`project3d` helpers, before `map3d_panel`).

**Files:**
- Modify: `src/overlay.cpp`

- [ ] **Step 1: Add the cache + accessor** — insert this block before `map3d_panel`:

```cpp
// ---- solid hull cache (read once per object, placed live each frame) -----------
struct HullCache {
    meshgl::Hull gpu;
    float        extent_km = 1.0f;   // body bbox half-extent in km (for dot<->hull + box)
    uint32_t     model_id  = 0;      // root model id at read time (re-read if it changes)
    int          lod       = -1;     // map LOD at read time
    bool         ok        = false;
};
static std::unordered_map<uint32_t, HullCache> g_hulls;   // key = object address
static int g_map_lod = 3;                                 // hull detail in the map

// Ensure object `addr` (root model `mid`) has a cached hull at the current map LOD.
// Reads at most when missing/stale; returns nullptr if it has no resolvable mesh.
// `budget` is decremented on an actual (expensive) read; pass a per-frame counter.
static HullCache* hull_ensure(uint32_t addr, uint32_t mid, int& budget) {
    auto it = g_hulls.find(addr);
    if (it != g_hulls.end() && it->second.lod == g_map_lod && it->second.model_id == mid)
        return it->second.ok ? &it->second : nullptr;
    if (budget <= 0) return (it != g_hulls.end() && it->second.ok) ? &it->second : nullptr;
    --budget;
    HullCache hc;
    hc.model_id = mid; hc.lod = g_map_lod;
    x3::Mem mem = make_engine_mem();
    x3::MeshWire w = x3::read_object_wireframe(mem, addr, 512, 200000, 600000, g_map_lod, 1000);
    if (w.ok && !w.tris.empty()) {
        // Un-rotate world-oriented geometry into body frame by R_read^T (root+0x3c).
        uint32_t root = 0; float R[9];
        x3::read_object_root(mem, addr, root);
        x3::read_node_orient(mem, root, 0x3c, R);
        float lo[3]={1e30f,1e30f,1e30f}, hi[3]={-1e30f,-1e30f,-1e30f};
        for (size_t i = 0; i + 2 < w.verts.size(); i += 3) {
            float x=w.verts[i], y=w.verts[i+1], z=w.verts[i+2];
            // body = R^T * world : out_i = sum_j R[j*3+i]*v_j
            float bx=R[0]*x+R[3]*y+R[6]*z, by=R[1]*x+R[4]*y+R[7]*z, bz=R[2]*x+R[5]*y+R[8]*z;
            w.verts[i]=bx; w.verts[i+1]=by; w.verts[i+2]=bz;
            for (int k=0;k<3;++k){ float v=(k==0?bx:k==1?by:bz); lo[k]=std::min(lo[k],v); hi[k]=std::max(hi[k],v);} }
        std::vector<float> nrm = x3::compute_vertex_normals(w.verts, w.tris);
        if (!meshgl::g_tried) meshgl::init();
        if (meshgl::g_ok) hc.gpu = meshgl::hull_create(w.verts, nrm, w.tris);
        float ext = std::max(std::max(hi[0]-lo[0], hi[1]-lo[1]), hi[2]-lo[2]) * 0.5f;
        hc.extent_km = (ext > 1e-3f ? ext : 1.0f) / x3::off::UNITS_PER_KM;
        hc.ok = (hc.gpu.icount > 0);
    }
    if (it != g_hulls.end()) meshgl::hull_destroy(it->second.gpu);
    g_hulls[addr] = hc;
    return hc.ok ? &g_hulls[addr] : nullptr;
}

// Drop hulls for objects no longer in `live` (despawned / out of range).
static void hull_gc(const std::vector<x3::ObjInfo>& live) {
    std::unordered_set<uint32_t> keep;
    for (const auto& o : live) keep.insert(o.addr);
    for (auto it = g_hulls.begin(); it != g_hulls.end(); ) {
        if (!keep.count(it->first)) { meshgl::hull_destroy(it->second.gpu); it = g_hulls.erase(it); }
        else ++it;
    }
}
```

- [ ] **Step 2: Add the tiny `read_object_root` helper** — declare in `src/sector_reader.h` (after `read_node_orient`) and define in `src/sector_reader.cpp`. Declaration:

```cpp
// Root transform node of an object (= *(obj+0x6c)). false if unreadable/null.
bool read_object_root(const Mem& mem, uint32_t obj_addr, uint32_t& out_root);
```

Definition (in `.cpp`):

```cpp
bool read_object_root(const Mem& mem, uint32_t obj_addr, uint32_t& out_root) {
    uint32_t r = 0;
    if (!ru32(mem, obj_addr + 0x6c, r) || !r) return false;
    out_root = r; return true;
}
```

- [ ] **Step 3: Add includes if missing** — ensure `src/overlay.cpp` has `#include <unordered_map>` and `#include <unordered_set>` near the other includes (check the top of the file; add whichever is absent).

- [ ] **Step 4: Build** — Run: `nix-shell --run 'make'`
Expected: builds clean. (Still unused — wired up in Task 6.)

- [ ] **Step 5: Commit**

```bash
git add src/overlay.cpp src/sector_reader.h src/sector_reader.cpp
git commit -m "feat(map): solid hull cache (read once, body-frame, normals, budgeted)"
```

---

### Task 6: Wire hulls into `map3d_panel` (dot↔hull, live placement, selection box)

**Files:**
- Modify: `src/overlay.cpp` (`map3d_panel`, the object loop ~line 1109-1140)

- [ ] **Step 1: Per-frame setup** — at the top of `map3d_panel`, after `view = read_current_sector(...)` and the `g_sel_alive` block, add a map-LOD control and GC:

```cpp
    ImGui::SameLine(); ImGui::SetNextItemWidth(70);
    ImGui::SliderInt("hull LOD", &g_map_lod, 0, 6);
    hull_gc(view.objects);
    int hull_budget = 2;                 // expensive geometry reads per frame
    meshgl::g_solids.clear();
```

- [ ] **Step 2: Helper to build a per-object MVP/MV matching `project3d`** — add this lambda after the `PROJ` lambda is defined in `map3d_panel` (it reuses `cam`, `ctr`, `focal`, `view`):

```cpp
    const float U2 = x3::off::UNITS_PER_KM;
    // View matrix matching project3d's basis (eye relative to look-at center).
    auto build_view = [&](float Vm[16]) {
        float cp=cosf(cam.pitch), sp=sinf(cam.pitch), cyw=cosf(cam.yaw), syw=sinf(cam.yaw);
        float ex=cam.dist*cp*syw, ey=cam.dist*sp, ez=cam.dist*cp*cyw;     // eye rel target
        float fx=-ex,fy=-ey,fz=-ez,fl=sqrtf(fx*fx+fy*fy+fz*fz); if(fl<1e-6f)fl=1; fx/=fl;fy/=fl;fz/=fl;
        float rx=-fz,ry=0,rz=fx,rl=sqrtf(rx*rx+rz*rz); if(rl<1e-6f){rx=1;rz=0;rl=1;} rx/=rl;rz/=rl;
        float ux=ry*fz-rz*fy, uy=rz*fx-rx*fz, uz=rx*fy-ry*fx;
        // column-major GL view: rows r,u,f ; translation -(R*eye)
        Vm[0]=rx; Vm[4]=ry; Vm[8]=rz;  Vm[12]=-(rx*ex+ry*ey+rz*ez);
        Vm[1]=ux; Vm[5]=uy; Vm[9]=uz;  Vm[13]=-(ux*ex+uy*ey+uz*ez);
        Vm[2]=-fx;Vm[6]=-fy;Vm[10]=-fz;Vm[14]=(fx*ex+fy*ey+fz*ez);
        Vm[3]=0; Vm[7]=0; Vm[11]=0; Vm[15]=1;
    };
    // Per-object model matrix: body -> flipped-km relative to the look-at center, using
    // the LIVE root orient (Rl, app row-major) + live root pos. Flip = diag(1,-1,-1).
    auto build_model = [&](const float Rl[9], int32_t ox, int32_t oy, int32_t oz, float Mm[16]) {
        float tx = ( (float)(ox - view.player_x))/U2 - cam.cx;
        float ty = (-(float)(oy - view.player_y))/U2 - cam.cy;
        float tz = (-(float)(oz - view.player_z))/U2 - cam.cz;
        // M3 = Flip * (Rl/U2) ; row-major Rl -> GL column-major (transpose into columns)
        float s = 1.0f/U2; const float F[3]={1.0f,-1.0f,-1.0f};
        for (int c=0;c<3;++c){ for (int r=0;r<3;++r) Mm[c*4+r]=F[r]*Rl[r*3+c]*s; Mm[c*4+3]=0; }
        Mm[12]=tx; Mm[13]=ty; Mm[14]=tz; Mm[15]=1;
    };
```

- [ ] **Step 3: Decide dot vs hull in the existing object loop** — extend the `PObj` struct and the projection loop (~line 1110-1119) to carry the hull + apparent radius:

```cpp
    struct PObj { float depth; ImVec2 s, ground; bool gok; const x3::ObjInfo* o; HullCache* hull; float rpx; };
    std::vector<PObj> ps; ps.reserve(view.objects.size());
    for (const auto& o : view.objects) {
        if (!obj_passes_filter(o, map3d_rel, map3d_type)) continue;
        ImVec2 s; float d;
        if (!PROJ(KX(o.x), KY(o.y), KZ(o.z), s, d)) continue;
        PObj e; e.depth = d; e.s = s; e.o = &o; e.hull = nullptr; e.rpx = 0; float gd;
        e.gok = PROJ(KX(o.x), 0, KZ(o.z), e.ground, gd);
        // promote to a hull when big enough on screen and it has a resolvable mesh
        uint32_t root=0, mid=0;
        if (x3::read_object_root(make_engine_mem(), o.addr, root)) {
            make_engine_mem().u32(root + 0x12c, mid);
            HullCache* hc = hull_ensure(o.addr, mid, hull_budget);
            if (hc) { e.rpx = hc->extent_km / d * focal; if (e.rpx > 6.0f) e.hull = hc; }
        }
        ps.push_back(e);
    }
    std::sort(ps.begin(), ps.end(), [](const PObj& a, const PObj& b){ return a.depth > b.depth; });
```

- [ ] **Step 4: Queue hulls (far→near) and suppress the dot when a hull draws** — in the draw loop over `ps` (~line 1121+), at the very start of the loop body add the hull queue, and gate the dot/glow on `!e.hull`. Replace the dot-drawing lines:

```cpp
    // viewport for the GPU hull pass (framebuffer pixels)
    {
        ImGuiIO& io = ImGui::GetIO();
        float fbx=io.DisplayFramebufferScale.x, fby=io.DisplayFramebufferScale.y;
        int fbh=(int)(io.DisplaySize.y*fby);
        meshgl::g_svx=(int)(p0.x*fbx); meshgl::g_svw=(int)(avail.x*fbx);
        meshgl::g_svh=(int)(avail.y*fby); meshgl::g_svy=fbh-(int)(p1.y*fby);
    }
    float Vm[16], Pm[16];
    build_view(Vm);
    meshgl::m_perspective(Pm, 0.9f, avail.x/avail.y, 0.02f, 4000.0f);
    for (const auto& e : ps) {
        const auto& o = *e.o; ImU32 col = faction_color(o);
        if (e.gok) {
            dl->AddLine(e.ground, e.s, (col & 0x00FFFFFFu) | 0x55000000u, 1.0f);
            dl->AddCircleFilled(e.ground, 1.6f, (col & 0x00FFFFFFu) | 0x66000000u);
        }
        if (e.hull && meshgl::g_ok) {
            uint32_t root=0; float Rl[9];
            x3::read_object_root(make_engine_mem(), o.addr, root);
            x3::read_node_orient(make_engine_mem(), root, 0x3c, Rl);   // LIVE orient
            meshgl::SolidItem si; si.h = &e.hull->gpu;
            float Mm[16], MV[16];
            build_model(Rl, o.x, o.y, o.z, Mm);
            meshgl::m_mul(MV, Vm, Mm);
            meshgl::m_mul(si.mvp, Pm, MV);
            for (int k=0;k<16;++k) si.mv[k]=MV[k];
            si.col[0]=((col)&0xff)/255.0f; si.col[1]=((col>>8)&0xff)/255.0f;
            si.col[2]=((col>>16)&0xff)/255.0f; si.col[3]=0.85f;
            meshgl::g_solids.push_back(si);
        } else {
            float rad = 70.0f / e.depth; rad = rad < 1.5f ? 1.5f : (rad > 9.0f ? 9.0f : rad);
            dl->AddCircleFilled(e.s, rad*2.4f, (col & 0x00FFFFFFu) | 0x22000000u);
            dl->AddCircleFilled(e.s, rad, col);
        }
        if (o.id == g_sel_id) {                              // four-corner selection box
            float r = (e.hull ? std::max(e.rpx, 10.0f) : 9.0f) + 4.0f;
            ImU32 bc = IM_COL32(255,255,255,235); float L = r*0.5f;
            ImVec2 c = e.s;
            dl->AddLine(ImVec2(c.x-r,c.y-r), ImVec2(c.x-r+L,c.y-r), bc, 1.6f);
            dl->AddLine(ImVec2(c.x-r,c.y-r), ImVec2(c.x-r,c.y-r+L), bc, 1.6f);
            dl->AddLine(ImVec2(c.x+r,c.y-r), ImVec2(c.x+r-L,c.y-r), bc, 1.6f);
            dl->AddLine(ImVec2(c.x+r,c.y-r), ImVec2(c.x+r,c.y-r+L), bc, 1.6f);
            dl->AddLine(ImVec2(c.x-r,c.y+r), ImVec2(c.x-r+L,c.y+r), bc, 1.6f);
            dl->AddLine(ImVec2(c.x-r,c.y+r), ImVec2(c.x-r,c.y+r-L), bc, 1.6f);
            dl->AddLine(ImVec2(c.x+r,c.y+r), ImVec2(c.x+r-L,c.y+r), bc, 1.6f);
            dl->AddLine(ImVec2(c.x+r,c.y+r), ImVec2(c.x+r,c.y+r-L), bc, 1.6f);
        }
        // (existing label-drawing code stays below)
```

Note: remove the OLD selection-circle block (the `dl->AddCircle(e.s, r, ...)` + the two
tick `AddLine`s at ~line 1130-1135) — it's replaced by the four-corner box above.

- [ ] **Step 5: Emit the GPU hull pass** — after the `for (const auto& e : ps)` draw loop closes (after labels, before `dl->PopClipRect()` / `ImGui::End()`), add:

```cpp
    if (meshgl::g_ok && !meshgl::g_solids.empty()) {
        dl->AddCallback(meshgl::solid_cb, nullptr);
        dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }
```

- [ ] **Step 6: Build** — Run: `nix-shell --run 'make'`
Expected: builds clean.

- [ ] **Step 7: Commit**

```bash
git add src/overlay.cpp
git commit -m "feat(map): solid holographic hulls in the 3D map (dot<->hull, live placement, corner-box select)"
```

---

### Task 7: Live visual verification (the real oracle)

**Files:** none (verification only).

- [ ] **Step 1: Host tests still green** — Run: `make test`
Expected: `... 0 failures` (both suites).

- [ ] **Step 2: Build** — Run: `nix-shell --run 'make'`
Expected: `=== built libx3ui.so ===`.

- [ ] **Step 3: Launch & inspect** — kill any running game, then `./x3launch.sh`. Press ScrollLock for the overlay, open **x3ui - 3D map**. Verify:
  - Nearby ships/stations show translucent faction-colored hulls sitting **on** their blips, oriented like the real ships (turn a ship — its hull turns live).
  - **Off-screen** objects (behind you / across the sector) still show correctly oriented hulls (confirms the sim-maintained transform) — may carry extra LOD/internal geometry (known, deferred).
  - Zoom out → hulls shrink and snap to dots below the threshold; zoom in → dots become hulls.
  - Selected object shows the four-corner box (not a circle), on both dots and hulls.
  - Frame rate stays smooth panning a busy sector (budget = 2 reads/frame; raise/lower `hull_budget` if hitching/slow to populate).

- [ ] **Step 4: If something's off, RE/probe — don't guess.** Per the methodology: use `owire <addr> [lod]` + a socket probe to compare placement/orientation against the engine; re-decompile if a transform looks wrong. Update `docs/x3-render-pipeline.md` with any new (D)/(L) finding.

- [ ] **Step 5: Update memory** — once it visually checks out, update `[[x3ui-mesh-render-parked]]` / project status to record that solid holographic hulls in the map are done.

---

## Notes for the executor
- `make` MUST run in the nix-shell (`nix-shell --run 'make'`); `make test` runs anywhere.
- The map's existing Y/Z screen flip (`KX`/`KY`/`KZ`, with KY/KZ negated) is mirrored in `build_model`'s `F = diag(1,-1,-1)`. If hulls appear mirrored or rotated vs blips, that flip (and the orient transpose in `read_node_orient`) is the first suspect.
- Reads go through `make_engine_mem()` (pread on the live process); a stale pointer yields `ok=false`, never a crash. `read_object_wireframe` at `min_model_id=1000` already skips proxies.
- Do NOT call `model_load_resolve` live.
