#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "struct_offsets.h"

namespace x3 {

// IFF relation to the player, matching the game's blip coloring. REL_NONE = an
// object with no faction (gate/sun/planet/asteroid/ware/...).
enum Relation { REL_NONE = 0, REL_NEUTRAL, REL_FRIEND, REL_ENEMY, REL_OWN };

struct ObjInfo {
    uint32_t    id      = 0;
    uint32_t    addr    = 0;         // live engine node address (for actions: set-target, etc.)
    std::string name;
    int16_t     type    = 0;
    int16_t     subtype = 0;
    int32_t     x = 0, y = 0, z = 0; // raw position words (interpret int or float)
    int32_t     dist    = -1;        // distance from the player (raw units); -1 if unknown
    int16_t     race    = -1;        // owner/race id (obj+0x3c); -1 if unknown
    int8_t      relation = REL_NONE; // IFF relation to the player (see Relation)
};

// Abstraction over the game's 32-bit address space, so the walk is testable.
struct Mem {
    // Read a 32-bit word at `addr`. Return false if unreadable.
    std::function<bool(uint32_t addr, uint32_t& out)> u32;
    // Read a NUL-terminated string at `addr` (<= maxlen bytes). "" if 0/unreadable.
    std::function<std::string(uint32_t addr, size_t maxlen)> cstr;
    // Resolve a {textPage, textID} reference to its string (the engine does the
    // lookup + marker expansion). "" if unset/unavailable. Optional.
    std::function<std::string(uint32_t page, uint32_t id)> text;
    // Bulk-read `n` raw bytes at `addr` into `dst` (one shot). Optional fast path for
    // big arrays (verts/tris); when unset, callers fall back to per-word u32 reads.
    // Returns false if any part is unreadable.
    std::function<bool(uint32_t addr, void* dst, size_t n)> bytes;
};

struct SectorView {
    bool                 in_sector   = false;
    uint32_t             sector_addr = 0;
    std::string          sector_name;
    std::vector<ObjInfo> objects;
    int64_t              scanner_range = 0;   // gravidar scanner range (raw units; 0 = unknown)
    bool                 player_valid  = false;
    int32_t              player_x = 0, player_y = 0, player_z = 0;  // player pos (raw units)
    bool                 player_orient_valid = false;
    float                player_fwd_x = 0, player_fwd_y = 0, player_fwd_z = 1; // ship forward (unit, +Z nose)
};

// Player position + scanner range, for the gravidar visibility test (ships beyond
// the scanner range are hidden, matching the in-game sector map). `valid` false =
// no range filtering applied.
struct RangeCtx {
    bool     valid = false;
    int32_t  px = 0, py = 0, pz = 0;  // player position (raw units, 500000/km)
    int64_t  range = 0;               // scanner range (raw units)
    int16_t  player_race  = -1;       // player ship race id (for the IFF relation)
    uint32_t player_ship  = 0;        // the player's own ship address (== REL_OWN)
};

// Convenience alias to the scene-global address (aliases the single source of
// truth in struct_offsets.h — no duplicate literal). Must precede the function
// that defaults to it.
constexpr uint32_t off_SCENE_GLOBAL = off::SCENE_GLOBAL;

// Walk the contained-object list of a type-7 sector object. cap bounds iterations.
// If `rc` is non-null and valid, ships beyond the scanner range are dropped.
std::vector<ObjInfo> read_sector_objects(const Mem& mem, uint32_t sector_addr,
                                         size_t cap = 4096, const RangeCtx* rc = nullptr);

// Resolve the player's current sector from the scene-mgr global and read it.
// `scene_global` is the ADDRESS that holds the scene-mgr pointer.
SectorView read_current_sector(const Mem& mem,
                               uint32_t scene_global = off_SCENE_GLOBAL,
                               size_t cap = 4096);

// Human label for a type id (+0x48). Returns a static string.
const char* type_label(int16_t type);

// Resolve a {textPage, textID} reference to its string via the engine text DB.
// Returns "" if unavailable. Used to fix object names that show as "{17, 6651}".
std::string resolve_text(const Mem& mem, uint32_t page, uint32_t id);

// A wireframe mesh read from the engine's resident CPU model data (no GPU, no
// engine calls). Vertices are model-local units; edges index into them.
struct MeshWire {
    std::vector<float>    verts;   // flat x,y,z triples (model-local units)
    std::vector<uint32_t> edges;   // flat index pairs into verts (i0,i1,i0,i1,...)
    std::vector<uint32_t> tris;    // flat index triples into verts (v0,v1,v2,...) for solid fill
    bool ok = false;
    size_t vcount() const { return verts.size() / 3; }
};

// Read an object's wireframe by walking model id (obj+0x12c) -> model cache ->
// LOD mesh -> int16 vertex array (/16384) + triangle list -> triangle edges.
// `coarsest` picks the lowest-detail LOD (cheaper wireframe). Bounded + all reads
// go through `mem` (pread), so a stale pointer yields {ok=false}, never a crash.
MeshWire read_mesh_wireframe(const Mem& mem, uint32_t obj_addr, bool coarsest = true,
                             size_t vcap = 60000, size_t ecap = 200000);

// Assemble a whole object's wireframe by walking its scene graph from the
// transform node (obj+0x6c): DFS children (node+0x0c) and siblings (node+0x00),
// reading each mesh-bearing node at finest LOD and (Task 3) composing transforms.
// Pure reads + bounded (node_cap / vcap / ecap). Proxy and unloaded nodes are
// skipped. Returns {ok=false} if no part resolved.
// Optional instrumentation for read_object_wireframe (diagnostics only).
struct ObjWireStats {
    size_t nodes = 0;          // scene-graph nodes visited
    size_t resolved = 0;       // nodes whose mesh resolved (read_node_mesh ok)
    size_t kept = 0;           // parts appended to the output
    size_t dropped_guard = 0;  // parts dropped by the finiteness/magnitude guard
    size_t sum_resolved_v = 0; // total verts across all resolved parts (pre-guard)
    float  max_coord = 0.0f;   // largest |coord| any transform produced
};
// lod_level: 0 = finest detail (slow), higher = coarser (clamped per model, faster).
// min_model_id: skip drawing nodes whose model id is below this (1000 in-game drops the
// built-in primitive proxies; 0 = draw all, unit tests). Independent of that, the engine
// per-part gate is always applied (a part with no render-part, part+0x50==0, is
// collision/invisible and skipped — matching render_mesh_part). Children are always
// traversed. NOTE: node+0x118&2 is a per-frame visibility flag (off-screen objects clear
// it), so it is NOT used as a draw gate here.
MeshWire read_object_wireframe(const Mem& mem, uint32_t obj_addr,
                               size_t node_cap = 512, size_t vcap = 200000,
                               size_t ecap = 600000, int lod_level = 0,
                               uint32_t min_model_id = 0, ObjWireStats* stats = nullptr);

// Root transform node of an object (= *(obj+0x6c)). false if unreadable/null.
bool read_object_root(const Mem& mem, uint32_t obj_addr, uint32_t& out_root);

// Read a node's 3x3 orientation at byte offset `orient_off` (16.16 fixed, /65536),
// in the overlay's row-major convention (transposed from how the engine stores it,
// matching node_world_xform's +0xac read). out[i*3+j] = R[i][j]. Returns true if the
// matrix is non-identity (any off-diagonal or non-1 diagonal); false (and identity in
// `out`) when every field reads 0. Use orient_off=0x3c for a top-level object's live
// world orientation (== +0xac, sim-maintained even off-screen).
bool read_node_orient(const Mem& mem, uint32_t node, uint32_t orient_off, float out[9]);

// Per-vertex normals (flat x,y,z triples, same count as verts) from a triangle index
// list, by accumulating face normals and normalizing. Degenerate/zero normals become
// (0,0,1). Pure (no engine reads) — for the holographic fresnel shader.
std::vector<float> compute_vertex_normals(const std::vector<float>& verts,
                                          const std::vector<uint32_t>& tris);

} // namespace x3
