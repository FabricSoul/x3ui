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
    fm.w(0x4000 + 0x00, 0x5000);
    fm.w(0x4000 + 0x48, 7);
    fm.w(0x4000 + 0x0c, 0x9100);  fm.s(0x9100, "Visible Ship");
    // node2: hidden via +0x40 bit4 (0x10)
    fm.w(0x5000 + 0x00, 0x6000);
    fm.w(0x5000 + 0x48, 7);
    fm.w(0x5000 + 0x40, 0x10);
    fm.w(0x5000 + 0x0c, 0x9200);  fm.s(0x9200, "Hidden A");
    // node3: hidden via +0x44 bit29 (0x20000000)
    fm.w(0x6000 + 0x00, 0x3004);     // -> sentinel
    fm.w(0x6000 + 0x48, 20);
    fm.w(0x6000 + 0x44, 0x20000000);
    fm.w(0x6000 + 0x0c, 0x9300);  fm.s(0x9300, "Hidden B");

    auto v = x3::read_current_sector(fm.mem(), x3::off_SCENE_GLOBAL);
    CHECK_EQ(v.objects.size(), (size_t)1);
    CHECK_STR(v.objects[0].name.c_str(), "Visible Ship");
}

int main() {
    RUN(test_null_scene);
    RUN(test_visibility_filter);
    RUN(test_two_objects);
    RUN(test_fallback_climbs_to_sector);
    RUN(test_cycle_is_capped);
    RUN(test_null_name);
    RUN(test_null_transform);
    RUN(test_type_label);
    REPORT();
}
