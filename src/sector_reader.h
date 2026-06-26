#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace x3 {

struct ObjInfo {
    uint32_t    id      = 0;
    std::string name;
    int16_t     type    = 0;
    int16_t     subtype = 0;
    int32_t     x = 0, y = 0, z = 0; // raw position words (interpret int or float)
};

// Abstraction over the game's 32-bit address space, so the walk is testable.
struct Mem {
    // Read a 32-bit word at `addr`. Return false if unreadable.
    std::function<bool(uint32_t addr, uint32_t& out)> u32;
    // Read a NUL-terminated string at `addr` (<= maxlen bytes). "" if 0/unreadable.
    std::function<std::string(uint32_t addr, size_t maxlen)> cstr;
};

struct SectorView {
    bool                 in_sector   = false;
    uint32_t             sector_addr = 0;
    std::string          sector_name;
    std::vector<ObjInfo> objects;
};

// Default scene-global address (duplicated from struct_offsets.h so callers of
// this header needn't include it). Must precede the function that defaults to it.
constexpr uint32_t off_SCENE_GLOBAL = 0x0837a0d0u;

// Walk the contained-object list of a type-7 sector object. cap bounds iterations.
std::vector<ObjInfo> read_sector_objects(const Mem& mem, uint32_t sector_addr,
                                         size_t cap = 4096);

// Resolve the player's current sector from the scene-mgr global and read it.
// `scene_global` is the ADDRESS that holds the scene-mgr pointer.
SectorView read_current_sector(const Mem& mem,
                               uint32_t scene_global = off_SCENE_GLOBAL,
                               size_t cap = 4096);

// Human label for a type id (+0x48). Returns a static string.
const char* type_label(int16_t type);

} // namespace x3
