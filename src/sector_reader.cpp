#include "sector_reader.h"
#include "struct_offsets.h"

#include <algorithm>
#include <cmath>

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

// For a jump gate, resolve the name of the sector it leads to: each non-null slot
// in the gate payload's destination array points at the paired object in the dest
// sector; its parent chain (+0x54, stepping past a paired gate/station) reaches the
// type-1 sector whose name is at +0x0c. Returns "" if not resolvable.
static std::string gate_dest_sector_name(const Mem& mem, uint32_t gate) {
    uint32_t gdata = 0;
    if (!ru32(mem, gate + off::OBJ_PAYLOAD, gdata) || !gdata) return "";
    for (int i = 0; i < 7; ++i) {
        uint32_t dest = 0;
        if (!ru32(mem, gdata + off::GATE_DEST_ARRAY + i * 4, dest) || !dest) continue;
        uint32_t parent = 0;
        if (!ru32(mem, dest + off::OBJ_PARENT, parent) || !parent) continue;
        int16_t pt = read_type(mem, parent);
        if (pt == off::TYPE_STATION || pt == off::TYPE_FACTORY) {  // paired -> step up once
            uint32_t up = 0;
            if (ru32(mem, parent + off::OBJ_PARENT, up) && up) { parent = up; pt = read_type(mem, parent); }
        }
        if (pt == off::TYPE_SECTOR) {
            uint32_t np = 0;
            if (ru32(mem, parent + off::OBJ_NAME, np) && np && mem.cstr) {
                std::string n = mem.cstr(np, 64);
                if (!n.empty()) return n;
            }
        }
    }
    return "";
}

std::vector<ObjInfo> read_sector_objects(const Mem& mem, uint32_t sector_addr, size_t cap,
                                         const RangeCtx* rc) {
    std::vector<ObjInfo> out;
    if (!sector_addr) return out;

    uint32_t payload = 0;
    if (!ru32(mem, sector_addr + off::OBJ_PAYLOAD, payload) || !payload) return out;

    const uint32_t sentinel = payload + off::SEC_LIST_SENTINEL;
    uint32_t node = 0;
    if (!ru32(mem, payload + off::SEC_LIST_HEAD, node)) return out;

    size_t iter = 0;
    while (node && node != sentinel && iter < cap) {
        ++iter;

        // Visibility gate — replicate the game's gravidar / sector-map filter so
        // we show the player-visible subset, not the engine's omniscient list:
        //   hide if (+0x40 & 0x11)        internal sub-part / non-contact flag
        //   hide if (+0x44 & 0x28000000)  bit29: camera/debris/wreck/nav-special;
        //                                 bit27: internal/despawn (the *** UNDEFINED ***
        //                                 type-20 objects the in-game map also drops)
        // (read failures leave the flag 0 -> treated as visible, e.g. in tests).
        uint32_t f40 = 0, f44 = 0;
        ru32(mem, node + off::OBJ_FLAGS,  f40);
        ru32(mem, node + off::OBJ_FLAGS2, f44);
        bool hidden = (f40 & 0x11u) || (f44 & 0x28000000u);

        if (!hidden) {
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

            // Jump gates: show the destination sector instead of just "Gate".
            if (o.type == off::TYPE_GATE) {
                std::string dest = gate_dest_sector_name(mem, node);
                if (!dest.empty()) o.name = "-> " + dest;
            }

            // Distance from the player (for display + the scanner-range gate).
            if (rc && rc->valid) {
                int64_t dx = (int64_t)o.x - rc->px;
                int64_t dy = (int64_t)o.y - rc->py;
                int64_t dz = (int64_t)o.z - rc->pz;
                o.dist = (int32_t)std::llround(std::sqrt((double)(dx*dx + dy*dy + dz*dz)));
            }
            // Ships beyond the scanner range are hidden (matching the gravidar),
            // unless the object is already known (+0x40 & 0x8).
            bool out_of_range = (rc && rc->valid && o.type == off::TYPE_SHIP &&
                                 !(f40 & off::FLAG_KNOWN) && (int64_t)o.dist > rc->range);
            if (!out_of_range)
                out.push_back(std::move(o));
        }

        uint32_t next = 0;
        if (!ru32(mem, node + off::OBJ_NEXT, next)) break;
        node = next;
    }
    return out;
}

// Resolve player position + scanner range for the visibility test. Walks the
// gravidar out of the HUD manager's hashmap (the one whose scanner source == the
// player ship) and reads its cached scanner range. Returns {valid=false} if
// anything is unavailable -> no range filtering.
static RangeCtx resolve_range_ctx(const Mem& mem, uint32_t scene) {
    RangeCtx rc;
    uint32_t player = 0, ptf = 0, px = 0, py = 0, pz = 0;
    if (!ru32(mem, scene + off::SCENE_PLAYER_ANCHOR, player) || !player) return rc;
    if (!ru32(mem, player + off::OBJ_TRANSFORM, ptf) || !ptf) return rc;
    if (!ru32(mem, ptf + off::TF_X, px) || !ru32(mem, ptf + off::TF_Y, py) ||
        !ru32(mem, ptf + off::TF_Z, pz)) return rc;

    uint32_t hud = 0, hmap = 0, buckets = 0, mask = 0;
    if (!ru32(mem, off::HUD_MGR_GLOBAL, hud) || !hud) return rc;
    if (!ru32(mem, hud, hmap) || !hmap) return rc;                       // hashmap = *(hud)
    if (!ru32(mem, hmap + off::HASH_BUCKETS, buckets) || !buckets) return rc;
    ru32(mem, hmap + off::HASH_MASK, mask);

    uint32_t range = 0;
    uint32_t lim = (mask < 1024) ? mask : 1024;
    for (uint32_t i = 0; i <= lim && !range; ++i) {
        uint32_t node = 0;
        if (!ru32(mem, buckets + i * 4, node)) continue;
        for (int guard = 0; node && guard < 64; ++guard) {
            uint32_t grav = 0, src = 0;
            if (ru32(mem, node + off::HASH_NODE_VALUE, grav) && grav &&
                ru32(mem, grav + off::GRAVIDAR_SOURCE, src) && src == player) {
                ru32(mem, grav + off::GRAVIDAR_RANGE, range);
                break;
            }
            uint32_t nxt = 0;
            if (!ru32(mem, node + off::HASH_NODE_NEXT, nxt)) break;
            node = nxt;
        }
    }
    if (!range) return rc;

    rc.valid = true;
    rc.px = (int32_t)px; rc.py = (int32_t)py; rc.pz = (int32_t)pz;
    rc.range = (int64_t)(int32_t)range;
    return rc;
}

SectorView read_current_sector(const Mem& mem, uint32_t scene_global, size_t cap) {
    SectorView v;
    uint32_t scene = 0;
    if (!ru32(mem, scene_global, scene) || !scene) return v;

    // Primary: the current sector hangs directly off the scene at +0x08 (type 1).
    uint32_t sector = 0, cand = 0;
    if (ru32(mem, scene + off::SCENE_SECTOR, cand) && cand &&
        read_type(mem, cand) == off::TYPE_SECTOR)
        sector = cand;

    // Fallback: climb the player ship's container chain (+0x54) up to the sector
    // (handles e.g. ship -> docked station -> sector).
    if (!sector) {
        uint32_t cur = 0;
        if (ru32(mem, scene + off::SCENE_PLAYER_ANCHOR, cur)) {
            for (int k = 0; k < 8 && cur; ++k) {
                if (read_type(mem, cur) == off::TYPE_SECTOR) { sector = cur; break; }
                uint32_t parent = 0;
                if (!ru32(mem, cur + off::OBJ_PARENT, parent)) break;
                cur = parent;
            }
        }
    }
    if (!sector) return v;

    v.in_sector   = true;
    v.sector_addr = sector;
    uint32_t nameptr = 0;
    if (ru32(mem, sector + off::OBJ_NAME, nameptr) && nameptr && mem.cstr)
        v.sector_name = mem.cstr(nameptr, 64);
    RangeCtx rc = resolve_range_ctx(mem, scene);
    v.scanner_range = rc.valid ? rc.range : 0;
    // Player position for the 3D map center (independent of scanner range).
    {
        uint32_t player = 0, ptf = 0, px = 0, py = 0, pz = 0;
        if (ru32(mem, scene + off::SCENE_PLAYER_ANCHOR, player) && player &&
            ru32(mem, player + off::OBJ_TRANSFORM, ptf) && ptf &&
            ru32(mem, ptf + off::TF_X, px) && ru32(mem, ptf + off::TF_Y, py) &&
            ru32(mem, ptf + off::TF_Z, pz)) {
            v.player_valid = true;
            v.player_x = (int32_t)px; v.player_y = (int32_t)py; v.player_z = (int32_t)pz;
        }
    }
    v.objects = read_sector_objects(mem, sector, cap, &rc);

    // Stable display order by object id, so the live 6 Hz refresh doesn't reshuffle
    // rows under the user (which made the table scroll snap/twitch).
    std::sort(v.objects.begin(), v.objects.end(),
              [](const ObjInfo& a, const ObjInfo& b) { return a.id < b.id; });
    return v;
}

const char* type_label(int16_t type) {
    switch (type) {
        case off::TYPE_SECTOR:   return "Sector";
        case off::TYPE_SUN:      return "Sun";
        case off::TYPE_PLANET:   return "Planet";
        case off::TYPE_STATION:  return "Station";
        case off::TYPE_FACTORY:  return "Factory";
        case off::TYPE_SHIP:     return "Ship";
        case off::TYPE_WARE:     return "Ware";
        case off::TYPE_ASTEROID: return "Asteroid";
        case off::TYPE_GATE:     return "Gate";
        default:                 return "?";
    }
}

} // namespace x3
