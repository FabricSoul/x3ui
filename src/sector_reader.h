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
    // Resolve a model id (obj+0x12c) to the resident model struct pointer. 0 if
    // unavailable. Optional (used only by read_mesh_wireframe).
    std::function<uint32_t(uint32_t model_id)> model;
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
    bool ok = false;
    size_t vcount() const { return verts.size() / 3; }
};

// Read an object's wireframe by walking model id (obj+0x12c) -> model cache ->
// LOD mesh -> int16 vertex array (/16384) + triangle list -> triangle edges.
// `coarsest` picks the lowest-detail LOD (cheaper wireframe). Bounded + all reads
// go through `mem` (pread), so a stale pointer yields {ok=false}, never a crash.
MeshWire read_mesh_wireframe(const Mem& mem, uint32_t obj_addr, bool coarsest = true,
                             size_t vcap = 60000, size_t ecap = 200000);

} // namespace x3
