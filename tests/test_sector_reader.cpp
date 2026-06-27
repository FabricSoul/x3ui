#include "check.h"
#include "fake_mem.h"
#include "sector_reader.h"

// Live-verified model: current sector = *(scene+0x08), type 1.
// Types: 1=Sector 3=Sun 4=Planet 5=Station 6=Factory 7=Ship 9=Ware 0x11=Asteroid 0x12=Gate.

// Not in a game: scene-mgr global reads as 0 -> no sector, no objects.
static void test_null_scene() {
    FakeMem fm; // empty: every read fails
    x3::SectorView v = x3::read_current_sector(fm.mem(), x3::off_SCENE_GLOBAL);
    CHECK(!v.in_sector);
    CHECK(v.objects.empty());
}

// Synthetic graph:
//   scene 0x1000; scene+0x08 -> sector 0x2000 (type 1, "Argon Prime", payload 0x3000)
//   payload head -> node1 0x4000 ; sentinel = 0x3004
//   node1 0x4000: next->0x5000 id=101 type=7(ship) "Player Ship" tf 0x6000 (1000,-2000,3000)
//   node2 0x5000: next->sentinel id=202 type=5(station) "Trade Stn"
static void test_two_objects() {
    FakeMem fm;
    fm.w(0x0837a0d0, 0x1000);
    fm.w(0x1000 + 0x08, 0x2000);   // scene+0x08 -> sector
    fm.w(0x2000 + 0x48, 1);        // type 1 = sector
    fm.w(0x2000 + 0x0c, 0x9000);   fm.s(0x9000, "Argon Prime");
    fm.w(0x2000 + 0x50, 0x3000);   // payload
    fm.w(0x3000 + 0x00, 0x4000);   // first node
    // node1 (ship)
    fm.w(0x4000 + 0x00, 0x5000);
    fm.w(0x4000 + 0x08, 101);
    fm.w(0x4000 + 0x48, 7);        // type 7 = ship
    fm.w(0x4000 + 0x0c, 0x9100);   fm.s(0x9100, "Player Ship");
    fm.w(0x4000 + 0x6c, 0x6000);
    fm.w(0x6000 + 0x2c, (uint32_t)1000);
    fm.w(0x6000 + 0x30, (uint32_t)(int32_t)-2000);
    fm.w(0x6000 + 0x34, (uint32_t)3000);
    // node2 (station)
    fm.w(0x5000 + 0x00, 0x3004);   // -> sentinel (end)
    fm.w(0x5000 + 0x08, 202);
    fm.w(0x5000 + 0x48, 5);        // type 5 = station
    fm.w(0x5000 + 0x0c, 0x9200);   fm.s(0x9200, "Trade Stn");

    x3::SectorView v = x3::read_current_sector(fm.mem(), x3::off_SCENE_GLOBAL);
    CHECK(v.in_sector);
    CHECK_EQ(v.sector_addr, 0x2000u);
    CHECK_STR(v.sector_name.c_str(), "Argon Prime");
    CHECK_EQ(v.objects.size(), (size_t)2);

    CHECK_EQ(v.objects[0].id, 101u);
    CHECK_EQ(v.objects[0].type, (int16_t)7);
    CHECK_STR(v.objects[0].name.c_str(), "Player Ship");
    CHECK_EQ(v.objects[0].x, 1000);
    CHECK_EQ(v.objects[0].y, -2000);
    CHECK_EQ(v.objects[0].z, 3000);

    CHECK_EQ(v.objects[1].id, 202u);
    CHECK_EQ(v.objects[1].type, (int16_t)5);
    CHECK_STR(v.objects[1].name.c_str(), "Trade Stn");
}

// When scene+0x08 isn't a valid sector, fall back to climbing the player ship's
// parent chain (ship -> docked station -> sector), like the docked Mercury case.
static void test_fallback_climbs_to_sector() {
    FakeMem fm;
    fm.w(0x0837a0d0, 0x1000);
    // (no scene+0x08 -> primary path fails)
    fm.w(0x1000 + 0x38, 0x2000);   // player ship
    fm.w(0x2000 + 0x48, 7);        // ship
    fm.w(0x2000 + 0x54, 0x2200);   // parent -> station
    fm.w(0x2200 + 0x48, 5);        // station
    fm.w(0x2200 + 0x54, 0x2500);   // parent -> sector
    fm.w(0x2500 + 0x48, 1);        // sector
    fm.w(0x2500 + 0x50, 0x3000);
    fm.w(0x3000 + 0x00, 0x3004);   // empty list

    x3::SectorView v = x3::read_current_sector(fm.mem(), x3::off_SCENE_GLOBAL);
    CHECK(v.in_sector);
    CHECK_EQ(v.sector_addr, 0x2500u);
    CHECK(v.objects.empty());
}

// A cyclic list (no sentinel reached) must terminate at `cap`, not hang.
static void test_cycle_is_capped() {
    FakeMem fm;
    fm.w(0x2000 + 0x50, 0x3000);   // payload
    fm.w(0x3000 + 0x00, 0x4000);   // head -> node
    fm.w(0x4000 + 0x00, 0x4000);   // node.next -> itself (cycle)
    fm.w(0x4000 + 0x48, 7);

    auto objs = x3::read_sector_objects(fm.mem(), 0x2000, /*cap=*/3);
    CHECK_EQ(objs.size(), (size_t)3);   // stopped at cap, did not hang
}

// Null name pointer -> empty name (no crash).
static void test_null_name() {
    FakeMem fm;
    fm.w(0x0837a0d0, 0x1000);
    fm.w(0x1000 + 0x08, 0x2000);
    fm.w(0x2000 + 0x48, 1);
    fm.w(0x2000 + 0x50, 0x3000);
    fm.w(0x3000 + 0x00, 0x4000);
    fm.w(0x4000 + 0x00, 0x3004);   // next -> sentinel
    fm.w(0x4000 + 0x48, 7);
    fm.w(0x4000 + 0x0c, 0);        // name ptr = null

    auto v = x3::read_current_sector(fm.mem(), x3::off_SCENE_GLOBAL);
    CHECK_EQ(v.objects.size(), (size_t)1);
    CHECK(v.objects[0].name.empty());
}

// Null transform pointer -> position stays 0 (no crash).
static void test_null_transform() {
    FakeMem fm;
    fm.w(0x0837a0d0, 0x1000);
    fm.w(0x1000 + 0x08, 0x2000);
    fm.w(0x2000 + 0x48, 1);
    fm.w(0x2000 + 0x50, 0x3000);
    fm.w(0x3000 + 0x00, 0x4000);
    fm.w(0x4000 + 0x00, 0x3004);
    fm.w(0x4000 + 0x48, 7);
    fm.w(0x4000 + 0x6c, 0);        // transform = null

    auto v = x3::read_current_sector(fm.mem(), x3::off_SCENE_GLOBAL);
    CHECK_EQ(v.objects.size(), (size_t)1);
    CHECK_EQ(v.objects[0].x, 0);
    CHECK_EQ(v.objects[0].y, 0);
    CHECK_EQ(v.objects[0].z, 0);
}

static void test_type_label() {
    CHECK_STR(x3::type_label(1), "Sector");
    CHECK_STR(x3::type_label(3), "Sun");
    CHECK_STR(x3::type_label(4), "Planet");
    CHECK_STR(x3::type_label(5), "Station");
    CHECK_STR(x3::type_label(6), "Factory");
    CHECK_STR(x3::type_label(7), "Ship");
    CHECK_STR(x3::type_label(9), "Ware");
    CHECK_STR(x3::type_label(0x11), "Asteroid");
    CHECK_STR(x3::type_label(0x12), "Gate");
    CHECK_STR(x3::type_label(99), "?");
}

// Visibility gate: objects flagged internal/hidden are dropped, matching the
// game's gravidar / sector-map filter.
static void test_visibility_filter() {
    FakeMem fm;
    fm.w(0x0837a0d0, 0x1000);
    fm.w(0x1000 + 0x08, 0x2000);
    fm.w(0x2000 + 0x48, 1);          // sector
    fm.w(0x2000 + 0x50, 0x3000);
    fm.w(0x3000 + 0x00, 0x4000);     // head -> node1
    // node1: visible ship
    fm.w(0x4000 + 0x00, 0x4800);
    fm.w(0x4000 + 0x48, 7);
    fm.w(0x4000 + 0x0c, 0x9100);  fm.s(0x9100, "Visible Ship");
    // node1b: visible ship WITH +0x40 bit4 (0x10) — normal-ship flag, must show
    fm.w(0x4800 + 0x00, 0x5000);
    fm.w(0x4800 + 0x48, 7);
    fm.w(0x4800 + 0x40, 0x10);
    fm.w(0x4800 + 0x0c, 0x9150);  fm.s(0x9150, "Bit4 Ship");
    // node2: hidden via +0x40 bit0 (0x1) — internal / non-contact (ware/cargo)
    fm.w(0x5000 + 0x00, 0x6000);
    fm.w(0x5000 + 0x48, 9);
    fm.w(0x5000 + 0x40, 0x1);
    fm.w(0x5000 + 0x0c, 0x9200);  fm.s(0x9200, "Hidden A");
    // node3: hidden via +0x44 bit29 (0x20000000)
    fm.w(0x6000 + 0x00, 0x7000);     // -> node4
    fm.w(0x6000 + 0x48, 20);
    fm.w(0x6000 + 0x44, 0x20000000);
    fm.w(0x6000 + 0x0c, 0x9300);  fm.s(0x9300, "Hidden B");
    // node4: hidden via +0x44 bit27 (0x08000000) — the UNDEFINED type-20 case
    fm.w(0x7000 + 0x00, 0x3004);     // -> sentinel
    fm.w(0x7000 + 0x48, 20);
    fm.w(0x7000 + 0x44, 0x08000000);
    fm.w(0x7000 + 0x0c, 0x9400);  fm.s(0x9400, "Hidden C");

    auto v = x3::read_current_sector(fm.mem(), x3::off_SCENE_GLOBAL);
    CHECK_EQ(v.objects.size(), (size_t)2);   // Visible Ship + Bit4 Ship; bit0 + f44 hidden
}

// Scanner-range gate: a ship beyond the player's scanner range is dropped; one
// inside is kept. The range comes from the gravidar (HUD mgr hashmap -> the
// gravidar whose source == the player ship -> +0x240).
static void test_scanner_range_filter() {
    FakeMem fm;
    fm.w(0x0837a0d0, 0x1000);              // scene
    fm.w(0x1000 + 0x08, 0x2000);           // sector
    fm.w(0x1000 + 0x38, 0x8000);           // player ship
    fm.w(0x2000 + 0x48, 1);                // sector type
    fm.w(0x2000 + 0x50, 0x3000);           // payload
    fm.w(0x3000 + 0x00, 0x4000);           // head -> node1
    // player at origin
    fm.w(0x8000 + 0x6c, 0x8100);
    fm.w(0x8100 + 0x2c, 0); fm.w(0x8100 + 0x30, 0); fm.w(0x8100 + 0x34, 0);
    // HUD mgr -> hashmap -> buckets[0] -> node -> gravidar (source=player, range=20km)
    fm.w(0x0837a100, 0x9000);
    fm.w(0x9000 + 0x00, 0x9100);           // hashmap = *(hud)
    fm.w(0x9100 + 0x00, 0x9200);           // buckets
    fm.w(0x9100 + 0x04, 0);                // mask = 0 (one bucket)
    fm.w(0x9200 + 0x00, 0x9300);           // buckets[0] -> node
    fm.w(0x9300 + 0x08, 0x9400);           // node.value = gravidar
    fm.w(0x9400 + 0x10, 0x8000);           // gravidar source == player
    fm.w(0x9400 + 0x240, 10000000);        // scanner range = 20 km
    // node1: ship at 2 km (in range)
    fm.w(0x4000 + 0x00, 0x5000);
    fm.w(0x4000 + 0x08, 1);  fm.w(0x4000 + 0x48, 7);
    fm.w(0x4000 + 0x0c, 0x9a00); fm.s(0x9a00, "Near Ship");
    fm.w(0x4000 + 0x6c, 0x4100); fm.w(0x4100 + 0x2c, (uint32_t)1000000);
    // node2: ship at 30 km (out of range)
    fm.w(0x5000 + 0x00, 0x3004);
    fm.w(0x5000 + 0x08, 2);  fm.w(0x5000 + 0x48, 7);
    fm.w(0x5000 + 0x0c, 0x9b00); fm.s(0x9b00, "Far Ship");
    fm.w(0x5000 + 0x6c, 0x5100); fm.w(0x5100 + 0x2c, (uint32_t)15000000);

    auto v = x3::read_current_sector(fm.mem(), x3::off_SCENE_GLOBAL);
    CHECK(v.in_sector);
    CHECK_EQ(v.objects.size(), (size_t)1);
    CHECK_STR(v.objects[0].name.c_str(), "Near Ship");
}

// Jump gate shows the destination sector name (gate payload +0x58 dest array ->
// dest obj -> parent type-1 sector -> name).
static void test_gate_dest_name() {
    FakeMem fm;
    fm.w(0x0837a0d0, 0x1000);
    fm.w(0x1000 + 0x08, 0x2000);
    fm.w(0x2000 + 0x48, 1);
    fm.w(0x2000 + 0x50, 0x3000);
    fm.w(0x3000 + 0x00, 0x4000);     // head -> gate
    fm.w(0x4000 + 0x00, 0x3004);     // next -> sentinel
    fm.w(0x4000 + 0x48, 0x12);       // type = gate
    fm.w(0x4000 + 0x0c, 0x9000);  fm.s(0x9000, "Gate");
    fm.w(0x4000 + 0x50, 0x5000);     // gate payload
    fm.w(0x5000 + 0x58, 0x6000);     // dest array[0] -> dest obj
    fm.w(0x6000 + 0x54, 0x7000);     // dest obj -> parent
    fm.w(0x7000 + 0x48, 1);          // parent is a sector
    fm.w(0x7000 + 0x0c, 0x9100);  fm.s(0x9100, "Argon Prime");

    auto v = x3::read_current_sector(fm.mem(), x3::off_SCENE_GLOBAL);
    CHECK_EQ(v.objects.size(), (size_t)1);
    CHECK_STR(v.objects[0].name.c_str(), "-> Argon Prime");
}

// IFF relation matches the game: friend/foe via payload masks (bit = 1<<player_race),
// own via payload owner == player owner.
static void test_iff_relation() {
    FakeMem fm;
    fm.w(0x2000 + 0x50, 0x3000);     // sector payload
    fm.w(0x3000 + 0x00, 0x4000);     // head -> ship A
    fm.w(0x4000 + 0x00, 0x5000); fm.w(0x4000 + 0x48, 7);
    fm.w(0x4000 + 0x50, 0x4100); fm.w(0x4100 + 0x48, 0x2);   // friendMask bit1
    fm.w(0x5000 + 0x00, 0x6000); fm.w(0x5000 + 0x48, 7);
    fm.w(0x5000 + 0x50, 0x5100); fm.w(0x5100 + 0x4c, 0x2);   // foeMask bit1
    fm.w(0x6000 + 0x00, 0x7000); fm.w(0x6000 + 0x48, 7);
    fm.w(0x6000 + 0x50, 0x6100); fm.w(0x6100 + 0x48, 0x0);   // neither -> neutral
    fm.w(0x7000 + 0x00, 0x3004); fm.w(0x7000 + 0x48, 7);     // -> sentinel (the player's own ship)
    fm.w(0x7000 + 0x50, 0x7100);

    x3::RangeCtx rc;
    rc.player_race = 1;              // bit 0x2
    rc.player_ship = 0x7000;         // own = the player's own ship node
    auto objs = x3::read_sector_objects(fm.mem(), 0x2000, 4096, &rc);
    CHECK_EQ(objs.size(), (size_t)4);
    CHECK_EQ((int)objs[0].relation, (int)x3::REL_FRIEND);
    CHECK_EQ((int)objs[1].relation, (int)x3::REL_ENEMY);
    CHECK_EQ((int)objs[2].relation, (int)x3::REL_NEUTRAL);
    CHECK_EQ((int)objs[3].relation, (int)x3::REL_OWN);
}

static void test_text_db_resolve() {
    FakeMem fm;
    fm.t(17, 6651, "Trading Station");
    fm.t(17, 100,  "Alpha");
    CHECK_STR(x3::resolve_text(fm.mem(), 17, 6651).c_str(), "Trading Station");
    CHECK_STR(x3::resolve_text(fm.mem(), 17, 100).c_str(), "Alpha");
    CHECK(x3::resolve_text(fm.mem(), 17, 9999).empty());   // not found
}

// A station named "{17, 6651} {17, 7740}" (space after comma -> engine leaves it
// raw) gets each token resolved + joined: "Swarm Missile Production Complex alpha".
static void test_name_brace_expand() {
    FakeMem fm;
    fm.t(17, 6651, "Swarm Missile Production Complex");
    fm.t(17, 7740, "alpha");
    fm.w(0x2000 + 0x50, 0x3000);   fm.w(0x3000, 0x4000);
    fm.w(0x4000, 0x3004);          fm.w(0x4000 + 0x48, 5);
    fm.w(0x4000 + 0x0c, 0x9000);   fm.s(0x9000, "{17, 6651} {17, 7740}");
    auto objs = x3::read_sector_objects(fm.mem(), 0x2000);
    CHECK_EQ(objs.size(), (size_t)1);
    CHECK_STR(objs[0].name.c_str(), "Swarm Missile Production Complex alpha");
}

// A tiny model (3 verts, 1 triangle) read through the model cache + LOD mesh.
static void test_mesh_wireframe() {
    FakeMem fm;
    uint32_t obj = 0x5000;
    fm.w(obj + 0x12c, 10);             // model id
    fm.w(obj + 0x138, 0);              // lod index
    // resident model list: world *(0x082a0640) -> head *(world+0x8) -> model 0x6000
    fm.w(0x082a0640, 0x100);           // g_world_root pointer
    fm.w(0x100 + 0x8, 0x6000);         // list head -> model
    fm.w(0x6000 + 0x8, 10);            // model.id == obj model id
    fm.w(0x6000 + 0x4, 0);             // model.next = end
    fm.w(0x6000 + 0xc, 0x7000);        // model.lodarr
    fm.w(0x6000 + 0x10, 2);            // model.lodcount
    fm.w(0x7000 + 1 * 4, 0x8000);      // lod[coarsest=1] -> mesh
    fm.w(0x8000 + 0, 3);               // vcount
    fm.w(0x8000 + 8, 0x9000);          // verts
    fm.w(0x8000 + 4, 1);               // part count
    fm.w(0x8000 + 0x10, 0xA000);       // parts
    fm.w(0x9000 + 0, 0x00004000); fm.w(0x9000 + 4, 0);          // v0 = (1,0,0)
    fm.w(0x9018 + 0, 0x40000000); fm.w(0x9018 + 4, 0);          // v1 = (0,1,0)
    fm.w(0x9030 + 0, 0);          fm.w(0x9030 + 4, 0x00004000); // v2 = (0,0,1)
    fm.w(0xA000 + 0, 0xB000);          // part[0]
    fm.w(0xB000 + 0, 1);               // group count
    fm.w(0xB000 + 4, 0xC000);          // groups
    fm.w(0xC000 + 0, 1);               // triangle count
    fm.w(0xC000 + 4, 0xD000);          // triangles
    fm.w(0xD000 + 0, 0); fm.w(0xD000 + 4, 1); fm.w(0xD000 + 8, 2);  // tri (0,1,2)

    auto w = x3::read_mesh_wireframe(fm.mem(), obj);
    CHECK(w.ok);
    CHECK_EQ(w.vcount(), (size_t)3);
    CHECK_EQ(w.edges.size(), (size_t)6);          // 3 edges * 2 indices
    CHECK(w.verts[0] > 0.99f && w.verts[0] < 1.01f);   // v0.x = 16384/16384 = 1.0
    CHECK(w.verts[4] > 0.99f && w.verts[4] < 1.01f);   // v1.y = 1.0
}

int main() {
    RUN(test_null_scene);
    RUN(test_visibility_filter);
    RUN(test_scanner_range_filter);
    RUN(test_gate_dest_name);
    RUN(test_iff_relation);
    RUN(test_text_db_resolve);
    RUN(test_name_brace_expand);
    RUN(test_mesh_wireframe);
    RUN(test_two_objects);
    RUN(test_fallback_climbs_to_sector);
    RUN(test_cycle_is_capped);
    RUN(test_null_name);
    RUN(test_null_transform);
    RUN(test_type_label);
    REPORT();
}
