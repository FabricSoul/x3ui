#include "sector_reader.h"
#include "struct_offsets.h"

namespace x3 {

static bool ru32(const Mem& m, uint32_t addr, uint32_t& out) {
    return m.u32 && m.u32(addr, out);
}

// Returns the object's type id, or -1 (no valid type) if the read fails.
// -1 never matches any TYPE_* constant, so callers can treat it as "unknown".
static int16_t read_type(const Mem& m, uint32_t obj) {
    uint32_t w = 0;
    if (!ru32(m, obj + off::OBJ_TYPE, w)) return -1;
    return (int16_t)(w & 0xFFFF);
}

std::vector<ObjInfo> read_sector_objects(const Mem& mem, uint32_t sector_addr, size_t cap) {
    std::vector<ObjInfo> out;
    if (!sector_addr) return out;

    uint32_t payload = 0;
    if (!ru32(mem, sector_addr + off::OBJ_PAYLOAD, payload) || !payload) return out;

    const uint32_t sentinel = payload + off::SEC_LIST_SENTINEL;
    uint32_t node = 0;
    if (!ru32(mem, payload + off::SEC_LIST_HEAD, node)) return out;

    size_t n = 0;
    while (node && node != sentinel && n < cap) {
        ObjInfo o;
        uint32_t w = 0;
        if (ru32(mem, node + off::OBJ_ID, w)) o.id = w;
        if (ru32(mem, node + off::OBJ_TYPE, w)) {
            o.type    = (int16_t)(w & 0xFFFF);
            o.subtype = (int16_t)(w >> 16);
        }
        uint32_t nameptr = 0;
        if (ru32(mem, node + off::OBJ_NAME, nameptr) && nameptr && mem.cstr)
            o.name = mem.cstr(nameptr, 64);
        uint32_t tf = 0;
        if (ru32(mem, node + off::OBJ_TRANSFORM, tf) && tf) {
            if (ru32(mem, tf + off::TF_X, w)) o.x = (int32_t)w;
            if (ru32(mem, tf + off::TF_Y, w)) o.y = (int32_t)w;
            if (ru32(mem, tf + off::TF_Z, w)) o.z = (int32_t)w;
        }
        // Keep the object even if its next-link is unreadable; the break below
        // then stops the walk (partial data beats dropping a visible object).
        out.push_back(std::move(o));
        ++n;

        uint32_t next = 0;
        if (!ru32(mem, node + off::OBJ_NEXT, next)) break;
        node = next;
    }
    return out;
}

SectorView read_current_sector(const Mem& mem, uint32_t scene_global, size_t cap) {
    SectorView v;
    uint32_t scene = 0;
    if (!ru32(mem, scene_global, scene) || !scene) return v;

    uint32_t anchor = 0;
    if (!ru32(mem, scene + off::SCENE_PLAYER_ANCHOR, anchor) || !anchor) return v;

    int16_t t = read_type(mem, anchor);
    uint32_t sector = 0;
    if (t == off::TYPE_SECTOR) {
        sector = anchor;                              // anchor IS the sector
    } else if (t == off::TYPE_SHIP || t == off::TYPE_STATION) {
        uint32_t parent = 0;                          // anchor is player obj; sector is its container
        if (ru32(mem, anchor + off::OBJ_PARENT, parent) && parent &&
            read_type(mem, parent) == off::TYPE_SECTOR)
            sector = parent;
    }
    if (!sector) return v;

    v.in_sector   = true;
    v.sector_addr = sector;
    uint32_t nameptr = 0;
    if (ru32(mem, sector + off::OBJ_NAME, nameptr) && nameptr && mem.cstr)
        v.sector_name = mem.cstr(nameptr, 64);
    v.objects = read_sector_objects(mem, sector, cap);
    return v;
}

const char* type_label(int16_t type) {
    switch (type) {
        case off::TYPE_SHIP:    return "Ship";
        case off::TYPE_STATION: return "Station";
        case off::TYPE_SECTOR:  return "Sector";
        default:                return "?";
    }
}

} // namespace x3
