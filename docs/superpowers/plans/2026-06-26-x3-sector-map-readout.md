# X3 Sector Map Readout — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a read-only ImGui panel to the injected overlay that lists the objects (ships, stations, gates…) in the player's current X3 sector, read by walking the engine's in-memory object graph.

**Architecture:** Pure-logic core (`sector_reader`) walks an intrusive linked list of engine objects behind a tiny `Mem` read-abstraction, so it is unit-testable on the host with a fake address space. The overlay supplies a production `Mem` backed by the existing `/proc/self/mem` reader and renders the result in a throttled ImGui window. No game memory is written.

**Tech Stack:** C++17, Dear ImGui (OpenGL3 backend), GNU Make, `/proc/self/mem` `pread`. Unit tests are dependency-free (a tiny header of macros) and compile with plain `g++`.

---

## Context the implementer needs

- The engine is a 32-bit, non-PIE x86 binary (loaded at `0x08048000`), so absolute addresses like `0x0837a0d0` are valid at runtime. The overlay is `LD_PRELOAD`'d into it and renders from a `SDL_GL_SwapWindow` hook (engine main thread — safe for reads).
- **Existing read primitive** (`src/overlay.cpp:139`): `static bool mem_read_i32(uintptr_t addr, int32_t *out)` — a `pread` on `/proc/self/mem`. Returns `false` on a bad address instead of crashing. Call `mem_open()` once before using it (it opens the fd).
- **Existing draw path** (`src/overlay.cpp:648`): `overlay_draw()` runs `ImGui::NewFrame()`, draws windows, then `ImGui::Render()`. Other windows (`state_bridge_window()`, `ship_window()`) are plain `static void f(){ if(!ImGui::Begin(...)){ImGui::End();return;} … ImGui::End(); }` called before `ImGui::Render()`. We add `map_panel()` the same way.
- **Verified offsets** (from RE this session; single source of truth lives in `src/struct_offsets.h` after Task 1):
  - scene-mgr pointer is stored at `0x0837a0d0`.
  - object header: `next +0x00`, `prev +0x04`, `id +0x08`, `name(char*) +0x0c`, `type(i16) +0x48`, `subtype(i16) +0x4a`, `parent +0x54`, `transform +0x6c`, type-payload `+0x50`.
  - sector (type 7) payload (`*(sector+0x50)`) holds the child list: first node `*(payload+0x00)`, sentinel `payload+0x04`.
  - position via `T=*(obj+0x6c)`: x `+0x2c`, y `+0x30`, z `+0x34`.
  - type ids: ship 5, station 6, sector 7.
- **Known ambiguity to handle, not to resolve up front:** the existing `ship_window` treats `scene+0x38` as the *player ship*; the RE trace says `scene+0x38` is the *player sector* (type 7). The code resolves this at runtime by checking the type at `+0x48`: if it is 7 we use it as the sector; if it is a ship/station we follow its `parent +0x54` to the sector. Whichever is true, the walk works. Position int-vs-float is likewise confirmed live (the panel shows both).

## File structure

| File | Responsibility |
|---|---|
| `src/struct_offsets.h` (new) | `constexpr` offset/address/type constants. No logic. Single source of truth. |
| `src/sector_reader.h` (new) | `ObjInfo`, `Mem`, `SectorView` types + function declarations. No engine/ImGui deps. |
| `src/sector_reader.cpp` (new) | The walk logic. Includes only `sector_reader.h` + `struct_offsets.h`. |
| `tests/check.h` (new) | ~10-line dependency-free test-assert macros. |
| `tests/fake_mem.h` (new) | `FakeMem` — a `std::map`-backed fake address space producing a `x3::Mem`. |
| `tests/test_sector_reader.cpp` (new) | Unit tests for the walk. |
| `src/overlay.cpp` (modify) | Production `Mem` + `map_panel()` window; call it in `overlay_draw()`. |
| `Makefile` (modify) | Add `src/sector_reader.cpp` to `SRC`; add a `test` target. |

> **Note on git:** the repo is not yet under version control. Task 0 initializes it so the `Commit` steps work. If you choose not to use git, skip every step labeled **Commit**.

---

## Task 0: Initialize git (recommended)

**Files:**
- Create: `.gitignore`

- [ ] **Step 1: Init repo and ignore build artifacts**

```bash
cd /home/fabric/x3ui
git init
printf '%s\n' 'build/' 'libx3ui.so' 'test_so.so' '*.o' > .gitignore
git add -A
git commit -m "chore: initialize repo with current overlay state"
```

Expected: a first commit containing the existing sources. If you are skipping git, do nothing here.

---

## Task 1: Scaffolding + first failing test

Creates the headers, the test harness, the `make test` target, and a stub so the first test links and passes. This locks down the public types every later task depends on.

**Files:**
- Create: `src/struct_offsets.h`
- Create: `src/sector_reader.h`
- Create: `src/sector_reader.cpp`
- Create: `tests/check.h`
- Create: `tests/fake_mem.h`
- Create: `tests/test_sector_reader.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Write `src/struct_offsets.h`**

```cpp
#pragma once
#include <cstdint>

// Verified engine struct offsets (X3 32-bit engine, non-PIE @0x08048000).
// Single source of truth — do not hardcode these numbers elsewhere.
namespace x3 { namespace off {

// Global that holds the scene-manager pointer.
constexpr uint32_t SCENE_GLOBAL          = 0x0837a0d0u;

// scene-mgr fields
constexpr uint32_t SCENE_PLAYER_ANCHOR   = 0x38; // -> player sector (type 7) OR player ship

// object header
constexpr uint32_t OBJ_NEXT      = 0x00;
constexpr uint32_t OBJ_PREV      = 0x04;
constexpr uint32_t OBJ_ID        = 0x08;
constexpr uint32_t OBJ_NAME      = 0x0c; // char*
constexpr uint32_t OBJ_TYPE      = 0x48; // int16 (subtype packed in high half)
constexpr uint32_t OBJ_PARENT    = 0x54; // container; 0 = top-level
constexpr uint32_t OBJ_PAYLOAD   = 0x50; // type-specific payload ptr
constexpr uint32_t OBJ_TRANSFORM = 0x6c; // ptr; position lives inside

// sector (type 7) payload: child-object intrusive list
constexpr uint32_t SEC_LIST_HEAD     = 0x00; // *(payload+0) = first child node
constexpr uint32_t SEC_LIST_SENTINEL = 0x04; // node == payload+4 -> end of list

// transform: position (raw 32-bit words; int-vs-float confirmed live)
constexpr uint32_t TF_X = 0x2c;
constexpr uint32_t TF_Y = 0x30;
constexpr uint32_t TF_Z = 0x34;

// type ids (object +0x48)
constexpr int16_t TYPE_SHIP    = 5;
constexpr int16_t TYPE_STATION = 6;
constexpr int16_t TYPE_SECTOR  = 7;

}} // namespace x3::off
```

- [ ] **Step 2: Write `src/sector_reader.h`**

```cpp
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
```

- [ ] **Step 3: Write `tests/check.h`**

```cpp
#pragma once
#include <cstdio>
#include <cstring>

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond) do { ++g_checks; if(!(cond)) { ++g_fails; \
    std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while(0)

#define CHECK_EQ(a,b) do { ++g_checks; if(!((a)==(b))) { ++g_fails; \
    std::printf("  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while(0)

#define CHECK_STR(a,b) do { ++g_checks; if(std::strcmp((a),(b))!=0) { ++g_fails; \
    std::printf("  FAIL %s:%d: \"%s\" == \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while(0)

#define RUN(fn) do { std::printf("-- %s\n", #fn); fn(); } while(0)

#define REPORT() do { std::printf("\n%d checks, %d failures\n", g_checks, g_fails); \
    return g_fails ? 1 : 0; } while(0)
```

- [ ] **Step 4: Write `tests/fake_mem.h`**

```cpp
#pragma once
#include <map>
#include <string>
#include "sector_reader.h"

// A fake 32-bit address space: words at addresses, plus C-strings at addresses.
// Unset words read as "unreadable" (u32 returns false) — simulating bad pointers.
struct FakeMem {
    std::map<uint32_t, uint32_t> words;
    std::map<uint32_t, std::string> strings;

    void w(uint32_t addr, uint32_t val)            { words[addr] = val; }
    void s(uint32_t addr, const std::string& str)  { strings[addr] = str; }

    x3::Mem mem() {
        x3::Mem m;
        auto* W = &words;
        auto* S = &strings;
        m.u32 = [W](uint32_t a, uint32_t& o) -> bool {
            auto it = W->find(a);
            if (it == W->end()) return false;
            o = it->second; return true;
        };
        m.cstr = [S](uint32_t a, size_t) -> std::string {
            auto it = S->find(a);
            return it == S->end() ? std::string() : it->second;
        };
        return m;
    }
};
```

- [ ] **Step 5: Write `tests/test_sector_reader.cpp` (first test only)**

```cpp
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
```

- [ ] **Step 6: Add the `test` target to `Makefile`**

Add `test` to the `.PHONY` line (currently `\.PHONY: all clean inspect`) so it reads:

```make
.PHONY: all clean inspect test
```

Then append this target to the end of the file:

```make
# Host unit tests for the pure walk logic (no SDL/GL/ImGui). Runs in or out of
# the nix-shell — sector_reader is arch-independent (fixed-width types only).
test:
	@mkdir -p $(BUILD)
	g++ -std=c++17 -I src -O0 -g \
		src/sector_reader.cpp tests/test_sector_reader.cpp \
		-o $(BUILD)/test_sector_reader
	$(BUILD)/test_sector_reader
```

- [ ] **Step 7: Run the test — verify it FAILS to link**

Run: `make test`
Expected: compile/link error — `undefined reference to x3::read_current_sector(...)` (the function is declared but not defined yet).

- [ ] **Step 8: Write the stub `src/sector_reader.cpp`**

```cpp
#include "sector_reader.h"
#include "struct_offsets.h"

namespace x3 {

std::vector<ObjInfo> read_sector_objects(const Mem&, uint32_t, size_t) {
    return {};
}

SectorView read_current_sector(const Mem&, uint32_t, size_t) {
    return SectorView{}; // in_sector = false
}

const char* type_label(int16_t) { return "?"; }

} // namespace x3
```

- [ ] **Step 9: Run the test — verify it PASSES**

Run: `make test`
Expected: `1 checks, 0 failures` and exit 0.

- [ ] **Step 10: Commit**

```bash
git add src/struct_offsets.h src/sector_reader.h src/sector_reader.cpp \
        tests/check.h tests/fake_mem.h tests/test_sector_reader.cpp Makefile
git commit -m "feat(map): scaffold sector_reader types, test harness, stub"
```

---

## Task 2: Happy-path sector walk

Builds a synthetic sector with two objects in the fake address space and implements the real walk to read them.

**Files:**
- Modify: `tests/test_sector_reader.cpp`
- Modify: `src/sector_reader.cpp`

- [ ] **Step 1: Add the happy-path test**

Add this function above `main()` and add `RUN(test_two_objects);` inside `main()` (before `REPORT()`):

```cpp
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

    x3::SectorView v = x3::read_current_sector(fm.mem());
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
}
```

- [ ] **Step 2: Run the test — verify it FAILS**

Run: `make test`
Expected: failures on `test_two_objects` (the stub returns `in_sector=false` and zero objects), e.g. `FAIL ... v.in_sector`.

- [ ] **Step 3: Implement the real walk in `src/sector_reader.cpp`**

Replace the entire file with:

```cpp
#include "sector_reader.h"
#include "struct_offsets.h"

namespace x3 {

static bool ru32(const Mem& m, uint32_t addr, uint32_t& out) {
    return m.u32 && m.u32(addr, out);
}

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
```

- [ ] **Step 4: Run the test — verify it PASSES**

Run: `make test`
Expected: `test_null_scene` + `test_two_objects` pass; `… 0 failures`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add src/sector_reader.cpp tests/test_sector_reader.cpp
git commit -m "feat(map): implement sector object walk (happy path)"
```

---

## Task 3: Robustness — ship-anchor fallback, cap, null guards

Adds one test per edge case. The implementation from Task 2 already handles these; each test confirms a specific guarantee. If a test fails, fix the implementation, not the test.

**Files:**
- Modify: `tests/test_sector_reader.cpp`

- [ ] **Step 1: Add four edge-case tests**

Add these functions above `main()` and `RUN(...)` lines for each inside `main()`:

```cpp
// scene+0x38 points at the player SHIP (type 5); sector is its parent (+0x54).
static void test_ship_anchor_resolves_via_parent() {
    FakeMem fm;
    fm.w(0x0837a0d0, 0x1000);
    fm.w(0x1000 + 0x38, 0x2000);   // anchor = ship
    fm.w(0x2000 + 0x48, 5);        // type 5 = ship
    fm.w(0x2000 + 0x54, 0x2500);   // parent -> sector
    fm.w(0x2500 + 0x48, 7);        // parent type 7 = sector
    fm.w(0x2500 + 0x50, 0x3000);   // sector payload (empty list)
    fm.w(0x3000 + 0x00, 0x3004);   // head == sentinel -> 0 objects

    x3::SectorView v = x3::read_current_sector(fm.mem());
    CHECK(v.in_sector);
    CHECK_EQ(v.sector_addr, 0x2500u);
    CHECK(v.objects.empty());
}

// A cyclic list (no sentinel reached) must terminate at `cap`, not hang.
static void test_cycle_is_capped() {
    FakeMem fm;
    fm.w(0x0837a0d0, 0x1000);
    fm.w(0x1000 + 0x38, 0x2000);
    fm.w(0x2000 + 0x48, 7);        // sector
    fm.w(0x2000 + 0x50, 0x3000);   // payload
    fm.w(0x3000 + 0x00, 0x4000);   // head -> node
    fm.w(0x4000 + 0x00, 0x4000);   // node.next -> itself (cycle)
    fm.w(0x4000 + 0x48, 5);

    auto objs = x3::read_sector_objects(fm.mem(), 0x2000, /*cap=*/3);
    CHECK_EQ(objs.size(), (size_t)3);   // stopped at cap, did not hang
}

// Null name pointer -> empty name (no crash).
static void test_null_name() {
    FakeMem fm;
    fm.w(0x0837a0d0, 0x1000);
    fm.w(0x1000 + 0x38, 0x2000);
    fm.w(0x2000 + 0x48, 7);
    fm.w(0x2000 + 0x50, 0x3000);
    fm.w(0x3000 + 0x00, 0x4000);
    fm.w(0x4000 + 0x00, 0x3004);   // next -> sentinel
    fm.w(0x4000 + 0x08, 7);
    fm.w(0x4000 + 0x48, 5);
    fm.w(0x4000 + 0x0c, 0);        // name ptr = null

    auto v = x3::read_current_sector(fm.mem());
    CHECK_EQ(v.objects.size(), (size_t)1);
    CHECK(v.objects[0].name.empty());
}

// Null transform pointer -> position stays 0 (no crash).
static void test_null_transform() {
    FakeMem fm;
    fm.w(0x0837a0d0, 0x1000);
    fm.w(0x1000 + 0x38, 0x2000);
    fm.w(0x2000 + 0x48, 7);
    fm.w(0x2000 + 0x50, 0x3000);
    fm.w(0x3000 + 0x00, 0x4000);
    fm.w(0x4000 + 0x00, 0x3004);
    fm.w(0x4000 + 0x48, 5);
    fm.w(0x4000 + 0x6c, 0);        // transform = null

    auto v = x3::read_current_sector(fm.mem());
    CHECK_EQ(v.objects.size(), (size_t)1);
    CHECK_EQ(v.objects[0].x, 0);
    CHECK_EQ(v.objects[0].y, 0);
    CHECK_EQ(v.objects[0].z, 0);
}
```

And inside `main()`:

```cpp
    RUN(test_ship_anchor_resolves_via_parent);
    RUN(test_cycle_is_capped);
    RUN(test_null_name);
    RUN(test_null_transform);
```

- [ ] **Step 2: Run the tests — verify they PASS**

Run: `make test`
Expected: all six tests pass, `0 failures`, exit 0. (If `test_cycle_is_capped` hangs, the `n < cap` guard is missing — re-check Task 2 Step 3.)

- [ ] **Step 3: Commit**

```bash
git add tests/test_sector_reader.cpp
git commit -m "test(map): cover ship-anchor fallback, cap, null name/transform"
```

---

## Task 4: `type_label` test

A tiny pure-function test to pin the labels the panel uses.

**Files:**
- Modify: `tests/test_sector_reader.cpp`

- [ ] **Step 1: Add the test**

Add above `main()` and a `RUN(test_type_label);` inside `main()`:

```cpp
static void test_type_label() {
    CHECK_STR(x3::type_label(5), "Ship");
    CHECK_STR(x3::type_label(6), "Station");
    CHECK_STR(x3::type_label(7), "Sector");
    CHECK_STR(x3::type_label(99), "?");
}
```

- [ ] **Step 2: Run — verify it PASSES**

Run: `make test`
Expected: all tests pass (the `type_label` impl from Task 2 already returns these), `0 failures`.

- [ ] **Step 3: Commit**

```bash
git add tests/test_sector_reader.cpp
git commit -m "test(map): pin type_label strings"
```

---

## Task 5: Wire into the overlay + live validation

Adds a production `Mem` (backed by `/proc/self/mem`) and the `map_panel()` ImGui window, then builds the real `.so` and validates against the running game.

**Files:**
- Modify: `Makefile` (add `src/sector_reader.cpp` to `SRC`)
- Modify: `src/overlay.cpp` (include, engine `Mem`, `map_panel()`, call site)

- [ ] **Step 1: Add `sector_reader.cpp` to the build**

In `Makefile`, change the `SRC` line from:

```make
SRC      := src/overlay.cpp $(IMGUI_SRC)
```

to:

```make
SRC      := src/overlay.cpp src/sector_reader.cpp $(IMGUI_SRC)
```

- [ ] **Step 2: Include the header in `src/overlay.cpp`**

After the existing ImGui includes (the block ending with `#include "backends/imgui_impl_opengl3.h"`, around line 40), add:

```cpp
#include "sector_reader.h"
```

- [ ] **Step 3: Add the engine `Mem` and `map_panel()`**

Insert this block immediately **above** `static void overlay_draw(void)` (around line 648):

```cpp
// Production Mem: read the game's own address space via /proc/self/mem (the same
// safe pread path the scanners use — bad addresses return false, never crash).
static x3::Mem make_engine_mem(void)
{
    x3::Mem m;
    m.u32 = [](uint32_t a, uint32_t& o) -> bool {
        int32_t v;
        if (!mem_read_i32((uintptr_t)a, &v)) return false;
        o = (uint32_t)v;
        return true;
    };
    m.cstr = [](uint32_t addr, size_t maxlen) -> std::string {
        std::string s;
        if (!addr) return s;
        for (size_t i = 0; i < maxlen; i += 4) {
            int32_t w = 0;
            if (!mem_read_i32((uintptr_t)(addr + i), &w)) break;
            const char* b = (const char*)&w;
            for (int k = 0; k < 4 && (i + (size_t)k) < maxlen; ++k) {
                if (b[k] == '\0') return s;
                s.push_back(b[k]);
            }
        }
        return s;
    };
    return m;
}

// Read-only panel: list the objects in the player's current sector.
static void map_panel(void)
{
    if (!ImGui::Begin("x3ui - sector map")) { ImGui::End(); return; }
    mem_open();

    // Throttle: re-walk at most ~6x/sec (cheap, but no need per-frame).
    static x3::SectorView view;
    static double last = -1.0;
    double now = ImGui::GetTime();
    if (last < 0.0 || (now - last) > 0.15) {
        view = x3::read_current_sector(make_engine_mem());
        last = now;
    }

    if (!view.in_sector) {
        ImGui::TextDisabled("not in a sector (main menu / loading?)");
        ImGui::End();
        return;
    }

    ImGui::Text("sector: %s  @ 0x%08x   (%zu objects)",
                view.sector_name.empty() ? "(unnamed)" : view.sector_name.c_str(),
                view.sector_addr, view.objects.size());

    static int filter = 0; // 0 = all, else a type id
    ImGui::RadioButton("all", &filter, 0);      ImGui::SameLine();
    ImGui::RadioButton("ships", &filter, 5);    ImGui::SameLine();
    ImGui::RadioButton("stations", &filter, 6);

    if (ImGui::BeginTable("objs", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_RowBg, ImVec2(0, 320))) {
        ImGui::TableSetupColumn("id");
        ImGui::TableSetupColumn("type");
        ImGui::TableSetupColumn("name");
        ImGui::TableSetupColumn("pos (int)");
        ImGui::TableSetupColumn("pos (float)");
        ImGui::TableHeadersRow();
        for (const auto& o : view.objects) {
            if (filter != 0 && o.type != filter) continue;
            float fx, fy, fz;
            memcpy(&fx, &o.x, 4); memcpy(&fy, &o.y, 4); memcpy(&fz, &o.z, 4);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%u", o.id);
            ImGui::TableNextColumn(); ImGui::Text("%s(%d)", x3::type_label(o.type), o.type);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(o.name.empty() ? "(unnamed)" : o.name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d,%d,%d", o.x, o.y, o.z);
            ImGui::TableNextColumn(); ImGui::Text("%.0f,%.0f,%.0f", fx, fy, fz);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
```

- [ ] **Step 4: Call `map_panel()` in the draw path**

In `overlay_draw()`, find the lines (around 696–697):

```cpp
    state_bridge_window();
    ship_window();
```

and add a third call:

```cpp
    state_bridge_window();
    ship_window();
    map_panel();
```

- [ ] **Step 5: Run unit tests again (regression check)**

Run: `make test`
Expected: all tests still pass (`0 failures`). Logic is unchanged; this confirms nothing broke.

- [ ] **Step 6: Build the overlay `.so`**

Run: `nix-shell --run 'make'`
Expected: ends with `=== built libx3ui.so ===` and the `inspect` output (bare sonames, GLOBAL DEFAULT `SDL_GL_SwapWindow`). No compile errors.

- [ ] **Step 7: Live validation (crawl → walk → run)**

Launch the game via the existing wrapper (Steam launch option `…/x3run.sh %command%`, or run `x3engine.sh` in-container as set up). Load a saved game, press **ScrollLock** to show the overlay, and open **"x3ui - sector map"**. Verify in order:

1. **Crawl:** the header shows `sector: <name> @ 0x… (N objects)` matching the sector you are in. If it says "not in a sector" while in-game, the `scene+0x38` anchor or its type check is off — open the existing **"x3ui - ship"** window, note the type word at `+0x48` of that object, and confirm it is 5/6/7; adjust `SCENE_PLAYER_ANCHOR` handling if the anchor is neither a sector nor a ship/station.
2. **Walk:** the table lists objects whose **names** match what you see on the in-game sector map / gate list. Count is in the right ballpark.
3. **Run:** fly around — objects appear/disappear and the list updates within ~150 ms. Use the **ships/stations** filter.
4. **Position format:** compare the `pos (int)` vs `pos (float)` columns against in-game distances to confirm which interpretation is real, and note it. (No code change required for v1; this informs a later units pass.)

Capture a screenshot or note the observed sector name + a few object names as evidence the panel is correct.

- [ ] **Step 8: Commit**

```bash
git add Makefile src/overlay.cpp
git commit -m "feat(map): add live sector map panel to overlay"
```

---

## Validation summary

- **Automated:** `make test` — six+ unit tests cover the walk (happy path), ship-anchor fallback, cycle cap, null name, null transform, and `type_label`. These prove the *logic* against a known fake graph.
- **Manual (Task 5 Step 7):** proves the *offsets* against the live engine — sector name, object names, live updates. This is the part the unit tests cannot cover because it depends on the real struct layout.

## Deferred (not in this plan — tracked from the spec §7)

- **Owner/race column + relation tint** — field not yet located; needs a follow-up trace of the `GS_SetSectorOwner` opcode handler.
- **Richer type labels** — map more `+0x48` ids (gates, asteroids, nav beacons) using the type switch in `FUN_080f0750`.
- **Position units** — convert raw words to km once the format/scale is confirmed live.
- **Galaxy-wide listing** — the global object list at `scene+0x08` (sentinel `scene+0x0c`) is available as a later enhancement.
- **Actions (Phase 2)** — issuing commands from the panel via the `run_script` bridge with argument marshalling.
