#include "check.h"
#include "fake_mem.h"
#include "debug_server.h"

// Same synthetic graph as the sector-reader happy-path test:
//   scene 0x1000; scene+0x38 -> sector 0x2000 (type 7, "Argon Prime", payload 0x3000)
//   node1 0x4000 (ship, id 101, "Player Ship", type 5, pos 1000,-2000,3000)
//   node2 0x5000 (station, id 202, "Trade Stn", type 6)
static void build(FakeMem &fm) {
    fm.w(0x0837a0d0, 0x1000);
    fm.w(0x1000 + 0x38, 0x2000);
    fm.w(0x2000 + 0x48, 7);
    fm.w(0x2000 + 0x0c, 0x9000);  fm.s(0x9000, "Argon Prime");
    fm.w(0x2000 + 0x50, 0x3000);
    fm.w(0x3000 + 0x00, 0x4000);
    fm.w(0x4000 + 0x00, 0x5000);
    fm.w(0x4000 + 0x08, 101);
    fm.w(0x4000 + 0x48, 5);
    fm.w(0x4000 + 0x0c, 0x9100);  fm.s(0x9100, "Player Ship");
    fm.w(0x4000 + 0x6c, 0x6000);
    fm.w(0x6000 + 0x2c, (uint32_t)1000);
    fm.w(0x6000 + 0x30, (uint32_t)(int32_t)-2000);
    fm.w(0x6000 + 0x34, (uint32_t)3000);
    fm.w(0x5000 + 0x00, 0x3004);
    fm.w(0x5000 + 0x08, 202);
    fm.w(0x5000 + 0x48, 6);
    fm.w(0x5000 + 0x0c, 0x9200);  fm.s(0x9200, "Trade Stn");
}

static bool has(const std::string &s, const char *sub) {
    return s.find(sub) != std::string::npos;
}

static void test_ping() {
    FakeMem fm;
    CHECK_STR(x3::handle_request(fm.mem(), "ping").c_str(), "pong");
}

static void test_read_and_cstr() {
    FakeMem fm; build(fm);
    x3::Mem m = fm.mem();
    CHECK_STR(x3::handle_request(m, "read 0x1038").c_str(), "ok 0x00002000 8192");
    CHECK_STR(x3::handle_request(m, "cstr 0x9000").c_str(), "ok \"Argon Prime\"");
}

static void test_type_and_obj() {
    FakeMem fm; build(fm);
    x3::Mem m = fm.mem();
    CHECK_STR(x3::handle_request(m, "type 0x4000").c_str(), "ok type=5 sub=0");
    std::string o = x3::handle_request(m, "obj 0x4000");
    CHECK(has(o, "id=101"));
    CHECK(has(o, "type=5"));
    CHECK(has(o, "name=\"Player Ship\""));
    CHECK(has(o, "pos=1000,-2000,3000"));
}

static void test_scene_and_sector() {
    FakeMem fm; build(fm);
    x3::Mem m = fm.mem();
    std::string sc = x3::handle_request(m, "scene");
    CHECK(has(sc, "anchor=0x00002000"));
    CHECK(has(sc, "type=7"));
    CHECK(has(sc, "Argon Prime"));

    std::string s = x3::handle_request(m, "sector");
    CHECK(has(s, "in_sector=1"));
    CHECK(has(s, "count=2"));
    CHECK(has(s, "Player Ship"));
    CHECK(has(s, "Trade Stn"));
}

static void test_chain_and_unknown() {
    FakeMem fm; build(fm);
    x3::Mem m = fm.mem();
    // chain from node1 (ship): lvl0 ship(type5) -> its parent +0x54 (unset -> stops or 0)
    std::string ch = x3::handle_request(m, "chain 0x4000");
    CHECK(has(ch, "lvl=0 addr=0x00004000 type=5"));
    CHECK_STR(x3::handle_request(m, "frobnicate").c_str(), "err unknown op");
}

int main() {
    RUN(test_ping);
    RUN(test_read_and_cstr);
    RUN(test_type_and_obj);
    RUN(test_scene_and_sector);
    RUN(test_chain_and_unknown);
    REPORT();
}
