# Target Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Clicking an object in the overlay's sector table or 3D map makes the game itself target that object (HUD brackets follow), and the overlay highlights whatever the game currently targets.

**Architecture:** A pure, testable gravidar-object lookup in `sector_reader` (factored out of the existing scanner-range walk) feeds two consumers: `read_current_sector` reads the live target back into `SectorView`, and the overlay's `select_target` calls the engine setter `gravidar_set_target@0x08131d10` (cdecl, in-process) to push the selection into the game. No raw memory writes, no writable fd.

**Tech Stack:** C++17, 32-bit non-PIE target (nix-shell), ImGui overlay, `/proc/self/mem` reads, in-process engine-function call.

---

## Verified foundation (do not re-derive)

- Setter `void __cdecl gravidar_set_target(void* gravidarObj, int16_t mode, void* targetObjPtr)`
  @`0x08131d10` — **cdecl** (gravidar is first stack arg), `targetObjPtr` is a real object
  **pointer**, mode 3 = lock+brackets, mode 0 = clear. (memory `x3ui-player-target`)
- Gravidar/HUD object: target ptr at `+0x1bc`, mode at `+0x1c0`. It is the same object
  `resolve_range_ctx` already finds in the `g_hud_mgr`@`0x0837a100` hashmap (entry whose
  `+0x10` == player ship).
- Overlay code runs on the game's main thread inside the `SDL_GL_SwapWindow` hook, so calling
  the engine function is thread-safe.

## File structure

- `src/struct_offsets.h` — add target offsets + setter address (single source of truth).
- `src/sector_reader.h` / `.cpp` — `find_player_gravidar` (new, exported); `RangeCtx.gravidar_addr`;
  `SectorView.current_target_addr`; read-back in `read_current_sector`; `resolve_range_ctx`
  reuses the factored walk.
- `src/overlay.cpp` — `select_target` body (call the setter); game-target reticle in `map_panel`
  (table) and `map3d_panel` (3D).
- `tests/test_sector_reader.cpp` — unit tests for the lookup + read-back.

---

### Task 1: Engine offsets/address in struct_offsets.h

**Files:**
- Modify: `src/struct_offsets.h` (after the `HUD / gravidar` block, ~line 85)

- [ ] **Step 1: Add the constants**

In `src/struct_offsets.h`, immediately after the line
`constexpr uint32_t FLAG_KNOWN      = 0x8;` (end of the HUD/gravidar block) and before the
closing `}}`, add:

```cpp
// Player current target (HUD targeting brackets) — fields on the gravidar/HUD object,
// plus the engine setter. See memory x3ui-player-target.
constexpr uint32_t GRAVIDAR_TARGET        = 0x1bc;        // *: current target OBJECT POINTER
constexpr uint32_t GRAVIDAR_TARGET_MODE   = 0x1c0;        // i16: 0 clear, 3 lock+brackets
constexpr uint32_t GRAVIDAR_SET_TARGET_FN = 0x08131d10u;  // void __cdecl(gravidar, i16 mode, void* targetObj)
constexpr int16_t  TARGET_MODE_LOCK       = 3;            // lock + brackets (+ NotifyTargetLock)
constexpr int16_t  TARGET_MODE_CLEAR      = 0;            // clear target
```

- [ ] **Step 2: Verify it still builds**

Run: `make test`
Expected: builds and all existing tests PASS (constants are unused so far — just confirms no typo).

- [ ] **Step 3: Commit**

```bash
git add src/struct_offsets.h
git commit -m "feat(target-bridge): add gravidar target offsets + setter address"
```

---

### Task 2: `find_player_gravidar` (factor out the hashmap walk)

**Files:**
- Modify: `src/sector_reader.h` (declaration; near the other free-function decls ~line 73)
- Modify: `src/sector_reader.cpp` (new function + refactor `resolve_range_ctx`)
- Test: `tests/test_sector_reader.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/test_sector_reader.cpp`, add this function before `int main()`:

```cpp
// The gravidar object = the g_hud_mgr hashmap entry whose scanner source == player ship.
static void test_find_player_gravidar() {
    FakeMem fm;
    uint32_t player = 0x8000;
    fm.w(0x0837a100, 0x9000);        // HUD mgr global -> hud
    fm.w(0x9000 + 0x00, 0x9100);     // hashmap = *(hud)
    fm.w(0x9100 + 0x00, 0x9200);     // buckets ptr
    fm.w(0x9100 + 0x04, 0);          // mask = 0 (single bucket)
    fm.w(0x9200 + 0x00, 0x9300);     // buckets[0] -> node
    fm.w(0x9300 + 0x08, 0x9400);     // node.value = gravidar
    fm.w(0x9400 + 0x10, player);     // gravidar source == player
    CHECK_EQ(x3::find_player_gravidar(fm.mem(), player), 0x9400u);
    CHECK_EQ(x3::find_player_gravidar(fm.mem(), 0xdeadu), 0u);  // wrong player -> not found
    FakeMem empty;
    CHECK_EQ(x3::find_player_gravidar(empty.mem(), player), 0u);  // no hud mgr -> 0
}
```

Register it: add `RUN(test_find_player_gravidar);` inside `main()` (e.g. right after
`RUN(test_scanner_range_filter);`).

- [ ] **Step 2: Run test to verify it fails**

Run: `make test`
Expected: COMPILE FAILURE — `find_player_gravidar` is not declared.

- [ ] **Step 3: Declare in the header**

In `src/sector_reader.h`, add before the `read_sector_objects` declaration (~line 71):

```cpp
// Address of the player's gravidar/HUD-state object (the g_hud_mgr hashmap entry whose
// scanner source == player_ship), or 0 if not resolvable. Pure reads via Mem. Used for the
// scanner range (+0x240) and the player's current target (+0x1bc).
uint32_t find_player_gravidar(const Mem& mem, uint32_t player_ship);
```

- [ ] **Step 4: Implement + refactor in the .cpp**

In `src/sector_reader.cpp`, add this function immediately *above* `resolve_range_ctx`
(~line 532, after the blank line before its comment):

```cpp
uint32_t find_player_gravidar(const Mem& mem, uint32_t player_ship) {
    if (!player_ship) return 0;
    uint32_t hud = 0, hmap = 0, buckets = 0, mask = 0;
    if (!ru32(mem, off::HUD_MGR_GLOBAL, hud) || !hud) return 0;
    if (!ru32(mem, hud, hmap) || !hmap) return 0;                       // hashmap = *(hud)
    if (!ru32(mem, hmap + off::HASH_BUCKETS, buckets) || !buckets) return 0;
    ru32(mem, hmap + off::HASH_MASK, mask);
    uint32_t lim = (mask < 1024) ? mask : 1024;
    for (uint32_t i = 0; i <= lim; ++i) {
        uint32_t node = 0;
        if (!ru32(mem, buckets + i * 4, node)) continue;
        for (int guard = 0; node && guard < 64; ++guard) {
            uint32_t grav = 0, src = 0;
            if (ru32(mem, node + off::HASH_NODE_VALUE, grav) && grav &&
                ru32(mem, grav + off::GRAVIDAR_SOURCE, src) && src == player_ship)
                return grav;                                            // the player's gravidar
            uint32_t nxt = 0;
            if (!ru32(mem, node + off::HASH_NODE_NEXT, nxt)) break;
            node = nxt;
        }
    }
    return 0;
}
```

Then, inside `resolve_range_ctx`, **replace** the inline walk — the block from
`uint32_t hud = 0, hmap = 0, buckets = 0, mask = 0;` down to and including
`if (!range) return rc;` (currently `sector_reader.cpp:552-575`) — with:

```cpp
    uint32_t grav = find_player_gravidar(mem, player);
    rc.gravidar_addr = grav;                       // set even if range is unavailable below
    uint32_t range = 0;
    if (grav) ru32(mem, grav + off::GRAVIDAR_RANGE, range);
    if (!range) return rc;
```

- [ ] **Step 5: Add the `gravidar_addr` field to RangeCtx**

In `src/sector_reader.h`, in `struct RangeCtx`, add after `uint32_t player_ship = 0;`:

```cpp
    uint32_t gravidar_addr = 0;       // player's gravidar/HUD object (target + range live here)
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `make test`
Expected: PASS — `test_find_player_gravidar` passes and `test_scanner_range_filter` still passes
(same walk, now factored; range filtering unchanged).

- [ ] **Step 7: Commit**

```bash
git add src/sector_reader.h src/sector_reader.cpp tests/test_sector_reader.cpp
git commit -m "feat(target-bridge): find_player_gravidar; factor out the hashmap walk"
```

---

### Task 3: Read the live game target back into SectorView

**Files:**
- Modify: `src/sector_reader.h` (`SectorView`)
- Modify: `src/sector_reader.cpp` (`read_current_sector`)
- Test: `tests/test_sector_reader.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/test_sector_reader.cpp`, add before `int main()`:

```cpp
// read_current_sector should surface the gravidar's current target ptr (+0x1bc).
static void test_current_target_readback() {
    FakeMem fm;
    fm.w(0x0837a0d0, 0x1000);            // scene
    fm.w(0x1000 + 0x08, 0x2000);         // sector
    fm.w(0x1000 + 0x38, 0x8000);         // player ship
    fm.w(0x2000 + 0x48, 1);              // sector type
    fm.w(0x2000 + 0x50, 0x3000);         // payload
    fm.w(0x3000 + 0x00, 0x3004);         // empty list (head -> sentinel)
    fm.w(0x8000 + 0x6c, 0x8100);         // player transform node
    fm.w(0x8100 + 0x2c, 0); fm.w(0x8100 + 0x30, 0); fm.w(0x8100 + 0x34, 0);  // player pos
    fm.w(0x0837a100, 0x9000);            // HUD mgr -> ... -> gravidar
    fm.w(0x9000 + 0x00, 0x9100);
    fm.w(0x9100 + 0x00, 0x9200);
    fm.w(0x9100 + 0x04, 0);
    fm.w(0x9200 + 0x00, 0x9300);
    fm.w(0x9300 + 0x08, 0x9400);
    fm.w(0x9400 + 0x10, 0x8000);         // source == player
    fm.w(0x9400 + 0x1bc, 0x4242);        // current target pointer
    auto v = x3::read_current_sector(fm.mem(), x3::off_SCENE_GLOBAL);
    CHECK(v.in_sector);
    CHECK_EQ(v.current_target_addr, 0x4242u);
}
```

Register it: add `RUN(test_current_target_readback);` in `main()`.

- [ ] **Step 2: Run test to verify it fails**

Run: `make test`
Expected: COMPILE FAILURE — `SectorView` has no member `current_target_addr`.

- [ ] **Step 3: Add the field**

In `src/sector_reader.h`, in `struct SectorView`, add after
`int64_t scanner_range = 0;`:

```cpp
    uint32_t current_target_addr = 0;   // game's live HUD target (gravidar+0x1bc); 0 = none
```

- [ ] **Step 4: Read it in `read_current_sector`**

In `src/sector_reader.cpp`, find (currently ~line 614):

```cpp
    RangeCtx rc = resolve_range_ctx(mem, scene);
    v.scanner_range = rc.valid ? rc.range : 0;
```

Add directly below those two lines:

```cpp
    if (rc.gravidar_addr)
        ru32(mem, rc.gravidar_addr + off::GRAVIDAR_TARGET, v.current_target_addr);
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `make test`
Expected: PASS — `test_current_target_readback` passes; all others still pass.

- [ ] **Step 6: Commit**

```bash
git add src/sector_reader.h src/sector_reader.cpp tests/test_sector_reader.cpp
git commit -m "feat(target-bridge): read live game target into SectorView.current_target_addr"
```

---

### Task 4: Push the overlay selection into the game (`select_target` body)

**Files:**
- Modify: `src/overlay.cpp` — `select_target` (~line 805)

No host test (in-process engine call); verified by launch.

- [ ] **Step 1: Implement the call**

In `src/overlay.cpp`, replace the entire `select_target` function (currently lines 805-811,
ending at the `TODO(target-bridge)` comment) with:

```cpp
static void select_target(const x3::ObjInfo& o)
{
    g_sel_id   = o.id;
    g_sel_addr = o.addr;
    // Push the selection into the game: resolve the player's gravidar/HUD object and call
    // the engine target setter (mode 3 = lock + brackets). Calling the function (vs.
    // raw-writing +0x1bc/+0x1c0) preserves full HUD consistency — lock timestamp, SFX, UI
    // bits, and the NotifyTargetLock script callback. cdecl is the default on 32-bit x86;
    // the attribute makes it explicit. o.addr is a live object from this frame's sector walk.
    if (!o.addr) return;
    mem_open();
    x3::Mem m = make_engine_mem();
    uint32_t scene = 0, player = 0;
    if (m.u32(x3::off::SCENE_GLOBAL, scene) && scene &&
        m.u32(scene + x3::off::SCENE_PLAYER_ANCHOR, player) && player) {
        uint32_t grav = x3::find_player_gravidar(m, player);
        if (grav) {
            typedef void (__attribute__((cdecl)) *set_target_t)(void*, int16_t, void*);
            ((set_target_t)(uintptr_t)x3::off::GRAVIDAR_SET_TARGET_FN)(
                (void*)(uintptr_t)grav, x3::off::TARGET_MODE_LOCK, (void*)(uintptr_t)o.addr);
        }
    }
}
```

- [ ] **Step 2: Build (32-bit, in nix-shell)**

Run: `nix-shell --run 'make'`
Expected: compiles to `libx3ui.so` with no errors.

- [ ] **Step 3: Live-verify the target push**

Run: `pkill -x X3AP_main; ./x3launch.sh`
Then (Claude drives the launch): in flight, open `x3ui - sector map`, click a ship/station row.
Expected: the in-flight HUD targeting brackets jump to the clicked object; clicking another row
moves them; no crash. Try a station, a ship, and an asteroid.

- [ ] **Step 4: Commit**

```bash
git add src/overlay.cpp
git commit -m "feat(target-bridge): select_target calls gravidar_set_target to set game target"
```

---

### Task 5: Highlight the game's live target in the table and 3D map

**Files:**
- Modify: `src/overlay.cpp` — `map_panel` table loop (~line 906) and `map3d_panel` blip loop (~line 1545)

No host test (visual); verified by launch.

- [ ] **Step 1: Table reticle (amber row outline)**

In `src/overlay.cpp`, in the table row loop, find (~line 906):

```cpp
            ImGui::PopStyleColor();
```

(the one right after the `Selectable(... ) select_target(o);` block). Add directly below it:

```cpp
            if (view.current_target_addr && o.addr == view.current_target_addr) {  // live game target
                ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRect(ra, rb, IM_COL32(255,170,40,220), 0.0f, 0, 1.5f);
            }
```

- [ ] **Step 2: 3D map reticle (amber double ring)**

In `src/overlay.cpp`, in `map3d_panel`'s per-object draw loop, find the end of the four-corner
selection-box block (the lines guarded by `if (o.id == g_sel_id)`, ~line 1535-1560). Immediately
**after** that `if (o.id == g_sel_id) { ... }` block closes, add:

```cpp
        if (view.current_target_addr && o.addr == view.current_target_addr) {  // live game target reticle
            float tr = (e.hull ? std::max(e.rpx, 10.0f) : 9.0f) + 7.0f;
            ImU32 tc = IM_COL32(255,170,40,230);
            dl->AddCircle(e.s, tr, tc, 28, 1.8f);
            dl->AddCircle(e.s, tr * 0.6f, tc, 24, 1.2f);
        }
```

- [ ] **Step 3: Build**

Run: `nix-shell --run 'make'`
Expected: compiles cleanly.

- [ ] **Step 4: Live-verify two-way sync**

Run: `pkill -x X3AP_main; ./x3launch.sh`
Then: open both panels. Click an object → it gets the amber marker in the table AND a reticle in
the 3D map (coinciding with the white selection box right after a click). Then press the game's
own target key (T) to target something else → the amber marker/reticle follows the game's target,
while the white selection box (mesh viewer) stays on the clicked object.

- [ ] **Step 5: Commit**

```bash
git add src/overlay.cpp
git commit -m "feat(target-bridge): highlight the game's live target in table + 3D map"
```

---

### Task 6: Update docs status

**Files:**
- Modify: `docs/README.md` (status cell), `docs/superpowers/specs/2026-06-27-target-bridge-design.md` (status line)

- [ ] **Step 1: Flip statuses to done**

In `docs/README.md`, change the Target-bridge row's Plan cell from `_(pending)_` to
`` `superpowers/plans/2026-06-27-target-bridge.md` `` and Status from `design` to
`` done (`feat/sector-map`) ``. In the spec file, change `Status: design, awaiting review.` to
`Status: built. Branch: feat/sector-map.`

- [ ] **Step 2: Commit**

```bash
git add docs/README.md docs/superpowers/specs/2026-06-27-target-bridge-design.md
git commit -m "docs(target-bridge): mark built; link plan"
```

---

## Notes for the implementer

- **Run order matters for the live tests:** a rebuilt `.so` only loads on a fresh game start —
  always `pkill -x X3AP_main` before `./x3launch.sh`. Claude runs the launch itself.
- **If brackets appear but don't hold** (get stomped within a frame): mode 3 should be sticky; if
  not, the fallback is to re-assert the target each frame from the panel — but try the single-call
  version first (spec decision).
- **If the call crashes:** confirm via the debug socket that the resolved `grav` address reads a
  plausible gravidar (`grav+0x10` == player ship) before blaming the call; a bad `o.addr` (freed
  object) is the other suspect — it's read live from this frame's walk, so unlikely.
- The mesh-preview panel keeps following `g_sel_addr` (click selection), intentionally independent
  of the game target.
