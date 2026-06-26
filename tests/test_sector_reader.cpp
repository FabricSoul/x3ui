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

int main() {
    RUN(test_null_scene);
    REPORT();
}
