#include "check.h"
#include "fake_mem.h"
#include "sector_reader.h"

// Not in a game: scene-mgr global reads as 0 -> no sector, no objects.
static void test_null_scene() {
    FakeMem fm; // empty: every read fails
    x3::SectorView v = x3::read_current_sector(fm.mem(), x3::off_SCENE_GLOBAL);
    CHECK(!v.in_sector);
    CHECK(v.objects.empty());
}

// Synthetic graph:
//   scene_global(0x0837a0d0) -> scene 0x1000
//   scene+0x38 -> sector 0x2000 (type 7), name "Argon Prime", payload 0x3000
//   payload 0x3000: head -> node1 0x4000 ; sentinel = 0x3004
//   node1 0x4000: next->0x5000 id=101 type=5(ship) name "Player Ship" tf 0x6000 (1000,-2000,3000)
//   node2 0x5000: next->sentinel id=202 type=6(station) name "Trade Stn" tf 0x6100 (50,60,70)
static void test_two_objects() {
    FakeMem fm;
    // scene-mgr
    fm.w(0x0837a0d0, 0x1000);          // scene_global -> scene
    fm.w(0x1000 + 0x38, 0x2000);       // scene+0x38 -> sector
    // sector 0x2000 (type 7)
    fm.w(0x2000 + 0x48, 7);            // type = 7 (subtype 0 in high half)
    fm.w(0x2000 + 0x0c, 0x9000);       // name ptr
    fm.s(0x9000, "Argon Prime");
    fm.w(0x2000 + 0x50, 0x3000);       // payload
    // payload 0x3000 list: head -> node1, sentinel = 0x3000+4 = 0x3004
    fm.w(0x3000 + 0x00, 0x4000);       // first node
    // node1 0x4000 (ship)
    fm.w(0x4000 + 0x00, 0x5000);       // next -> node2
    fm.w(0x4000 + 0x08, 101);          // id
    fm.w(0x4000 + 0x48, 5);            // type 5 ship
    fm.w(0x4000 + 0x0c, 0x9100);       // name
    fm.s(0x9100, "Player Ship");
    fm.w(0x4000 + 0x6c, 0x6000);       // transform
    fm.w(0x6000 + 0x2c, (uint32_t)1000);
    fm.w(0x6000 + 0x30, (uint32_t)(int32_t)-2000);
    fm.w(0x6000 + 0x34, (uint32_t)3000);
    // node2 0x5000 (station)
    fm.w(0x5000 + 0x00, 0x3004);       // next -> sentinel (end)
    fm.w(0x5000 + 0x08, 202);          // id
    fm.w(0x5000 + 0x48, 6);            // type 6 station
    fm.w(0x5000 + 0x0c, 0x9200);       // name
    fm.s(0x9200, "Trade Stn");
    fm.w(0x5000 + 0x6c, 0x6100);       // transform
    fm.w(0x6100 + 0x2c, (uint32_t)50);
    fm.w(0x6100 + 0x30, (uint32_t)60);
    fm.w(0x6100 + 0x34, (uint32_t)70);

    x3::SectorView v = x3::read_current_sector(fm.mem(), x3::off_SCENE_GLOBAL);
    CHECK(v.in_sector);
    CHECK_EQ(v.sector_addr, 0x2000u);
    CHECK_STR(v.sector_name.c_str(), "Argon Prime");
    CHECK_EQ(v.objects.size(), (size_t)2);

    CHECK_EQ(v.objects[0].id, 101u);
    CHECK_EQ(v.objects[0].type, (int16_t)5);
    CHECK_STR(v.objects[0].name.c_str(), "Player Ship");
    CHECK_EQ(v.objects[0].x, 1000);
    CHECK_EQ(v.objects[0].y, -2000);
    CHECK_EQ(v.objects[0].z, 3000);

    CHECK_EQ(v.objects[1].id, 202u);
    CHECK_EQ(v.objects[1].type, (int16_t)6);
    CHECK_STR(v.objects[1].name.c_str(), "Trade Stn");
    CHECK_EQ(v.objects[1].x, 50);
    CHECK_EQ(v.objects[1].y, 60);
    CHECK_EQ(v.objects[1].z, 70);
}

int main() {
    RUN(test_null_scene);
    RUN(test_two_objects);
    REPORT();
}
