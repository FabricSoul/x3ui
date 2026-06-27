#include "sector_reader.h"
#include "struct_offsets.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

namespace x3 {

static bool ru32(const Mem& m, uint32_t addr, uint32_t& out) {
    return m.u32 && m.u32(addr, out);
}

// Resolve a {textPage, textID} reference via the engine's text lookup (the entry's
// string is stored inline as a 0x0b-marker reference that needs the engine's
// expander, so we call the game's get_text_simple through mem.text rather than
// re-implement it). "" if unavailable or not found.
std::string resolve_text(const Mem& mem, uint32_t page, uint32_t id) {
    return mem.text ? mem.text(page, id) : std::string();
}

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

namespace {
// 3x4 affine transform, row-major; column 3 is translation.
struct Mat34 { float m[12]; };
Mat34 mat_identity() { Mat34 r{}; r.m[0] = r.m[5] = r.m[10] = 1.0f; return r; }
Mat34 mat_mul(const Mat34& a, const Mat34& b) {  // a then b's parent: a ∘ b
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
// CALIBRATE LIVE — see the plan's live-calibration task. If parts sit too
// close/far, adjust this.
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
        // Hard cap: append a part only if the whole part fits (keeps edges valid).
        if (part.ok &&
            out.verts.size() / 3 + part.verts.size() / 3 <= vcap &&
            out.edges.size() / 2 + part.edges.size() / 2 <= ecap) {
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

// Replace each "{page, id}" token in a name with its resolved text. Tokens that
// don't resolve are left literal (so this never makes a name worse). The engine's
// own expander skips these when there's a space after the comma, which is why the
// raw "{17, 6651}" leaks through to the display.
// A resolved name must be clean printable ASCII; otherwise we treat the lookup as
// failed (a wrong walk must never replace a token with garbage).
static bool is_clean_text(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) if (c < 0x20 || c > 0x7e) return false;
    return true;
}

static std::string expand_braces(const Mem& mem, const std::string& in) {
    if (in.find('{') == std::string::npos) return in;
    std::string out;
    for (size_t i = 0; i < in.size();) {
        if (in[i] == '{') {
            size_t close = in.find('}', i);
            size_t comma = (close == std::string::npos) ? close : in.find(',', i);
            if (close != std::string::npos && comma != std::string::npos && comma < close) {
                long pg = strtol(in.c_str() + i + 1, nullptr, 10);
                long id = strtol(in.c_str() + comma + 1, nullptr, 10);
                if (pg > 0 && id > 0) {
                    std::string r = resolve_text(mem, (uint32_t)pg, (uint32_t)id);
                    if (is_clean_text(r)) { out += r; i = close + 1; continue; }
                }
            }
        }
        out.push_back(in[i++]);
    }
    return out;
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
        //   hide if (+0x40 & 0x1)         internal / non-contact (suns, planets,
        //                                 wares/cargo). NOTE: bit4 (0x10) is set on
        //                                 ~60% of normal ships (incl. visible pirate
        //                                 wings) and must NOT hide them.
        //   hide if (+0x44 & 0x28000000)  bit29: camera/debris/wreck/nav-special;
        //                                 bit27: internal/despawn (the *** UNDEFINED ***
        //                                 type-20 objects the in-game map also drops)
        // (read failures leave the flag 0 -> treated as visible, e.g. in tests).
        uint32_t f40 = 0, f44 = 0, f3c = 0;
        ru32(mem, node + off::OBJ_FLAGS,  f40);
        ru32(mem, node + off::OBJ_FLAGS2, f44);
        ru32(mem, node + off::OBJ_RACE,   f3c);
        // ...also drop spaceflies (race 13 wildlife) — clutter, not a contact.
        bool hidden = (f40 & 0x1u) || (f44 & 0x28000000u) ||
                      ((int16_t)(f3c & 0xFFFF) == off::SPACEFLY_RACE);

        if (!hidden) {
            ObjInfo o;
            o.addr = node;
            uint32_t w = 0;
            if (ru32(mem, node + off::OBJ_ID, w)) o.id = w;
            if (ru32(mem, node + off::OBJ_TYPE, w)) {
                o.type    = (int16_t)(w & 0xFFFF);
                o.subtype = (int16_t)(w >> 16);
            }
            uint32_t nameptr = 0;
            if (ru32(mem, node + off::OBJ_NAME, nameptr) && nameptr && mem.cstr) {
                o.name = mem.cstr(nameptr, 64);
                o.name = expand_braces(mem, o.name);   // resolve "{17, 6651}"-style refs
            }
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

            // Owner/race + IFF relation to the player (matches the game's blip color):
            // only ships/stations/factories carry a relation; +0x44 bit30 disables it.
            o.race = (int16_t)(f3c & 0xFFFF);
            bool has_iff = (o.type == off::TYPE_SHIP || o.type == off::TYPE_STATION ||
                            o.type == off::TYPE_FACTORY) && !(f44 & off::NO_IFF_FLAG);
            if (rc && has_iff) {
                if (rc->player_ship && node == rc->player_ship) {
                    o.relation = REL_OWN;   // own = the player's own ship (only)
                } else {
                    uint32_t pl = 0;
                    if (ru32(mem, node + off::OBJ_PAYLOAD, pl) && pl &&
                        rc->player_race >= 0 && rc->player_race < 32) {
                        uint32_t fr = 0, fo = 0, bit = 1u << rc->player_race;
                        ru32(mem, pl + off::REL_FRIEND_MASK, fr);
                        ru32(mem, pl + off::REL_FOE_MASK, fo);
                        o.relation = (fr & bit) ? REL_FRIEND
                                   : (fo & bit) ? REL_ENEMY
                                                : REL_NEUTRAL;
                    }
                }
            }

            // Distance from the player (for display + the scanner-range gate).
            if (rc && rc->valid) {
                int64_t dx = (int64_t)o.x - rc->px;
                int64_t dy = (int64_t)o.y - rc->py;
                int64_t dz = (int64_t)o.z - rc->pz;
                o.dist = (int32_t)std::llround(std::sqrt((double)(dx*dx + dy*dy + dz*dz)));
            }
            // Ships beyond the scanner range are hidden, matching the gravidar.
            // (The +0x40 bit3 "0x8" is set ON out-of-range contacts here, so it is
            // NOT a "show anyway" flag — we cull every ship past the range.)
            // Out-of-range SHIPS are hidden (gravidar behavior); stations, gates and
            // asteroids are not ships, so they always show regardless of range.
            bool out_of_range = (rc && rc->valid &&
                                 o.type == off::TYPE_SHIP && (int64_t)o.dist > rc->range);
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

    // Player race + own ship for the IFF relation (set even if the scanner range
    // below can't be resolved -> coloring still works without range filtering).
    uint32_t prace = 0;
    rc.player_ship = player;
    if (ru32(mem, player + off::OBJ_RACE, prace)) rc.player_race = (int16_t)(prace & 0xFFFF);

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
            // Ship forward vector (transform-node 3x3, 16.16 fixed; forward = column 2).
            uint32_t fx = 0, fy = 0, fz = 0;
            if (ru32(mem, ptf + off::TF_FWD_X, fx) && ru32(mem, ptf + off::TF_FWD_Y, fy) &&
                ru32(mem, ptf + off::TF_FWD_Z, fz)) {
                v.player_orient_valid = true;
                v.player_fwd_x = (int32_t)fx / off::FIXED16;
                v.player_fwd_y = (int32_t)fy / off::FIXED16;
                v.player_fwd_z = (int32_t)fz / off::FIXED16;
            }
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
