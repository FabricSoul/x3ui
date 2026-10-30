// x3ui — LD_PRELOAD overlay for X3: Terran Conflict / Albion Prelude
// (32-bit i386 engine, Steam Runtime / pressure-vessel, OpenGL).
//
// Strategy (Route B, non-invasive): we never modify the game on disk. We
// interpose two SDL2 entry points the engine already calls (confirmed via
// `nm -D X3AP_main`):
//
//   * SDL_GL_SwapWindow  — once per frame, right before the buffer flip. We
//                          draw a Dear ImGui frame on top, then call the real swap.
//   * SDL_PeepEvents     — the engine's event pump. We feed events to ImGui and,
//                          when the UI has focus, swallow them so they don't leak
//                          to the game. ScrollLock toggles the overlay.
//
// Because we are LD_PRELOAD'd, our SDL_GL_SwapWindow / SDL_PeepEvents are found
// before libSDL2's; we reach the originals via dlsym(RTLD_NEXT, ...). We use the
// ImGui OpenGL3 backend (own shaders + VAO/VBO, full GL state save/restore) so we
// coexist cleanly with X3's GLSL render state.

#define _FILE_OFFSET_BITS 64   // 64-bit off_t so pread reaches >2GB addresses

#include <dlfcn.h>
#include <errno.h>      // program_invocation_short_name (with _GNU_SOURCE)
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>   // GL enums + PFNGL* types (entrypoints loaded at runtime)

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"
#include "sector_reader.h"
#include "debug_server.h"

#define X3UI_EXPORT extern "C" __attribute__((visibility("default")))

// --- compat shims for X3's old bundled libSDL2 ----------------------------
// X3 ships an SDL2 older than the ImGui SDL2 backend expects, so two symbols it
// references are missing and would break load-time relocation. Neither matters
// to us (OpenGL3 backend, never open URLs). WEAK stubs: a real SDL definition
// wins if present; otherwise these satisfy the loader.
extern "C" {
__attribute__((weak)) int SDL_OpenURL(const char *url)
{
    (void)url;
    return -1;
}
__attribute__((weak)) void SDL_Vulkan_GetDrawableSize(SDL_Window *w, int *width, int *height)
{
    (void)w;
    if (width)  *width  = 0;
    if (height) *height = 0;
}
}

// ---- originals, resolved lazily ------------------------------------------
typedef void (*pfn_SwapWindow)(SDL_Window *);
typedef int  (*pfn_PeepEvents)(SDL_Event *, int, SDL_eventaction, Uint32, Uint32);
typedef void (*pfn_PumpEvents)(void);

static pfn_SwapWindow real_SwapWindow = nullptr;
static pfn_PeepEvents real_PeepEvents = nullptr;
static pfn_PumpEvents real_PumpEvents = nullptr;

// Color-clear tracking. The game clears the color buffer 1x/frame in flight, but
// in the docked/menu render path `W_RectFill` is a dStub (no-op) so the buffer is
// never cleared -> our translucent panels accumulate ("old frame not cleaned up").
// The overlay never calls glClear itself, so this interpose observes ONLY the
// game's clears; overlay_draw uses g_force_opaque to draw solid in no-clear frames.
typedef void (*pfn_Clear)(GLbitfield);
static pfn_Clear real_Clear = nullptr;
static volatile bool g_color_cleared = false;  // set by our glClear interpose
static bool g_force_opaque = false;            // read by the panels this frame

static void resolve_originals(void)
{
    if (!real_SwapWindow)
        real_SwapWindow = (pfn_SwapWindow)dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");
    if (!real_PeepEvents)
        real_PeepEvents = (pfn_PeepEvents)dlsym(RTLD_NEXT, "SDL_PeepEvents");
    if (!real_PumpEvents)
        real_PumpEvents = (pfn_PumpEvents)dlsym(RTLD_NEXT, "SDL_PumpEvents");
    if (!real_Clear)
        real_Clear = (pfn_Clear)dlsym(RTLD_NEXT, "glClear");
}

// Process gate: the .so is inherited by every process in X3's launch chain, but
// we only activate inside the engine (X3TC_main / X3AP_main, i.e. names ending
// in "_main"). Everything else passes through untouched. Cached + logged once.
static bool is_game_process(void)
{
    static int cached = -1;   // -1 unknown, 0 no, 1 yes
    if (cached != -1)
        return cached == 1;

    const char *name = program_invocation_short_name;
    size_t len = name ? strlen(name) : 0;
    bool is_main = len >= 5 && strcmp(name + len - 5, "_main") == 0;

    cached = is_main ? 1 : 0;
    fprintf(stderr, "[x3ui] process '%s' -> %s\n",
            name ? name : "(null)", is_main ? "ACTIVE (engine)" : "dormant");
    return is_main;
}

// ---- overlay state --------------------------------------------------------
static bool g_inited     = false;
static bool g_ui_visible = true;   // toggled with ScrollLock
static SDL_Window  *g_sdl_window = nullptr;   // captured for synthesizing input events (auto-dock)
static SDL_Event    g_synth[16];              // synthetic events injected into the game's PeepEvents read
static volatile int g_synth_n = 0;            // count queued by the synthkey harness
// The game polls SDL_GetKeyboardState (not events) for keys, so we force a key into SDL's
// STATE array for a few frames inside the SDL_PumpEvents hook (after SDL refreshes state).
static volatile int g_synth_hold  = 0;        // frames remaining to hold the synthetic key
static volatile int g_synth_sc    = 0;        // scancode to force down (e.g. SDL_SCANCODE_D)
static volatile int g_synth_shift = 0;        // also force LSHIFT (for Shift+D = Dock At)

static void overlay_init(SDL_Window *window)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    // Persist window layout to OUR dir (writable, on the LD_PRELOAD path) rather
    // than the game's cwd. Re-enables ImGui's auto save/load of imgui.ini.
    io.IniFilename = "/home/fabric/x3ui/imgui.ini";
    io.LogFilename = nullptr;
    // Never let the SDL backend force the OS/X11 cursor on; we manage cursor
    // visibility ourselves below (ImGui's own cursor only while hovering the
    // overlay; over the game world the game keeps its own cursor).
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.MouseDrawCursor = false;
    ImGui::StyleColorsDark();
    {   // Holographic HUD theme: translucent blue/cyan tinted glass with glowing edges —
        // this is a game-mod overlay, not a dev tool, so panels stay see-through.
        ImGuiStyle& st = ImGui::GetStyle();
        st.WindowRounding = 4.0f; st.FrameRounding = 3.0f;
        st.WindowBorderSize = 1.0f; st.FrameBorderSize = 1.0f;
        auto C = [&](ImGuiCol c, float r, float g, float b, float a){ st.Colors[c] = ImVec4(r,g,b,a); };
        C(ImGuiCol_WindowBg,       0.03f,0.07f,0.10f, 0.40f);
        C(ImGuiCol_ChildBg,        0.04f,0.09f,0.13f, 0.28f);
        C(ImGuiCol_TitleBg,        0.04f,0.12f,0.16f, 0.50f);
        C(ImGuiCol_TitleBgActive,  0.06f,0.22f,0.30f, 0.66f);
        C(ImGuiCol_Border,         0.35f,0.80f,0.95f, 0.45f);
        C(ImGuiCol_FrameBg,        0.10f,0.30f,0.40f, 0.28f);
        C(ImGuiCol_FrameBgHovered, 0.15f,0.45f,0.60f, 0.45f);
        C(ImGuiCol_FrameBgActive,  0.20f,0.55f,0.70f, 0.55f);
        C(ImGuiCol_Header,         0.15f,0.45f,0.60f, 0.35f);
        C(ImGuiCol_HeaderHovered,  0.20f,0.55f,0.70f, 0.50f);
        C(ImGuiCol_Button,         0.12f,0.38f,0.50f, 0.38f);
        C(ImGuiCol_ButtonHovered,  0.18f,0.52f,0.68f, 0.55f);
        C(ImGuiCol_ButtonActive,   0.25f,0.65f,0.82f, 0.70f);
        C(ImGuiCol_SliderGrab,     0.40f,0.85f,1.00f, 0.80f);
        C(ImGuiCol_Text,           0.82f,0.95f,1.00f, 1.00f);   // content stays bright/legible
        C(ImGuiCol_Separator,      0.35f,0.80f,0.95f, 0.30f);
    }

    SDL_GLContext ctx = SDL_GL_GetCurrentContext();
    ImGui_ImplSDL2_InitForOpenGL(window, ctx);
    ImGui_ImplOpenGL3_Init("#version 130");

    g_sdl_window = window;
    g_inited = true;
    fprintf(stderr, "[x3ui] overlay live (GL3 backend) — ScrollLock toggles\n");
}

// ===========================================================================
// State bridge — read live game state directly from the engine's own memory.
// We are in-process, so no IPC/scripts: we just read /proc/self/mem. Step 1 is
// an interactive i32 scanner (Cheat-Engine style) to LOCATE values like player
// credits; once pinned down we promote the address into a clean panel.
// ===========================================================================

struct Candidate { uintptr_t addr; int32_t val; };

static int                    g_memfd  = -1;
static std::vector<Candidate> g_cands;
static int                    g_scan_value = 0;
static uintptr_t              g_watch  = 0;
static bool                   g_scan_capped = false;

static void mem_open(void)
{
    if (g_memfd < 0)
        g_memfd = open("/proc/self/mem", O_RDONLY);
}

static bool mem_read_i32(uintptr_t addr, int32_t *out)
{
    return pread(g_memfd, out, sizeof(*out), (off_t)addr) == (ssize_t)sizeof(*out);
}

// Write path — separate O_RDWR handle on our own /proc/self/mem (we're in-process). pwrite
// to a bad address returns <0 instead of crashing, same safety model as the pread reads.
// Used by the command bridge (ship_set_command) to poke the AI command struct.
static int g_memfd_rw = -1;
static void mem_open_rw(void)
{
    if (g_memfd_rw < 0) g_memfd_rw = open("/proc/self/mem", O_RDWR);
}
static bool mem_write_u32(uintptr_t addr, uint32_t v)
{
    mem_open_rw();
    return g_memfd_rw >= 0 && pwrite(g_memfd_rw, &v, 4, (off_t)addr) == 4;
}
static bool mem_write_u16(uintptr_t addr, uint16_t v)
{
    mem_open_rw();
    return g_memfd_rw >= 0 && pwrite(g_memfd_rw, &v, 2, (off_t)addr) == 2;
}
static bool mem_write_u8(uintptr_t addr, uint8_t v)
{
    mem_open_rw();
    return g_memfd_rw >= 0 && pwrite(g_memfd_rw, &v, 1, (off_t)addr) == 1;
}
static bool mem_write_bytes(uintptr_t addr, const void *src, size_t n)
{
    mem_open_rw();
    if (g_memfd_rw < 0) return false;
    size_t done = 0;
    while (done < n) {
        ssize_t r = pwrite(g_memfd_rw, (const char *)src + done, n - done, (off_t)(addr + done));
        if (r <= 0) return false;
        done += (size_t)r;
    }
    return true;
}

// Bulk read: one (looped) pread for the whole range — orders of magnitude fewer
// syscalls than per-int reads when pulling big vertex/triangle arrays.
static bool mem_read_bytes(uintptr_t addr, void *dst, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = pread(g_memfd, (char *)dst + done, n - done, (off_t)(addr + done));
        if (r <= 0) return false;
        done += (size_t)r;
    }
    return true;
}

// Writable, anonymous-ish regions where game state lives (skip stack/vdso).
static void mem_regions(std::vector<std::pair<uintptr_t, uintptr_t>> &regs)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e;
        char perms[8] = {0}, path[256] = {0};
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %255[^\n]", &s, &e, perms, path) < 3)
            continue;
        if (perms[0] != 'r' || perms[1] != 'w')
            continue;
        // Skip ALL shared objects (incl. our own libx3ui.so + ImGui/SDL data) and
        // special regions. Otherwise the scanner matches its OWN search-value var,
        // a fake candidate that survives every rescan. Game state lives in the
        // heap, anonymous maps, or the engine executable's own data — none .so.
        if (strstr(path, ".so") ||
            strstr(path, "[stack]") || strstr(path, "[vvar]") ||
            strstr(path, "[vdso]")  || strstr(path, "[vsyscall]"))
            continue;
        regs.push_back({(uintptr_t)s, (uintptr_t)e});
    }
    fclose(f);
}

static void scan_first(int32_t target)
{
    mem_open();
    g_cands.clear();
    g_scan_capped = false;
    std::vector<std::pair<uintptr_t, uintptr_t>> regs;
    mem_regions(regs);

    const size_t CHUNK = 16u << 20;                // 16MB, multiple of 4
    std::vector<unsigned char> buf(CHUNK);
    for (auto &rg : regs) {
        for (uintptr_t base = rg.first; base < rg.second; ) {
            size_t want = rg.second - base;
            if (want > CHUNK) want = CHUNK;
            ssize_t r = pread(g_memfd, buf.data(), want, (off_t)base);
            if (r <= 0) break;
            size_t n = (size_t)r;
            for (size_t off = 0; off + 4 <= n; off += 4) {   // 4-byte aligned
                int32_t v;
                memcpy(&v, buf.data() + off, 4);
                if (v == target) {
                    g_cands.push_back({base + off, v});
                    if (g_cands.size() >= 500000) { g_scan_capped = true; return; }
                }
            }
            base += n;
        }
    }
}

enum { NEXT_EQ, NEXT_DEC, NEXT_INC, NEXT_CHANGED, NEXT_UNCHANGED };

// Re-read every candidate and keep those matching the comparison vs. their
// LAST-seen value (c.val). Candidates whose address no longer reads (freed
// transient/menu data) are dropped automatically.
static void scan_next(int mode, int32_t target)
{
    std::vector<Candidate> keep;
    keep.reserve(g_cands.size());
    for (auto &c : g_cands) {
        int32_t v;
        if (!mem_read_i32(c.addr, &v))
            continue;
        bool ok = false;
        switch (mode) {
            case NEXT_EQ:        ok = (v == target); break;
            case NEXT_DEC:       ok = (v < c.val);   break;
            case NEXT_INC:       ok = (v > c.val);   break;
            case NEXT_CHANGED:   ok = (v != c.val);  break;
            case NEXT_UNCHANGED: ok = (v == c.val);  break;
        }
        if (ok)
            keep.push_back({c.addr, v});   // remember current value for the next diff
    }
    g_cands.swap(keep);
}

// ---- anchored snapshot: seed candidates from the CONFIRMED fixed globals -----
// Credits is encoded (not a plain int32 == displayed value) but lives inside the
// player/galaxy/universe structs reachable from fixed .bss globals (found via
// SA_SetAsPlayerObject). Snapshot EVERY aligned int32 in those structs, then
// narrow with Changed/Unchanged — the encoded credits word changes when credits
// changes, so it survives. Result addresses hang off fixed globals -> stable.
static uintptr_t g_root_galaxy = 0, g_root_universe = 0, g_root_player = 0;

static void resolve_roots(void)
{
    mem_open();
    int32_t v, scene;
    g_root_galaxy   = (mem_read_i32(0x0837ceb8, &v) && v) ? (uintptr_t)(uint32_t)v : 0;
    g_root_universe = (mem_read_i32(0x0829d818, &v) && v) ? (uintptr_t)(uint32_t)v : 0;
    g_root_player   = 0;
    if (mem_read_i32(0x0837a0d0, &scene) && scene &&
        mem_read_i32((uintptr_t)(uint32_t)scene + 0x38, &v) && v)
        g_root_player = (uintptr_t)(uint32_t)v;
}

static void anchored_snapshot(void)
{
    resolve_roots();
    g_cands.clear();
    g_scan_capped = false;
    struct R { uintptr_t base; size_t size; } rs[3];
    int nr = 0;
    if (g_root_galaxy)   rs[nr++] = { g_root_galaxy,   0x100000 };  // ~1MB galaxy obj
    if (g_root_universe) rs[nr++] = { g_root_universe, 0x40000  };
    if (g_root_player)   rs[nr++] = { g_root_player,   0x8000   };

    std::vector<unsigned char> buf;
    for (int i = 0; i < nr; ++i) {
        buf.resize(rs[i].size);
        ssize_t r = pread(g_memfd, buf.data(), rs[i].size, (off_t)rs[i].base);
        if (r <= 0) continue;
        size_t n = (size_t)r;
        for (size_t o = 0; o + 4 <= n; o += 4) {
            int32_t vv; memcpy(&vv, buf.data() + o, 4);
            g_cands.push_back({ rs[i].base + o, vv });
            if (g_cands.size() >= 4000000) { g_scan_capped = true; return; }
        }
    }
}

// ---- flat differential over ALL writable memory (memory-safe, page-hashed) ---
// Cheat-Engine "unknown initial value": hash every 4KB page (snapshot), then
// after credits changes, seed candidates ONLY from pages whose hash changed.
// Finds the credits word anywhere & under any encoding (the page changes when
// the value changes). Stores ~12 bytes/page (~2MB for 600MB) instead of copying.
struct PageHash { uintptr_t addr; uint32_t hash; };
static std::vector<PageHash> g_pagehashes;

static uint32_t hash_page(const unsigned char *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static void flat_snapshot(void)
{
    mem_open();
    g_pagehashes.clear();
    std::vector<std::pair<uintptr_t, uintptr_t>> regs;
    mem_regions(regs);
    const size_t CH = 1u << 20;
    std::vector<unsigned char> buf(CH);
    for (auto &rg : regs) {
        for (uintptr_t base = rg.first; base < rg.second; ) {
            size_t want = rg.second - base; if (want > CH) want = CH;
            ssize_t r = pread(g_memfd, buf.data(), want, (off_t)base);
            if (r <= 0) { base += CH; continue; }
            size_t n = (size_t)r;
            for (size_t o = 0; o + 4096 <= n; o += 4096)
                g_pagehashes.push_back({ base + o, hash_page(buf.data() + o, 4096) });
            base += n;
        }
    }
}

static void flat_find_changed(void)
{
    mem_open();
    g_cands.clear();
    g_scan_capped = false;
    unsigned char page[4096];
    for (auto &ph : g_pagehashes) {
        if (pread(g_memfd, page, 4096, (off_t)ph.addr) != 4096) continue;
        if (hash_page(page, 4096) == ph.hash) continue;          // page unchanged
        for (size_t o = 0; o + 4 <= 4096; o += 4) {
            int32_t v; memcpy(&v, page + o, 4);
            g_cands.push_back({ ph.addr + o, v });
            if (g_cands.size() >= 4000000) { g_scan_capped = true; return; }
        }
    }
}

// ---- structural finder: locate the player object via a .bss global ----------
// The engine is non-PIE (loads at 0x08048000), so its globals live at FIXED
// low addresses. We scan those globals for ones holding a heap pointer, deref,
// and search the pointed-to struct for the credits value. A global whose struct
// holds the (tracking) credits IS the stable player-object pointer.
struct PtrHit { uintptr_t gslot; uint32_t off; };
static std::vector<PtrHit> g_hits;

static void maps_split(std::vector<std::pair<uintptr_t, uintptr_t>> &statics,
                       std::vector<std::pair<uintptr_t, uintptr_t>> &dyn)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e;
        char perms[8] = {0}, path[256] = {0};
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %255[^\n]", &s, &e, perms, path) < 3)
            continue;
        if (perms[0] != 'r' || perms[1] != 'w') continue;
        if (strstr(path, ".so") || strstr(path, "[stack]") ||
            strstr(path, "[vvar]") || strstr(path, "[vdso]") || strstr(path, "[vsyscall]"))
            continue;
        if (s < 0x09000000) statics.push_back({(uintptr_t)s, (uintptr_t)e});  // engine .data/.bss
        else                dyn.push_back({(uintptr_t)s, (uintptr_t)e});      // heap + anon
    }
    fclose(f);
}

static bool in_ranges(uintptr_t a, const std::vector<std::pair<uintptr_t, uintptr_t>> &rs)
{
    for (auto &r : rs) if (a >= r.first && a < r.second) return true;
    return false;
}

// Read credits live through a global: [gslot] -> base, then [base+off].
static bool read_via_global(const PtrHit &h, int32_t *out)
{
    int32_t base;
    if (!mem_read_i32(h.gslot, &base)) return false;
    return mem_read_i32((uintptr_t)base + h.off, out);
}

static void find_player(int32_t value)
{
    mem_open();
    g_hits.clear();
    std::vector<std::pair<uintptr_t, uintptr_t>> statics, dyn;
    maps_split(statics, dyn);

    const size_t WIN = 0x4000;            // search 16KB into each pointed-to struct
    std::vector<unsigned char> sbuf, win(WIN);
    for (auto &rg : statics) {
        size_t len = rg.second - rg.first;
        sbuf.resize(len);
        ssize_t r = pread(g_memfd, sbuf.data(), len, (off_t)rg.first);
        if (r <= 0) continue;
        size_t n = (size_t)r;
        for (size_t o = 0; o + 4 <= n; o += 4) {
            uint32_t p;
            memcpy(&p, sbuf.data() + o, 4);
            if (!in_ranges(p, dyn)) continue;                  // only real heap pointers
            ssize_t wr = pread(g_memfd, win.data(), WIN, (off_t)p);
            if (wr <= 0) continue;
            for (size_t wo = 0; wo + 4 <= (size_t)wr; wo += 4) {
                int32_t v;
                memcpy(&v, win.data() + wo, 4);
                if (v == value) {
                    g_hits.push_back({rg.first + o, (uint32_t)wo});
                    break;                                     // first match per struct
                }
            }
            if (g_hits.size() >= 400) return;
        }
    }
}

static void narrow_player(int32_t value)
{
    std::vector<PtrHit> keep;
    for (auto &h : g_hits) {
        int32_t v;
        if (read_via_global(h, &v) && v == value)
            keep.push_back(h);
    }
    g_hits.swap(keep);
}

// ---- hardware write-watchpoint: catch the instruction that writes an address -
// perf_event_open HW breakpoint; on write a SIGIO handler records the writer's
// instruction pointer. Engine is non-PIE @0x08048000, so that IP == the Ghidra
// address directly -> decompile it to see how credits is read/stored.
static int                    g_wp_fds[4] = { -1, -1, -1, -1 };
static int                    g_wp_count  = 0;
static volatile unsigned long g_writer_ip = 0;
static volatile unsigned long g_writer_addr = 0;
static volatile int           g_wp_hits   = 0;
static unsigned long          g_stack[48];   // stack words captured at the write
static volatile int           g_stack_n   = 0;

static void wp_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    ucontext_t *u = (ucontext_t *)uc;
    g_writer_ip = (unsigned long)u->uc_mcontext.gregs[REG_EIP];
    if (si) g_writer_addr = (unsigned long)si->si_addr;
    // Capture the stack above ESP so we can recover the caller chain (return
    // addresses in .text) — the generic VM copy's caller is what read credits.
    const unsigned long *sp = (const unsigned long *)u->uc_mcontext.gregs[REG_ESP];
    for (int i = 0; i < 48; ++i) g_stack[i] = sp[i];
    g_stack_n = 48;
    g_wp_hits++;
    for (int i = 0; i < 4; ++i)
        if (g_wp_fds[i] >= 0) ioctl(g_wp_fds[i], PERF_EVENT_IOC_DISABLE, 0);
}

static int arm_one(uintptr_t addr)
{
    struct perf_event_attr a; memset(&a, 0, sizeof a);
    a.type = PERF_TYPE_BREAKPOINT; a.size = sizeof a;
    a.bp_type = HW_BREAKPOINT_W; a.bp_addr = addr; a.bp_len = HW_BREAKPOINT_LEN_4;
    a.sample_period = 1; a.disabled = 0; a.exclude_kernel = 1; a.exclude_hv = 1;
    int fd = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_ASYNC);
    fcntl(fd, F_SETSIG, SIGIO);
    struct f_owner_ex ow; ow.type = F_OWNER_TID; ow.pid = (pid_t)syscall(SYS_gettid);
    fcntl(fd, F_SETOWN_EX, &ow);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    return fd;
}

// Arm up to 4 hardware write-watchpoints (the CPU's debug-register limit).
static void arm_write_watches(const uintptr_t *addrs, int n)
{
    for (int i = 0; i < 4; ++i)
        if (g_wp_fds[i] >= 0) { close(g_wp_fds[i]); g_wp_fds[i] = -1; }
    g_writer_ip = 0; g_writer_addr = 0; g_wp_hits = 0; g_wp_count = 0;

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = wp_handler; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    sigaction(SIGIO, &sa, NULL);

    if (n > 4) n = 4;
    for (int i = 0; i < n; ++i) {
        int fd = arm_one(addrs[i]);
        if (fd >= 0) g_wp_fds[g_wp_count++] = fd;
    }
    if (g_wp_count == 0)
        fprintf(stderr, "[x3ui] watch arm failed: %s\n", strerror(errno));
    else
        fprintf(stderr, "[x3ui] armed %d hardware write-watch(es)\n", g_wp_count);
}

static void state_bridge_window(void)
{
    if (!ImGui::Begin("x3ui — state bridge"))
    { ImGui::End(); return; }

    ImGui::TextDisabled("Find a value (e.g. credits) by scanning RAM.");
    ImGui::InputInt("value", &g_scan_value);

    if (ImGui::Button("First scan"))      scan_first(g_scan_value);
    ImGui::SameLine();
    if (ImGui::Button("Anchor snapshot")) anchored_snapshot();
    ImGui::SameLine();
    if (ImGui::Button("Reset"))           { g_cands.clear(); g_watch = 0; }
    ImGui::TextDisabled("Unknown-value (encoded credits): snapshot all, then find changed:");
    if (ImGui::Button("Flat snapshot"))   flat_snapshot();
    ImGui::SameLine();
    if (ImGui::Button("Find changed"))    flat_find_changed();
    ImGui::SameLine();
    ImGui::Text("pages: %zu", g_pagehashes.size());
    ImGui::Text("roots: gal=0x%08lx uni=0x%08lx ply=0x%08lx",
                (unsigned long)g_root_galaxy, (unsigned long)g_root_universe,
                (unsigned long)g_root_player);

    ImGui::TextDisabled("Narrow (change value in-game, then click):");
    if (ImGui::Button("= value"))   scan_next(NEXT_EQ, g_scan_value);
    ImGui::SameLine();
    if (ImGui::Button("Decreased")) scan_next(NEXT_DEC, 0);
    ImGui::SameLine();
    if (ImGui::Button("Increased")) scan_next(NEXT_INC, 0);
    ImGui::SameLine();
    if (ImGui::Button("Changed"))   scan_next(NEXT_CHANGED, 0);
    ImGui::SameLine();
    if (ImGui::Button("Unchanged")) scan_next(NEXT_UNCHANGED, 0);

    ImGui::Text("candidates: %zu%s", g_cands.size(), g_scan_capped ? " (capped)" : "");

    if (!g_cands.empty() && g_cands.size() <= 40) {
        ImGui::Separator();
        for (auto &c : g_cands) {
            int32_t v = 0; mem_read_i32(c.addr, &v);
            char label[64];
            snprintf(label, sizeof label, "0x%08lx = %d", (unsigned long)c.addr, v);
            if (ImGui::Selectable(label, g_watch == c.addr))
                g_watch = c.addr;
        }
    }

    if (g_watch) {
        int32_t v = 0; bool ok = mem_read_i32(g_watch, &v);
        ImGui::Separator();
        ImGui::Text("WATCH 0x%08lx = %d%s",
                    (unsigned long)g_watch, v, ok ? "" : " (unreadable)");
        if (ImGui::Button("Find what writes this (x1)")) {
            uintptr_t a = g_watch; arm_write_watches(&a, 1);
        }
    }

    // Hardware watchpoints are limited to 4 (CPU debug registers). Arm 4 spread
    // across the current candidates so we catch the write even if only some
    // entries get rewritten.
    if (!g_cands.empty() && ImGui::Button("Find writer: watch x4 (spread)")) {
        size_t n = g_cands.size();
        uintptr_t a[4]; int k = 0;
        if (n >= 4) {
            a[0] = g_cands[0].addr;     a[1] = g_cands[n / 3].addr;
            a[2] = g_cands[2 * n / 3].addr; a[3] = g_cands[n - 1].addr; k = 4;
        } else {
            for (size_t i = 0; i < n; ++i) a[k++] = g_cands[i].addr;
        }
        arm_write_watches(a, k);
    }
    if (g_wp_count > 0 || g_writer_ip)
        ImGui::Text("watching %d | WRITER @ 0x%08lx hits=%d",
                    g_wp_count, g_writer_ip, g_wp_hits);
    if (g_stack_n) {
        ImGui::TextDisabled("caller chain (return addrs in .text):");
        for (int i = 0; i < g_stack_n; ++i) {
            unsigned long w = g_stack[i];
            if (w >= 0x0804e960 && w <= 0x08190133)   // .text range
                ImGui::Text("  [esp+0x%02x] = 0x%08lx", i * 4, w);
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Player object via FIXED globals (robust):");
    if (ImGui::Button("Find player"))      find_player(g_scan_value);
    ImGui::SameLine();
    if (ImGui::Button("Narrow = value"))   narrow_player(g_scan_value);
    ImGui::Text("global hits: %zu", g_hits.size());
    if (!g_hits.empty() && g_hits.size() <= 40) {
        for (auto &h : g_hits) {
            int32_t v = 0; bool ok = read_via_global(h, &v);
            ImGui::Text("[0x%08lx]+0x%x = %d%s",
                        (unsigned long)h.gslot, h.off, v, ok ? "" : " (bad)");
        }
    }
    ImGui::End();
}

// ---- get_text_simple EXECUTE-trace: log (page,id,caller) per call, to settle
// whether the displayed object NAME is composed by engine C or the MSCI VM, and
// which (computed) text page it queries. A perf HW *execute* breakpoint fires on
// every get_text_simple(page,id) call on the main thread; a dedicated RT-signal
// handler reads the cdecl args off the stack at function entry ([esp+4]=page,
// [esp+8]=id, [esp]=return addr into the caller). Map caller -> Ghidra to see if
// it's the MSCI string interpreter (VM) or a HUD/menu function (engine).
static constexpr unsigned long GETTEXT_ADDR = 0x080acd30UL;   // get_text_simple(page,id)
struct TextCall { uint32_t eip, page, id, caller0, callers[4]; };
static TextCall      g_tc[4096];
static volatile int  g_tc_n = 0;          // captured (page-filtered) entries
static volatile long g_tc_total = 0;      // all hits (incl. filtered-out)
static int           g_tc_min_page = 100000;  // capture only page >= this (name pages are 301000/30220x)
static volatile int  g_tc_armed = 0;
static int           g_trace_fd = -1;
static int           g_trace_sig = 0;

static void text_trace_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si;
    if (!g_tc_armed) return;
    ucontext_t *u = (ucontext_t *)uc;
    const unsigned long *sp = (const unsigned long *)u->uc_mcontext.gregs[REG_ESP];
    g_tc_total++;
    if (g_tc_total >= 2000000) {        // hard safety: bound the signal storm
        g_tc_armed = 0;
        if (g_trace_fd >= 0) ioctl(g_trace_fd, PERF_EVENT_IOC_DISABLE, 0);
        return;
    }
    uint32_t page = (uint32_t)sp[1], id = (uint32_t)sp[2];   // cdecl: [esp]=ret [esp+4]=page [esp+8]=id
    if ((int)page < g_tc_min_page) return;
    int n = g_tc_n;
    if (n >= (int)(sizeof g_tc / sizeof g_tc[0])) {          // buffer full -> auto-disarm
        g_tc_armed = 0;
        if (g_trace_fd >= 0) ioctl(g_trace_fd, PERF_EVENT_IOC_DISABLE, 0);
        return;
    }
    TextCall &e = g_tc[n];
    e.eip = (uint32_t)u->uc_mcontext.gregs[REG_EIP];
    e.page = page; e.id = id; e.caller0 = (uint32_t)sp[0];
    int k = 0;
    for (int i = 1; i < 40 && k < 4; ++i) {                  // a few more .text return-addrs up-stack
        unsigned long w = sp[i];
        if (w >= 0x0804e960 && w <= 0x08190133) e.callers[k++] = (uint32_t)w;
    }
    for (; k < 4; ++k) e.callers[k] = 0;
    g_tc_n = n + 1;
}

static void arm_text_trace(void)
{
    if (g_trace_fd >= 0) { close(g_trace_fd); g_trace_fd = -1; }
    g_tc_n = 0; g_tc_total = 0;
    g_trace_sig = SIGRTMIN + 3;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = text_trace_handler; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(g_trace_sig, &sa, NULL);

    struct perf_event_attr a; memset(&a, 0, sizeof a);
    a.type = PERF_TYPE_BREAKPOINT; a.size = sizeof a;
    a.bp_type = HW_BREAKPOINT_X; a.bp_addr = GETTEXT_ADDR;
    a.sample_period = 1; a.disabled = 0; a.exclude_kernel = 1; a.exclude_hv = 1;
    // x86-64 kernels require bp_len == 8 for EXECUTE breakpoints (len 1/2/4 -> EINVAL),
    // even for a 32-bit task; verified via a perf probe. Try 8 first, then fall back.
    int lens[3] = { HW_BREAKPOINT_LEN_8, (int)sizeof(long), HW_BREAKPOINT_LEN_1 };
    int fd = -1, usedlen = 0;
    for (int li = 0; li < 3 && fd < 0; ++li) {
        a.bp_len = lens[li]; usedlen = lens[li];
        fd = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
    }
    if (fd < 0) { fprintf(stderr, "[x3ui] text-trace arm FAILED: %s\n", strerror(errno)); return; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_ASYNC);
    fcntl(fd, F_SETSIG, g_trace_sig);
    struct f_owner_ex ow; ow.type = F_OWNER_TID; ow.pid = (pid_t)syscall(SYS_gettid);
    fcntl(fd, F_SETOWN_EX, &ow);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    g_trace_fd = fd; g_tc_armed = 1;
    fprintf(stderr, "[x3ui] text-trace armed @0x%08lx (sig %d, len %d)\n",
            GETTEXT_ADDR, g_trace_sig, usedlen);
}

// Dump the captured calls to a file (main-thread; fopen is fine here, never in the
// signal handler) so the controller can read exact pages + caller addresses.
static void dump_text_trace(void)
{
    FILE *f = fopen("/tmp/x3_texttrace.tsv", "w");
    if (!f) { fprintf(stderr, "[x3ui] text-trace dump: fopen failed\n"); return; }
    fprintf(f, "# page\tid\tcaller_ret\tstack_callers   (total=%ld captured=%d min_page=%d)\n",
            g_tc_total, g_tc_n, g_tc_min_page);
    for (int i = 0; i < g_tc_n; ++i) {
        const TextCall &e = g_tc[i];
        fprintf(f, "%u\t%u\t0x%08x\t", e.page, e.id, e.caller0);
        for (int k = 0; k < 4 && e.callers[k]; ++k) fprintf(f, "0x%08x ", e.callers[k]);
        fprintf(f, "\n");
    }
    fclose(f);
    fprintf(stderr, "[x3ui] text-trace dumped %d rows -> /tmp/x3_texttrace.tsv\n", g_tc_n);
}

static void disarm_text_trace(void)
{
    g_tc_armed = 0;
    if (g_trace_fd >= 0) { ioctl(g_trace_fd, PERF_EVENT_IOC_DISABLE, 0); close(g_trace_fd); g_trace_fd = -1; }
    dump_text_trace();
}

// ---- generic execute-trace: capture a function's call args (ret + 7 args) at entry --------
struct ExecCall { uint32_t w[32]; uint32_t r[7]; };  // w[0]=ret w[1..]=stack words (scan code-range = call chain); r=eax,ebx,ecx,edx,esi,edi,ebp
static ExecCall      g_ec[256];
static volatile int  g_ec_n = 0;
static volatile long g_ec_total = 0;
static volatile int  g_ec_armed = 0;
static int           g_ec_fd = -1;
static uint32_t      g_ec_filter = 0;  // only log calls where some arg/reg == filter (0 = log all)

static void exec_trace_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si;
    if (!g_ec_armed) return;
    ucontext_t *u = (ucontext_t *)uc;
    const unsigned long *sp = (const unsigned long *)u->uc_mcontext.gregs[REG_ESP];
    g_ec_total++;
    if (g_ec_total >= 5000000) { g_ec_armed = 0; if (g_ec_fd >= 0) ioctl(g_ec_fd, PERF_EVENT_IOC_DISABLE, 0); return; }
    uint32_t regs[7] = { (uint32_t)u->uc_mcontext.gregs[REG_EAX], (uint32_t)u->uc_mcontext.gregs[REG_EBX],
                         (uint32_t)u->uc_mcontext.gregs[REG_ECX], (uint32_t)u->uc_mcontext.gregs[REG_EDX],
                         (uint32_t)u->uc_mcontext.gregs[REG_ESI], (uint32_t)u->uc_mcontext.gregs[REG_EDI],
                         (uint32_t)u->uc_mcontext.gregs[REG_EBP] };
    if (g_ec_filter) {
        bool match = false;
        for (int i = 1; i <= 24 && !match; ++i) if ((uint32_t)sp[i] == g_ec_filter) match = true;
        for (int i = 0; i < 7 && !match; ++i) if (regs[i] == g_ec_filter) match = true;
        if (!match) return;
    }
    int n = g_ec_n;
    if (n >= 256) { g_ec_armed = 0; if (g_ec_fd >= 0) ioctl(g_ec_fd, PERF_EVENT_IOC_DISABLE, 0); return; }
    for (int i = 0; i < 32; ++i) g_ec[n].w[i] = (uint32_t)sp[i];
    for (int i = 0; i < 7; ++i) g_ec[n].r[i] = regs[i];
    g_ec_n = n + 1;
}

static std::string overlay_exectrace(uint32_t addr, uint32_t filter)
{
    if (addr == 0) {                                   // addr 0 = disarm + dump
        g_ec_armed = 0;
        if (g_ec_fd >= 0) { ioctl(g_ec_fd, PERF_EVENT_IOC_DISABLE, 0); close(g_ec_fd); g_ec_fd = -1; }
        char b[200];
        snprintf(b, sizeof b, "ok exectrace total=%ld captured=%d\n", g_ec_total, g_ec_n);
        std::string r = b;
        for (int i = 0; i < g_ec_n; ++i) {
            snprintf(b, sizeof b, "[%d] ret=%08x a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x a6=%08x | "
                     "eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x\n",
                     i, g_ec[i].w[0], g_ec[i].w[1], g_ec[i].w[2], g_ec[i].w[3],
                     g_ec[i].w[4], g_ec[i].w[5], g_ec[i].w[6], g_ec[i].w[7],
                     g_ec[i].r[0], g_ec[i].r[1], g_ec[i].r[2], g_ec[i].r[3], g_ec[i].r[4], g_ec[i].r[5], g_ec[i].r[6]);
            r += b;
            r += "    bt:";  // code-range stack values = return-address call chain (outermost UI caller last)
            for (int k = 0; k < 32; ++k) {
                uint32_t v = g_ec[i].w[k];
                if (v >= 0x08049000u && v < 0x08300000u) { snprintf(b, sizeof b, " %08x", v); r += b; }
            }
            r += "\n";
        }
        return r;
    }
    if (g_ec_fd >= 0) { close(g_ec_fd); g_ec_fd = -1; }
    g_ec_n = 0; g_ec_total = 0; g_ec_filter = filter;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = exec_trace_handler; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
    sigaction(SIGRTMIN + 4, &sa, NULL);
    struct perf_event_attr a; memset(&a, 0, sizeof a);
    a.type = PERF_TYPE_BREAKPOINT; a.size = sizeof a;
    a.bp_type = HW_BREAKPOINT_X; a.bp_addr = addr;
    a.sample_period = 1; a.disabled = 0; a.exclude_kernel = 1; a.exclude_hv = 1;
    int lens[3] = { HW_BREAKPOINT_LEN_8, (int)sizeof(long), HW_BREAKPOINT_LEN_1 };
    int fd = -1;
    for (int li = 0; li < 3 && fd < 0; ++li) { a.bp_len = lens[li]; fd = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0); }
    if (fd < 0) { char b[80]; snprintf(b, sizeof b, "err exectrace arm: %s", strerror(errno)); return b; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_ASYNC);
    fcntl(fd, F_SETSIG, SIGRTMIN + 4);
    struct f_owner_ex ow; ow.type = F_OWNER_TID; ow.pid = (pid_t)syscall(SYS_gettid);
    fcntl(fd, F_SETOWN_EX, &ow);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    g_ec_fd = fd; g_ec_armed = 1;
    char b[96]; snprintf(b, sizeof b, "ok exectrace armed @0x%08x filter=0x%x", addr, filter); return b;
}

static void text_trace_window(void)
{
    if (!ImGui::Begin("x3ui - text trace")) { ImGui::End(); return; }
    ImGui::TextWrapped("execute-trace get_text_simple@0x080acd30 -> (page,id,caller). "
                       "Arm, THEN target a ship in-game (so the name composes while armed), "
                       "then Disarm. caller(ret) -> Ghidra tells engine-C vs MSCI-VM.");
    ImGui::SetNextItemWidth(120); ImGui::InputInt("min page (capture)", &g_tc_min_page);
    if (!g_tc_armed) { if (ImGui::Button("Arm"))    arm_text_trace(); }
    else             { if (ImGui::Button("Disarm")) disarm_text_trace(); }
    ImGui::SameLine(); if (ImGui::Button("Clear")) { g_tc_n = 0; g_tc_total = 0; }
    ImGui::SameLine(); if (ImGui::Button("Dump file")) dump_text_trace();
    ImGui::SameLine(); ImGui::Text("armed=%d  total=%ld  captured=%d",
                                   (int)g_tc_armed, g_tc_total, g_tc_n);
    if (g_tc_n && g_tc[0].eip != (uint32_t)GETTEXT_ADDR)
        ImGui::TextColored(ImVec4(1,0.55f,0.2f,1),
                           "skid: eip[0]=0x%08x != 0x%08lx (args may be off-by-prologue)",
                           g_tc[0].eip, GETTEXT_ADDR);
    ImGui::Separator();
    if (ImGui::BeginTable("tc", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_RowBg, ImVec2(0, 360))) {
        ImGui::TableSetupColumn("page"); ImGui::TableSetupColumn("id");
        ImGui::TableSetupColumn("caller(ret)"); ImGui::TableSetupColumn("stack .text callers");
        ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
        int n = g_tc_n; if (n > 600) n = 600;
        for (int i = 0; i < n; ++i) {
            const TextCall &e = g_tc[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%u", e.page);
            ImGui::TableNextColumn(); ImGui::Text("%u", e.id);
            ImGui::TableNextColumn(); ImGui::Text("0x%08x", e.caller0);
            ImGui::TableNextColumn();
            char b[160]; int o = 0; b[0] = 0;
            for (int k = 0; k < 4 && e.callers[k]; ++k)
                o += snprintf(b + o, (size_t)((int)sizeof b - o), "%08x ", e.callers[k]);
            ImGui::TextUnformatted(b);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// Live view of the player SHIP object, reached via the fixed global
// (ship = *(*0x0837a0d0 + 0x38)). Shows each dword as int + float so we can
// spot plain C++ fields (position floats, hull/shield/speed ints) by watching
// which ones move as you fly/fight. Stable across restarts (fixed global).
static void ship_window(void)
{
    if (!ImGui::Begin("x3ui - ship")) { ImGui::End(); return; }
    mem_open();
    int32_t scene = 0, ship = 0;
    bool ok = mem_read_i32(0x0837a0d0, &scene) && scene &&
              mem_read_i32((uintptr_t)(uint32_t)scene + 0x38, &ship) && ship;
    if (!ok) {
        ImGui::TextDisabled("player ship not resolved (loaded into a game?)");
        ImGui::End();
        return;
    }
    ImGui::Text("ship @ 0x%08x  (scene=0x%08x)", (uint32_t)ship, (uint32_t)scene);
    ImGui::TextDisabled("fly / take damage and watch which rows move:");
    if (ImGui::BeginTable("ship", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_RowBg, ImVec2(0, 260))) {
        ImGui::TableSetupColumn("off");
        ImGui::TableSetupColumn("hex");
        ImGui::TableSetupColumn("int");
        ImGui::TableSetupColumn("float");
        ImGui::TableHeadersRow();
        for (int off = 0; off < 0x140; off += 4) {
            int32_t v = 0; mem_read_i32((uintptr_t)(uint32_t)ship + off, &v);
            float f; memcpy(&f, &v, 4);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("+0x%03x", off);
            ImGui::TableNextColumn(); ImGui::Text("%08x", (uint32_t)v);
            ImGui::TableNextColumn(); ImGui::Text("%d", v);
            ImGui::TableNextColumn();
            if (f > -1e9f && f < 1e9f && (f > 1e-3f || f < -1e-3f || f == 0.0f))
                ImGui::Text("%.2f", f);
            else
                ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ---- engine bridge: call the MSCI script runner directly (we're in-process) --
// FUN_0809e270 = async spawn (returns a task handle, 0 if script/entry not found);
// FUN_0809e340 = spawn + immediate result. Plain cdecl, stack args, variadic tail.
// The engine host global is read implicitly. Engine is non-PIE @0x08048000 so
// these absolute addresses are valid at runtime.
typedef int (*run_script_t)(void *owner, int z, int scriptId,
                            const char *entry, int flags, unsigned argc, ...);
static run_script_t run_script_async = (run_script_t)0x0809e270;
static run_script_t run_script_sync  = (run_script_t)0x0809e340;
static int g_msci_async_h = -999;   // last async task handle (nonzero = engine accepted)
static int g_msci_sync_v  = -999;   // last sync result

// FUN_080ad620(scriptId, entry) -> 1 if a loaded script of that id has that entry.
typedef int (*script_exists_t)(int scriptId, const char *entry);
static script_exists_t script_exists = (script_exists_t)0x080ad620;
static int g_money_id = -1;     // resolved script id for our credits script
static int g_credits  = -999;   // last credits read via MSCI

// Find our custom script's class id by probing for its unique entry name.
static int msci_find_id(const char *entry)
{
    for (int id = 0; id < 0x8000; ++id)             // widened from 0x800 to catch AI-command scripts
        if (script_exists(id, entry)) return id;
    return -1;
}

// Production Mem: read the game's own address space via /proc/self/mem (the same
// safe pread path the scanners use — bad addresses return false, never crash).
static std::string overlay_issue_dock(uint32_t station, int cmd, int bay, int notify, int destmode);  // below
static std::string overlay_dockscript(uint32_t station, int thismode);  // below
static std::string overlay_runscript(std::string name, uint32_t thisH, uint32_t arg, int argc, int sync);  // below
static std::string overlay_runscript2(std::string name, uint32_t thisH, const std::vector<uint32_t>& a, int sync);  // below
static std::string overlay_jumpgate(uint32_t gate, int overshoot_km);  // below
static std::string overlay_cmdengage(uint32_t target, int op, int argc, int bay, int clear200);  // below
static std::string overlay_setcmd(int cmd);  // below
static std::string overlay_watchwrite(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);  // below
static std::string overlay_watchip(void);  // below
static std::string overlay_drive(int on, int rearm);  // below
static std::string overlay_dockdrive(uint32_t station, int op, int bay, int on);  // below
static std::string overlay_callfn1(uint32_t addr, uint32_t arg);  // below
static std::string overlay_callfn3(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2);  // below
static std::string overlay_sacall(int subcmd, int argc, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3);  // below
static std::string overlay_poke(uint32_t addr, uint32_t val);  // below
static std::string overlay_tracedock(uint32_t cmdId, uint32_t traceAddr);  // below
static std::string overlay_spawnorch(uint32_t station, int stage);  // below
static std::string overlay_settarget(uint32_t target_addr);  // below
static std::string overlay_synthkey(int scancode, int sym, int mod, int down);  // below
static std::string overlay_synthhold(int sc, int shift, int frames);  // below
static std::string overlay_synthdiag(void);  // below
static std::string overlay_taskid(uint32_t handle);  // below
static std::string overlay_spawn(uint32_t scriptKey, std::string entry, uint32_t thisId, uint32_t arg, int argc);  // below
static std::string overlay_dock2(uint32_t station, std::string name, uint32_t ctxOvr, uint32_t keyOvr);  // below
static std::string overlay_vmdock(uint32_t station, int do_op10);  // below
static std::string overlay_natdock(uint32_t station, int stage);  // below

static x3::Mem make_engine_mem(void)
{
    x3::Mem m;
    m.u32 = [](uint32_t a, uint32_t& o) -> bool {
        int32_t v;
        if (!mem_read_i32((uintptr_t)a, &v)) return false;
        o = (uint32_t)v;
        return true;
    };
    m.bytes = [](uint32_t addr, void* dst, size_t n) -> bool {
        return mem_read_bytes((uintptr_t)addr, dst, n);
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
    // Resolve {page,id} text refs via the engine's own get_text_simple(page,id).
    // __cdecl(int,int)->char*, read-only; safe here because read_current_sector and
    // the debug pump both run on the main thread between frames.
    m.text = [](uint32_t page, uint32_t id) -> std::string {
        static std::unordered_map<uint64_t, std::string> cache;   // names are constant
        uint64_t key = ((uint64_t)page << 32) | id;
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        typedef const char* (*get_text_t)(int, int);
        static get_text_t get_text = (get_text_t)0x080acd30;
        const char* s = get_text((int)page, (int)id);
        std::string out;
        for (int i = 0; s && i < 128 && s[i]; ++i) out.push_back(s[i]);
        if (out == "=TextIDNotFound=") out.clear();   // engine's not-found sentinel
        cache.emplace(key, out);
        return out;
    };
    // Self-test harness: `x3dbg dock <station>` issues a guarded player dock (writes + engine
    // call) — runs in the debug pump on the main thread between frames, so it's call-safe.
    m.issue_dock = [](uint32_t station, int cmd, int bay, int notify, int dmode) -> std::string {
        return overlay_issue_dock(station, cmd, bay, notify, dmode);
    };
    m.dockscript = [](uint32_t station, int thismode) -> std::string {
        return overlay_dockscript(station, thismode);
    };
    m.runscript = [](std::string name, uint32_t thisH, uint32_t arg, int argc, int sync) -> std::string {
        return overlay_runscript(name, thisH, arg, argc, sync);
    };
    m.runscript2 = [](std::string name, uint32_t thisH, std::vector<uint32_t> a, int sync) -> std::string {
        return overlay_runscript2(name, thisH, a, sync);
    };
    m.jumpgate = [](uint32_t gate, int overshoot) -> std::string {
        return overlay_jumpgate(gate, overshoot);
    };
    m.cmdengage = [](uint32_t target, int op, int argc, int bay, int clear200) -> std::string {
        return overlay_cmdengage(target, op, argc, bay, clear200);
    };
    m.setcmd = [](int cmd) -> std::string { return overlay_setcmd(cmd); };
    m.watchwrite = [](uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) -> std::string {
        return overlay_watchwrite(a0, a1, a2, a3);
    };
    m.watchip = []() -> std::string { return overlay_watchip(); };
    m.drive = [](int on, int rearm) -> std::string { return overlay_drive(on, rearm); };
    m.dockdrive = [](uint32_t station, int op, int bay, int on) -> std::string { return overlay_dockdrive(station, op, bay, on); };
    m.callfn1 = [](uint32_t addr, uint32_t arg) -> std::string { return overlay_callfn1(addr, arg); };
    m.callfn3 = [](uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2) -> std::string { return overlay_callfn3(addr, a0, a1, a2); };
    m.sacall = [](int subcmd, int argc, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3) -> std::string { return overlay_sacall(subcmd, argc, c0, c1, c2, c3); };
    m.poke = [](uint32_t addr, uint32_t val) -> std::string { return overlay_poke(addr, val); };
    m.tracedock = [](uint32_t cmdId, uint32_t traceAddr) -> std::string { return overlay_tracedock(cmdId, traceAddr); };
    m.spawnorch = [](uint32_t station, int stage) -> std::string { return overlay_spawnorch(station, stage); };
    m.settarget = [](uint32_t t) -> std::string { return overlay_settarget(t); };
    m.synthkey = [](int sc, int sym, int mod, int down) -> std::string { return overlay_synthkey(sc, sym, mod, down); };
    m.synthhold = [](int sc, int shift, int frames) -> std::string { return overlay_synthhold(sc, shift, frames); };
    m.synthdiag = []() -> std::string { return overlay_synthdiag(); };
    m.taskid = [](uint32_t handle) -> std::string { return overlay_taskid(handle); };
    m.spawn = [](uint32_t key, std::string entry, uint32_t thisId, uint32_t arg, int argc) -> std::string {
        return overlay_spawn(key, entry, thisId, arg, argc);
    };
    m.dock2 = [](uint32_t station, std::string name, uint32_t ctxOvr, uint32_t keyOvr) -> std::string {
        return overlay_dock2(station, name, ctxOvr, keyOvr);
    };
    m.vmdock = [](uint32_t station, int do_op10) -> std::string { return overlay_vmdock(station, do_op10); };
    m.natdock = [](uint32_t station, int stage) -> std::string { return overlay_natdock(station, stage); };
    m.exectrace = [](uint32_t addr, uint32_t filter) -> std::string { return overlay_exectrace(addr, filter); };
    return m;
}

// Two independent filter layers (multi-select): a relation layer (own/friend/
// neutral/enemy) and a type layer (ships/stations/gates/roids). An object shows if
// it matches ANY checked relation OR ANY checked type; nothing checked = show all.
// The table and the 3D map each keep their own selection.
enum RelBit  { RB_OWN = 1, RB_FRIEND = 2, RB_NEUTRAL = 4, RB_ENEMY = 8 };
enum TypeBit { TB_SHIP = 1, TB_STATION = 2, TB_GATE = 4, TB_ROID = 8 };

static int rel_bit(int8_t r)
{
    switch (r) {
        case x3::REL_OWN:     return RB_OWN;
        case x3::REL_FRIEND:  return RB_FRIEND;
        case x3::REL_NEUTRAL: return RB_NEUTRAL;
        case x3::REL_ENEMY:   return RB_ENEMY;
    }
    return 0;
}
static int type_bit(int16_t t)
{
    if (t == x3::off::TYPE_SHIP) return TB_SHIP;
    if (t == x3::off::TYPE_STATION || t == x3::off::TYPE_FACTORY) return TB_STATION;
    if (t == x3::off::TYPE_GATE) return TB_GATE;
    if (t == x3::off::TYPE_ASTEROID) return TB_ROID;
    return 0;
}
static bool obj_passes_filter(const x3::ObjInfo& o, int relmask, int typemask)
{
    if (!relmask && !typemask) return true;
    return (relmask & rel_bit(o.relation)) || (typemask & type_bit(o.type));
}

static void map_filter_bar(int* relmask, int* typemask)
{
    ImGui::TextDisabled("rel"); ImGui::SameLine();
    ImGui::CheckboxFlags("own", relmask, RB_OWN);            ImGui::SameLine();
    ImGui::CheckboxFlags("friend", relmask, RB_FRIEND);      ImGui::SameLine();
    ImGui::CheckboxFlags("neutral", relmask, RB_NEUTRAL);    ImGui::SameLine();
    ImGui::CheckboxFlags("enemy", relmask, RB_ENEMY);        ImGui::SameLine();
    ImGui::TextDisabled(" type"); ImGui::SameLine();
    ImGui::CheckboxFlags("ships", typemask, TB_SHIP);        ImGui::SameLine();
    ImGui::CheckboxFlags("stations", typemask, TB_STATION);  ImGui::SameLine();
    ImGui::CheckboxFlags("gates", typemask, TB_GATE);        ImGui::SameLine();
    ImGui::CheckboxFlags("roids", typemask, TB_ROID);        ImGui::SameLine();
    if (ImGui::SmallButton("all")) { *relmask = 0; *typemask = 0; }
}

static ImU32       faction_color(const x3::ObjInfo& o);   // defined with the 3D map
static const char* relation_label(int8_t rel);

// Current selection (the object the user left-clicked in the table or 3D map).
// select_target() is the single place that will also write the engine's
// current-target field once that RE lands (the address is RE'd separately).
static uint32_t g_sel_id    = 0;
static uint32_t g_sel_addr  = 0;
static bool     g_sel_alive = false;   // selected addr confirmed in the live object list
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

// THE working player auto-dock: target the object (HUD lock), then fire the game's own
// "Dock At" key (Shift+D = cmdId 0x1064) via its input dispatcher — this spawns the bound
// !ship.cmd.movestation.std orchestrator with [THIS]=player, which flies the full multi-leg
// dock. Verified live: ship docks end-to-end. (input_dispatch_key_script@0x08053690.)
static void player_dock_input(const x3::ObjInfo& o)
{
    if (!o.addr) return;
    select_target(o);                                  // HUD target = the station (Dock At acts on it)
    typedef void (__attribute__((cdecl)) *keydisp_t)(unsigned);
    ((keydisp_t)(uintptr_t)0x08053690u)(0x1064u);      // Shift+D ("Dock At") through the real input path
}

// ---- command bridge: order a ship to dock/move/attack an object -------------
// The player's own ship (the one being flown), via the scene global.
static uint32_t player_ship_addr(void)
{
    x3::Mem m = make_engine_mem();
    uint32_t scene = 0, ship = 0;
    if (m.u32(x3::off::SCENE_GLOBAL, scene) && scene)
        m.u32(scene + x3::off::SCENE_PLAYER_ANCHOR, ship);
    return ship;
}

// Diagnostics for the last command write — so we can confirm IN-PROCESS (no flaky socket)
// that the write landed at the right offset (readback==wrote).
static uint32_t g_cmd_dbg_ship = 0, g_cmd_dbg_payload = 0, g_cmd_dbg_target = 0;
static int      g_cmd_dbg_wrote = -1, g_cmd_dbg_readback = -1;

// Engage the PLAYER ship's autopilot toward `target` — writes the full command struct on the
// payload exactly as the validated-DOCKAT engage block (@0x0811cb14) / MOVE-FLYTO handler
// (@0x08123250) do: command id @+0xC4, target @+0x98, the navigation DESTINATION @+0xAC/B0/B4
// (THE field our earlier zeros broke — fly-to: the target's world pos; the bound !move.* MSCI
// script then navigates to it with avoidance), start-time @+0x84, the command-active handle
// ship+0x78=1, and for dock the family index @+0xC0=9 + a bay @+0xBC. obj+0x40 NOT touched
// (bit19 is set by default). See memory x3-ship-set-command. Crash-safe (pwrite).
static void player_autopilot(uint32_t ship, int16_t cmdId, uint32_t target)
{
    g_cmd_dbg_ship = ship; g_cmd_dbg_target = target; g_cmd_dbg_wrote = cmdId;
    g_cmd_dbg_payload = 0;  g_cmd_dbg_readback = -1;
    if (!ship) return;
    int32_t cls = 0, plw = 0;
    if (!mem_read_i32(ship + x3::off::OBJ_TYPE, &cls) ||
        (int16_t)(cls & 0xFFFF) != x3::off::TYPE_SHIP) return;
    if (!mem_read_i32(ship + x3::off::OBJ_PAYLOAD, &plw) || (uint32_t)plw < 0x08000000u) return;
    uint32_t pl = (uint32_t)plw; g_cmd_dbg_payload = pl;
    int32_t rc = 0, now = 0;
    if (mem_read_i32(x3::off::RENDER_CONFIG, &rc) && rc)
        mem_read_i32((uint32_t)rc + x3::off::RENDER_CFG_TIME, &now);
    int32_t dest[3] = { 0, 0, 0 };                                    // dest = target world pos (safe;
    int32_t ttf = 0;                                                  // calling the engine dock-approach
    if (target && mem_read_i32(target + x3::off::OBJ_TRANSFORM, &ttf) && ttf) {  // fn @0x08104ff0 crashed,
        mem_read_i32((uint32_t)ttf + x3::off::TF_X, &dest[0]);        // it assumes a full bay setup we
        mem_read_i32((uint32_t)ttf + x3::off::TF_Y, &dest[1]);        // can't safely provide from here)
        mem_read_i32((uint32_t)ttf + x3::off::TF_Z, &dest[2]);
    }
    mem_write_u32(pl + x3::off::SHIP_CMD_TARGET,   target);
    mem_write_u32(pl + x3::off::SHIP_CMD_DEST + 0, (uint32_t)dest[0]); // <-- the destination
    mem_write_u32(pl + x3::off::SHIP_CMD_DEST + 4, (uint32_t)dest[1]);
    mem_write_u32(pl + x3::off::SHIP_CMD_DEST + 8, (uint32_t)dest[2]);
    mem_write_u32(pl + x3::off::SHIP_CMD_STARTTIME, (uint32_t)now);
    mem_write_u32(ship + x3::off::SHIP_NOTIFY_OBJ, 0);                 // no completion-notify hook
    mem_write_u32(ship + x3::off::SHIP_NOTIFY_VAL, 0);
    { int32_t fl = 0;                                                  // OPEN THE GATE: the per-frame
      if (mem_read_i32(ship + x3::off::OBJ_FLAGS, &fl))                // command executor only runs a
          mem_write_u32(ship + x3::off::OBJ_FLAGS,                     // ship's command if obj+0x40
                        (uint32_t)fl | x3::off::AUTOPILOT_ENGAGED); }  // bit19 is set (manual/"Stop" clears it)
    if (cmdId == x3::off::CMD_DOCKAT) {                                // DECOMPILED case-4 (DOCK): with
        mem_write_u32(pl + x3::off::SHIP_CMD_FAMILY, 0);               // bay==-1 the executor reads +0xAC as
        mem_write_u32(pl + x3::off::SHIP_CMD_BAY, (uint32_t)-1);       // a LITERAL world point (station ctr)
    }                                                                 // and runs the real dock state machine
    //  (cmd 4<->5 + dock_select_bay_node) itself. bay>=0 would mis-transform +0xAC by station orient -> the
    //  ship flew to a garbage point ~10km out. family +0xC0 is bookkeeping (executor ignores it).
    mem_write_u16(pl + x3::off::SHIP_CMD_ID, (uint16_t)cmdId);         // LAST → script starts
    int32_t back = 0;
    if (mem_read_i32(pl + x3::off::SHIP_CMD_ID, &back)) g_cmd_dbg_readback = back & 0xFFFF;
}
static void player_autopilot_cancel(uint32_t ship)
{
    g_cmd_dbg_wrote = 0; g_cmd_dbg_readback = -1;
    int32_t plw = 0;
    if (ship && mem_read_i32(ship + x3::off::OBJ_PAYLOAD, &plw) && (uint32_t)plw >= 0x08000000u) {
        mem_write_u16((uint32_t)plw + x3::off::SHIP_CMD_ID, x3::off::CMD_NONE);  // cmd 0 = idle/decel
        mem_write_u32(ship + x3::off::SHIP_NOTIFY_OBJ, 0);
        int32_t fl = 0;                                             // DISENGAGE = clear bit19 (decompiled
        if (mem_read_i32(ship + x3::off::OBJ_FLAGS, &fl))           // truth: player_input_flight_handler
            mem_write_u32(ship + x3::off::OBJ_FLAGS,                // @0x0805ad8b does exactly flags&=~0x80000.
                          (uint32_t)fl & ~x3::off::AUTOPILOT_ENGAGED); // bit19 clear -> arrival physics
    }                                                              // reclaims the ship -> stops + manual.
}

// Auto-dock the PLAYER ship at `station` — PARAMETERIZED for the live experiment harness. The
// engine helper dock_compute_approach_for_bay@0x08104ff0 reliably CRASHES when called standalone
// (deep nested bay-slot struct walks fault outside the reserved-bay context — 2 crashes), so we do
// NOT call it. We write only the command struct (dest = station world center) and sweep the dock-
// distinguishing fields — cmd id (4 DOCK / 5 FLYTO), bay (a real index drives the executor into the
// bay; -1 = literal world point), notify (ship+0x78, 1 on every real mid-dock NPC) — to find which
// combo makes the executor's case-4/5 actually enter. Crash-safe writes (pwrite). Returns status.
static std::string player_dock(uint32_t ship, uint32_t station, int cmd, int bay, int notify, int destmode)
{
    mem_open_rw();
    char buf[200];
    if (!ship || !station) return "err no ship/station";
    int32_t cls = 0, shipPl = 0;
    if (!mem_read_i32(ship + x3::off::OBJ_TYPE, &cls) || (int16_t)(cls & 0xFFFF) != x3::off::TYPE_SHIP)
        return "err ship not class 7";
    if (!mem_read_i32(ship + x3::off::OBJ_PAYLOAD, &shipPl) || (uint32_t)shipPl < 0x08000000u)
        return "err ship payload";
    int32_t stcls = 0;
    if (!mem_read_i32(station + x3::off::OBJ_TYPE, &stcls)) return "err station type";
    int16_t st = (int16_t)(stcls & 0xFFFF);
    if (st != x3::off::TYPE_STATION && st != x3::off::TYPE_FACTORY) return "err not a station";
    // destmode: 0 = station world center (literal world point, for bay=-1);
    //           1 = (0,0,0)  (bay-local zero -> transforms to station center when bay>=0);
    //           2 = ship's current world pos.
    int32_t dest[3] = { 0, 0, 0 };
    if (destmode == 0) {
        int32_t tf = 0;
        if (mem_read_i32(station + x3::off::OBJ_TRANSFORM, &tf) && tf) {
            mem_read_i32((uint32_t)tf + x3::off::TF_X, &dest[0]);
            mem_read_i32((uint32_t)tf + x3::off::TF_Y, &dest[1]);
            mem_read_i32((uint32_t)tf + x3::off::TF_Z, &dest[2]);
        }
    } else if (destmode == 2) {
        int32_t tf = 0;
        if (mem_read_i32(ship + x3::off::OBJ_TRANSFORM, &tf) && tf) {
            mem_read_i32((uint32_t)tf + x3::off::TF_X, &dest[0]);
            mem_read_i32((uint32_t)tf + x3::off::TF_Y, &dest[1]);
            mem_read_i32((uint32_t)tf + x3::off::TF_Z, &dest[2]);
        }
    }   // destmode 1 leaves dest = (0,0,0)
    uint32_t pl = (uint32_t)shipPl;
    int32_t rc = 0, now = 0;
    if (mem_read_i32(x3::off::RENDER_CONFIG, &rc) && rc)
        mem_read_i32((uint32_t)rc + x3::off::RENDER_CFG_TIME, &now);
    mem_write_u32(pl + x3::off::SHIP_CMD_TARGET,   station);
    mem_write_u32(pl + x3::off::SHIP_CMD_DEST + 0, (uint32_t)dest[0]);
    mem_write_u32(pl + x3::off::SHIP_CMD_DEST + 4, (uint32_t)dest[1]);
    mem_write_u32(pl + x3::off::SHIP_CMD_DEST + 8, (uint32_t)dest[2]);
    mem_write_u32(pl + x3::off::SHIP_CMD_BAY,    (uint32_t)bay);
    mem_write_u32(pl + x3::off::SHIP_CMD_FAMILY, 0);
    mem_write_u32(pl + x3::off::SHIP_CMD_STARTTIME, (uint32_t)now);
    mem_write_u32(ship + x3::off::SHIP_NOTIFY_OBJ, (uint32_t)notify);
    mem_write_u32(ship + x3::off::SHIP_NOTIFY_VAL, 0);
    { int32_t fl = 0; if (mem_read_i32(ship + x3::off::OBJ_FLAGS, &fl))
          mem_write_u32(ship + x3::off::OBJ_FLAGS, (uint32_t)fl | x3::off::AUTOPILOT_ENGAGED); }
    mem_write_u16(pl + x3::off::SHIP_CMD_ID, (uint16_t)cmd);           // cmd LAST
    g_cmd_dbg_ship = ship; g_cmd_dbg_payload = pl; g_cmd_dbg_target = station;
    g_cmd_dbg_wrote = cmd;
    int32_t back = 0; if (mem_read_i32(pl + x3::off::SHIP_CMD_ID, &back)) g_cmd_dbg_readback = back & 0xFFFF;
    snprintf(buf, sizeof buf, "ok dock ship=0x%08x st=0x%08x cmd=%d bay=%d notify=%d dmode=%d dest=%d,%d,%d",
             ship, station, cmd, bay, notify, destmode, dest[0], dest[1], dest[2]);
    return buf;
}

// Debug-harness entry: `dock <station> [cmd=4] [bay=-1] [notify=0] [destmode=0]` -> dock PLAYER ship.
static std::string overlay_issue_dock(uint32_t station, int cmd, int bay, int notify, int destmode)
{
    return player_dock(player_ship_addr(), station, cmd, bay, notify, destmode);
}

// Fly the PLAYER ship THROUGH a gate. A gate jump is PROXIMITY-triggered (no bay tunnel like dock),
// so unlike "fly to gate" (which stops at the ~2.9km object-arrival standoff) we issue a MOVE-TO-
// POSITION (cmd 0x19, no object arrival) to a point `overshoot_km` PAST the gate along the ship->gate
// line — the ship flies straight through the gate ring, tripping the jump mid-flight. Crash-safe.
static std::string player_jumpgate(uint32_t ship, uint32_t gate, int overshoot_km)
{
    mem_open_rw();
    char buf[192];
    if (!ship || !gate) return "err no ship/gate";
    int32_t cls = 0, plw = 0;
    if (!mem_read_i32(ship + x3::off::OBJ_TYPE, &cls) || (int16_t)(cls & 0xFFFF) != x3::off::TYPE_SHIP)
        return "err ship not class 7";
    if (!mem_read_i32(ship + x3::off::OBJ_PAYLOAD, &plw) || (uint32_t)plw < 0x08000000u)
        return "err ship payload";
    int32_t gtf = 0, stf = 0, gp[3] = {0,0,0}, sp[3] = {0,0,0};
    if (mem_read_i32(gate + x3::off::OBJ_TRANSFORM, &gtf) && gtf) {
        mem_read_i32((uint32_t)gtf + x3::off::TF_X, &gp[0]);
        mem_read_i32((uint32_t)gtf + x3::off::TF_Y, &gp[1]);
        mem_read_i32((uint32_t)gtf + x3::off::TF_Z, &gp[2]);
    }
    if (mem_read_i32(ship + x3::off::OBJ_TRANSFORM, &stf) && stf) {
        mem_read_i32((uint32_t)stf + x3::off::TF_X, &sp[0]);
        mem_read_i32((uint32_t)stf + x3::off::TF_Y, &sp[1]);
        mem_read_i32((uint32_t)stf + x3::off::TF_Z, &sp[2]);
    }
    double dx = (double)gp[0]-sp[0], dy = (double)gp[1]-sp[1], dz = (double)gp[2]-sp[2];
    double len = sqrt(dx*dx + dy*dy + dz*dz); if (len < 1) len = 1;
    double over = (double)overshoot_km * 500000.0;                    // 1km = 500000 units
    int32_t dest[3] = { (int32_t)(gp[0] + dx/len*over),
                        (int32_t)(gp[1] + dy/len*over),
                        (int32_t)(gp[2] + dz/len*over) };
    uint32_t pl = (uint32_t)plw;
    int32_t rc = 0, now = 0;
    if (mem_read_i32(x3::off::RENDER_CONFIG, &rc) && rc)
        mem_read_i32((uint32_t)rc + x3::off::RENDER_CFG_TIME, &now);
    mem_write_u32(pl + x3::off::SHIP_CMD_TARGET,   0);                 // position move, no object target
    mem_write_u32(pl + x3::off::SHIP_CMD_DEST + 0, (uint32_t)dest[0]);
    mem_write_u32(pl + x3::off::SHIP_CMD_DEST + 4, (uint32_t)dest[1]);
    mem_write_u32(pl + x3::off::SHIP_CMD_DEST + 8, (uint32_t)dest[2]);
    mem_write_u32(pl + x3::off::SHIP_CMD_BAY,    (uint32_t)-1);
    mem_write_u32(pl + x3::off::SHIP_CMD_FAMILY, 0);
    mem_write_u32(pl + x3::off::SHIP_CMD_STARTTIME, (uint32_t)now);
    mem_write_u32(ship + x3::off::SHIP_NOTIFY_OBJ, 0);
    mem_write_u32(ship + x3::off::SHIP_NOTIFY_VAL, 0);
    { int32_t fl = 0; if (mem_read_i32(ship + x3::off::OBJ_FLAGS, &fl))
          mem_write_u32(ship + x3::off::OBJ_FLAGS, (uint32_t)fl | x3::off::AUTOPILOT_ENGAGED); }
    mem_write_u16(pl + x3::off::SHIP_CMD_ID, (uint16_t)x3::off::CMD_MOVE_POS);   // cmd 0x19, LAST
    snprintf(buf, sizeof buf, "ok jumpgate ship=0x%08x gate=0x%08x over=%dkm dest=%d,%d,%d",
             ship, gate, overshoot_km, dest[0], dest[1], dest[2]);
    return buf;
}

// Debug-harness entry: `jumpgate <gate> [overshoot_km=4]` -> fly the PLAYER ship through the gate.
static std::string overlay_jumpgate(uint32_t gate, int overshoot_km)
{
    return player_jumpgate(player_ship_addr(), gate, overshoot_km <= 0 ? 4 : overshoot_km);
}

// Scene id of an object (the key scene_lookup_object_by_id resolves) = obj+0x08 (the live "id=").
// (obj+0x28 reads 0 live — the savegame-serializer field the RE pass cited is not the runtime id.)
static int32_t obj_scene_id(uint32_t obj)
{
    int32_t id = 0; mem_read_i32(obj + x3::off::OBJ_ID, &id); return id;
}

// THE REAL AUTOPILOT: call the engine's command-ENGAGE `msci_native_operator_dispatch`@0x08116850 on
// the player ship. Unlike struct-poke (one straight leg, no avoidance), the engage builds the engine's
// own multi-leg nav path WITH collision-avoidance / dock-tunnel / gate-corridor. Operands = a flat VM
// buffer of 5-byte slots [tag byte][int32 LE]: slot0=ship scene-id, slot1=target scene-id, slot3=bay.
// op 9 = fly-to-with-avoidance (cmd5), op 10 = DOCK (cmd4, sets bay context safely), op16/30 = move.
// p1=0x7FFFFFFF (>=0x20) and p2=&{,,0} make the completion-notify a no-op. Then open the executor gate
// (bit19; optionally clear bit9 manual-direct). Crash-safe vs the bare dock helper — op-10 self-guards.
static std::string player_cmdengage(uint32_t ship, uint32_t target, int op, int argc, int bay, int clear200)
{
    mem_open_rw();
    char buf[224];
    if (!ship || !target) return "err no ship/target";
    int32_t cls = 0;
    if (!mem_read_i32(ship + x3::off::OBJ_TYPE, &cls) || (int16_t)(cls & 0xFFFF) != x3::off::TYPE_SHIP)
        return "err ship not class 7";
    int32_t shipId = obj_scene_id(ship), tgtId = obj_scene_id(target);
    uint8_t args[32]; for (int i = 0; i < 32; ++i) args[i] = 0;
    auto put = [&](int k, uint8_t tag, int32_t v) {            // slot k: tag@5k, int32 LE @5k+1
        args[5*k] = tag;
        args[5*k+1] = (uint8_t)v;        args[5*k+2] = (uint8_t)(v >> 8);
        args[5*k+3] = (uint8_t)(v >> 16); args[5*k+4] = (uint8_t)(v >> 24);
    };
    put(0, 4, shipId);                                          // slot0 = acting SHIP
    put(1, 4, tgtId);                                           // slot1 = TARGET object
    if (argc > 2) put(2, 4, bay);                               // slot2 = dock BAY (handler reads args[2])
    if (argc > 3) put(3, 4, (op == 9 || op == 10) ? (int)x3::off::CMD_FAMILY_DOCK : -1);  // slot3 = FAMILY (DOCK=9)
    int32_t p2buf[3] = { 0, 0, 0 };                            // *(p2+8)=0 -> ship+0x7C=0, notify off
    typedef int (__attribute__((cdecl)) *dispatch_t)(void*, void*, int, int, void*);
    int ret = ((dispatch_t)0x08116850u)((void*)0x7FFFFFFF, (void*)p2buf, op, argc, args);
    { int32_t fl = 0; if (mem_read_i32(ship + x3::off::OBJ_FLAGS, &fl)) {   // open executor gate
          uint32_t nf = (uint32_t)fl | x3::off::AUTOPILOT_ENGAGED;          // bit19
          if (clear200) nf &= ~0x200u;                                      // bit9 manual-direct
          mem_write_u32(ship + x3::off::OBJ_FLAGS, nf); } }
    int32_t plw = 0, backcmd = -1;
    if (mem_read_i32(ship + x3::off::OBJ_PAYLOAD, &plw) && (uint32_t)plw >= 0x08000000u)
        mem_read_i32((uint32_t)plw + x3::off::SHIP_CMD_ID, &backcmd);
    snprintf(buf, sizeof buf,
             "ok cmdengage op=%d argc=%d ship=0x%08x(id=%d) tgt=0x%08x(id=%d) bay=%d ret=%d -> cmd(C4)=%d",
             op, argc, ship, shipId, target, tgtId, bay, ret, backcmd & 0xFFFF);
    return buf;
}
static std::string overlay_cmdengage(uint32_t target, int op, int argc, int bay, int clear200)
{
    return player_cmdengage(player_ship_addr(), target, op, argc, bay, clear200);
}

// Per-frame autopilot DRIVER: the executor only processes a ship while (obj+0x40 & 0x8000200)==0,
// but physics re-sets bit27 (0x8000000 idle) on each leg-arrival — far faster than a 1Hz socket FSM
// can clear it. So clear it EVERY FRAME here (from the SwapWindow hook) to keep the executor advancing
// the player ship's command like an NPC. g_drive_rearm>0 also re-arms cmd if the executor completes it.
static volatile bool g_drive_on    = false;
static volatile int  g_drive_rearm = 0;     // cmd to PIN every frame (prevents executor completing+abandoning)
static volatile int  g_drive_bay   = -2;    // bay to PIN (-2 = don't pin)
// Per-frame DOCK driver: re-issue SA subcmd op (10=FlyBeforeContainer approach, 9=FlyIntoContainer
// entry) on the player every frame toward g_dock_drive_target, the way an NPC's command script does.
static volatile uint32_t g_dock_drive_target = 0;   // station addr, 0 = off
static volatile int      g_dock_drive_op     = 10;
static volatile int      g_dock_drive_bay    = 0;   // bay arg (slot2); 0 approaches, -1 overshoots
static std::string player_cmdengage(uint32_t ship, uint32_t target, int op, int argc, int bay, int clear200);  // fwd
static void drive_tick(void)
{
    if (!g_drive_on && !g_dock_drive_target) return;
    int32_t scene = 0, ship = 0;
    if (!mem_read_i32(x3::off::SCENE_GLOBAL, &scene) || !scene) return;
    if (!mem_read_i32((uint32_t)scene + x3::off::SCENE_PLAYER_ANCHOR, &ship) || !ship) return;
    if (g_dock_drive_target) {                       // drive the dock op every frame (op-10 opens gate + sets cmd)
        player_cmdengage((uint32_t)ship, (uint32_t)g_dock_drive_target, g_dock_drive_op,
                         (g_dock_drive_op == 9) ? 1 : 4, g_dock_drive_bay, 1);   // op-9 entry takes argc=1 (NPC-verified)
        if (!g_drive_on) return;
    }
    int32_t fl = 0;
    if (mem_read_i32((uint32_t)ship + x3::off::OBJ_FLAGS, &fl))
        mem_write_u32((uint32_t)ship + x3::off::OBJ_FLAGS,
                      ((uint32_t)fl | x3::off::AUTOPILOT_ENGAGED) & ~0x08000200u);   // bit19 set; bit27+bit9 clear
    int32_t plw = 0;
    if (mem_read_i32((uint32_t)ship + x3::off::OBJ_PAYLOAD, &plw) && (uint32_t)plw >= 0x08000000u) {
        if (g_drive_bay > -2) mem_write_u32((uint32_t)plw + x3::off::SHIP_CMD_BAY, (uint32_t)g_drive_bay);
        if (g_drive_rearm > 0) mem_write_u16((uint32_t)plw + x3::off::SHIP_CMD_ID, (uint16_t)g_drive_rearm);
    }
}
static std::string overlay_drive(int on, int rearm)
{
    mem_open_rw();
    g_drive_on = (on != 0);
    g_drive_rearm = rearm;
    g_drive_bay = (rearm > 0) ? 0 : -2;     // when pinning a dock cmd, also pin bay 0 (keep dock context)
    char b[96]; snprintf(b, sizeof b, "ok drive on=%d pin_cmd=%d pin_bay=%d",
                         (int)g_drive_on, g_drive_rearm, g_drive_bay); return b;
}
// Enable/disable the per-frame DOCK driver: re-issue SA subcmd `op` (10=approach, 9=enter) on the
// player toward `station` every frame. on=0 disables. This is the working injected auto-dock.
static std::string overlay_dockdrive(uint32_t station, int op, int bay, int on)
{
    mem_open_rw();
    g_dock_drive_op     = (op > 0) ? op : 10;
    g_dock_drive_bay    = bay;
    g_dock_drive_target = (on != 0) ? station : 0;
    char b[112]; snprintf(b, sizeof b, "ok dockdrive target=0x%08x op=%d bay=%d on=%d",
                          (unsigned)g_dock_drive_target, g_dock_drive_op, g_dock_drive_bay, on != 0); return b;
}
// Call a __cdecl void(uint) engine fn (e.g. input_dispatch_key_script@0x08053690) with one arg —
// the clean injected-keypress path: callfn1 0x08053690 0x1064 = Shift+D (Dock At) as the player.
static std::string overlay_callfn1(uint32_t addr, uint32_t arg)
{
    mem_open_rw();
    typedef void (__attribute__((cdecl)) *fn1_t)(unsigned);
    ((fn1_t)(uintptr_t)addr)(arg);
    int32_t s7c = -1, c4 = -1; uint32_t P = player_ship_addr(), plw = 0; (void)plw;
    if (P) { mem_read_i32((uint32_t)P + 0x7C, &s7c);
             int32_t v = 0; if (mem_read_i32((uint32_t)P + 0x50, &v) && (uint32_t)v >= 0x08000000u)
                 mem_read_i32((uint32_t)v + 0xC4, &c4); }
    char b[128]; snprintf(b, sizeof b, "ok callfn1 addr=0x%08x arg=0x%x -> obj+0x7C=0x%08x cmd(C4)=%d",
                          addr, arg, (uint32_t)s7c, c4 & 0xFFFF); return b;
}
// Call a __cdecl int(uint,uint,uint) engine fn with three args. Built to test the undock/launch
// primitive object_set_container_relink@0x080f31c0(obj, container, bay): container=sector physically
// relinks the docked player back into space. Reports the player's container (+0x54), command-task
// handle (+0x7C), and the game-session SCREEN state — flight flag *(g_game_session+0x4bc) (==1 in
// flight) and active scriptKey *(g_game_session+0x4c0) — so we can see whether the relink also flips
// the game into flight (which the input-dispatch dock path requires).
static std::string overlay_callfn3(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2)
{
    mem_open_rw();
    typedef int (__attribute__((cdecl)) *fn3_t)(unsigned, unsigned, unsigned);
    int ret = ((fn3_t)(uintptr_t)addr)(a0, a1, a2);
    uint32_t P = player_ship_addr();
    int32_t c54 = 0, s7c = 0, flight = -1, scrkey = 0, gsv = 0;
    if (P) { mem_read_i32((uint32_t)P + 0x54, &c54); mem_read_i32((uint32_t)P + 0x7C, &s7c); }
    if (mem_read_i32(0x0829f6b4, &gsv) && (uint32_t)gsv >= 0x08000000u) {   // g_game_session
        mem_read_i32((uint32_t)gsv + 0x4bc, &flight);
        mem_read_i32((uint32_t)gsv + 0x4c0, &scrkey);
    }
    char b[208]; snprintf(b, sizeof b,
        "ok callfn3 0x%08x(0x%x,0x%x,0x%x) ret=%d -> container=0x%08x obj+0x7C=0x%08x flight(4bc)=%d scrkey(4c0)=0x%08x",
        addr, a0, a1, a2, ret, (uint32_t)c54, (uint32_t)s7c, flight, (uint32_t)scrkey);
    return b;
}
// General SA-native operator call: dispatch(0x7FFFFFFF, &{0,0,0}, subcmd, argc, cells) with up to 4
// raw int cells (tag 4 = int/object-id). Unlike player_cmdengage it does NOT require a target and does
// NOT open the executor gate — for probing player-state ops like SA_SetPlayerLand(shipId,0)=UNLAND
// (subcmd 0x119, cell0=player scene-id, cell1=0). Object cells are scene-ids (engine resolves via
// scene_lookup_object_by_id). Reports player container/cmd/obj+0x7C after.
static std::string overlay_sacall(int subcmd, int argc, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3)
{
    mem_open_rw();
    uint8_t args[32]; for (int i = 0; i < 32; ++i) args[i] = 0;
    uint32_t cells[4] = { c0, c1, c2, c3 };
    for (int k = 0; k < 4; ++k) { int32_t v = (int32_t)cells[k];
        args[5*k] = 4; args[5*k+1] = (uint8_t)v; args[5*k+2] = (uint8_t)(v >> 8);
        args[5*k+3] = (uint8_t)(v >> 16); args[5*k+4] = (uint8_t)(v >> 24); }
    int32_t p2buf[3] = { 0, 0, 0 };
    typedef int (__attribute__((cdecl)) *dispatch_t)(void*, void*, int, int, void*);
    int ret = ((dispatch_t)0x08116850u)((void*)0x7FFFFFFF, (void*)p2buf, subcmd, argc, args);
    uint32_t P = player_ship_addr(); int32_t c54 = 0, s7c = 0, c4 = -1, plw = 0;
    if (P) { mem_read_i32((uint32_t)P + 0x54, &c54); mem_read_i32((uint32_t)P + 0x7C, &s7c);
             if (mem_read_i32((uint32_t)P + 0x50, &plw) && (uint32_t)plw >= 0x08000000u)
                 mem_read_i32((uint32_t)plw + 0xC4, &c4); }
    char b[208]; snprintf(b, sizeof b,
        "ok sacall sub=0x%x argc=%d cells=[0x%x,0x%x,0x%x,0x%x] ret=%d -> container=0x%08x obj+0x7C=0x%08x cmd=%d",
        subcmd, argc, c0, c1, c2, c3, ret, (uint32_t)c54, (uint32_t)s7c, c4 & 0xFFFF);
    return b;
}
// Raw u32 write probe — write `val` to absolute `addr` and read it back. For testing which game-state
// fields gate behavior (e.g. the game-session docked-screen context at gs+0x488 / gs+0x494).
static std::string overlay_poke(uint32_t addr, uint32_t val)
{
    mem_open_rw();
    int32_t before = 0; mem_read_i32(addr, &before);
    mem_write_u32(addr, val);
    int32_t after = 0; mem_read_i32(addr, &after);
    char b[112]; snprintf(b, sizeof b, "ok poke 0x%08x: 0x%08x -> 0x%08x (wanted 0x%08x)",
                          addr, (uint32_t)before, (uint32_t)after, val);
    return b;
}
// ISOLATED Input-script trace: arm the SA-native dispatch breakpoint, fire input_dispatch_key_script(cmdId)
// SYNCHRONOUSLY (between frames, on this thread), then disarm+dump — so the captured SA-op sequence is
// ONLY what the "Input" script runs for this key (no per-frame pollution). a2 column = SA subcmd. Lets us
// diff the docked vs flight Input-script path to find the docked dock-gate.
static std::string overlay_tracedock(uint32_t cmdId, uint32_t traceAddr)
{
    if (!traceAddr) traceAddr = 0x08116850u;                 // default: SA-native dispatch entry
    overlay_exectrace(traceAddr, 0);                         // arm BP at the chosen probe point
    typedef void (__attribute__((cdecl)) *fn1_t)(unsigned);
    ((fn1_t)(uintptr_t)0x08053690u)(cmdId);                  // run the Input script for this key, synchronously
    return overlay_exectrace(0, 0);                          // disarm + dump the captured sequence
}
// ROUTE B — hand-build an MsciTask running the dock orchestrator `!ship.cmd.movestation.std` bound to the
// player, replicating what the menu's START-command does (the script_resolve_and_alloc_task fabrication),
// because run_script can't resolve a command-script by NAME (its name isn't a class method). Staged so each
// safe step is verifiable before the crash-prone scheduler link. Recipe live-verified — see memory
// x3-player-dock-drive (Route B). Run between frames (debug pump). Re-resolves the heap-relocated entry-ip.
//   stage 0: resolve orchestrator instance + entry-ip/localcount (read-only)
//   stage 1: + alloc task + init the choke's fields (no arg push, no link)
//   stage 2: + push station arg on the value stack (no link)
//   stage 3: + bind obj+0x7C/0x78 + link into scheduler (the live spawn)

// Resolve the `!ship.cmd.movestation.std` orchestrator INSTANCE stably (its ptr + per-run key relocate
// every launch; the entry OFFSET 0xd952b does NOT). Walk the VM instance-map (root+0x12d0: bucketArr@+0,
// cap@+4; node next@+0, value=instance@+8) and match the instance whose private scope[0] (inst+8:
// count@+0x28, methodArr@+0x2c stride0x10) holds a method with rec[0]==entryOff. task+0x3c must be THIS
// instance (command-scripts live only in their own private scope — playerKey+name fails the choke bsearch).
static uint32_t resolve_orch_instance(uint32_t root, uint32_t entryOff)
{
    static uint32_t s_cache = 0;
    if (s_cache >= 0x08000000u) {                       // re-validate cache (instance stable within a run)
        int32_t sc0 = 0, arr = 0, r0 = 0;
        if (mem_read_i32(s_cache + 8, &sc0) && (uint32_t)sc0 >= 0x08000000u &&
            mem_read_i32((uint32_t)sc0 + 0x2c, &arr) && (uint32_t)arr >= 0x08000000u &&
            mem_read_i32((uint32_t)arr, &r0) && (uint32_t)r0 == entryOff)
            return s_cache;
        s_cache = 0;
    }
    int32_t mapStruct = 0, bucketArr = 0, cap = 0;
    if (!mem_read_i32(root + 0x12d0, &mapStruct) || (uint32_t)mapStruct < 0x08000000u) return 0;
    if (!mem_read_i32((uint32_t)mapStruct + 0, &bucketArr) || (uint32_t)bucketArr < 0x08000000u) return 0;
    if (!mem_read_i32((uint32_t)mapStruct + 4, &cap) || cap <= 0 || (uint32_t)cap > 0x200000u) return 0;
    for (uint32_t b = 0; b < (uint32_t)cap; b++) {
        int32_t node = 0;
        if (!mem_read_i32((uint32_t)bucketArr + b * 4, &node)) continue;
        int g = 0;
        while ((uint32_t)node >= 0x08000000u && g++ < 100000) {
            int32_t inst = 0; mem_read_i32((uint32_t)node + 8, &inst);
            if ((uint32_t)inst >= 0x08000000u) {
                int32_t sc0 = 0; mem_read_i32((uint32_t)inst + 8, &sc0);   // scope[0] (private)
                if ((uint32_t)sc0 >= 0x08000000u) {
                    int32_t cnt = 0, arr = 0;
                    mem_read_i32((uint32_t)sc0 + 0x28, &cnt);
                    mem_read_i32((uint32_t)sc0 + 0x2c, &arr);
                    if ((uint32_t)arr >= 0x08000000u && cnt > 0 && cnt < 100000) {
                        for (int i = 0; i < cnt; i++) {
                            int32_t r0 = 0;
                            if (mem_read_i32((uint32_t)arr + (uint32_t)i * 0x10, &r0) && (uint32_t)r0 == entryOff) {
                                s_cache = (uint32_t)inst; return (uint32_t)inst;
                            }
                        }
                    }
                }
            }
            int32_t nxt = 0; mem_read_i32((uint32_t)node + 0, &nxt); node = nxt;
        }
    }
    return 0;
}
// Native VM dock module (workflow design 2026-07-02): direct-spawn a task running our OWN byte-op
// program that fires SA-native op-0x82 subcmd 0x0c on group 8 (ship-command class) = the crash-free
// deferred-bay DOCKAT setter (0x0811c774): writes payload+0xc4=4, +0x98=station, +0xbc=-1; the
// per-frame executor then selects the bay + flies + docks. arg0=ship(actor), arg1=station, arg2..4=0.
// No [THIS] bind, no orchestrator, no bay math -> avoids the dock_compute_approach_for_bay crash.
// group=8 / subcmd=0x0c LIVE-verified; push ops 0x01(int0)/0x05(i8)/0x07(i32)/0x70(END)/0x82(SA) jumptable-verified.
//   stage 0 = mechanism proof (push 0; END).  stage 1 = DOCKAT.  stage 2 = sub-0x09 tunnel prefix + DOCKAT (R1 stall fallback).
static std::string overlay_natdock(uint32_t station, int stage)
{
    mem_open_rw();
    char b[320];
    int32_t ROOT = 0;
    if (!mem_read_i32(0x08308880, &ROOT) || (uint32_t)ROOT < 0x08000000u) return "err no root";
    uint32_t player = player_ship_addr();
    if (!player) return "err no player";
    if (stage >= 1 && !station) return "err no station";

    // stage 3 = DIRECT FIELD POKE: replicate subcmd-0x0c handler 0x0811c774, which is PURE field writes,
    // so we skip the VM task entirely (the alloc+link spawn path in stages 0-2 crashes — link_into_scheduler
    // needs resolver-set task state the bare alloc doesn't provide). +0xbc=-1 (deferred bay) is the field the
    // old overshooting struct-poke missed; the per-frame executor selects the bay + flies + docks.
    if (stage == 3) {
        int32_t pay3 = 0;
        if (!mem_read_i32(player + 0x50, &pay3) || (uint32_t)pay3 < 0x08000000u) return "err no payload";
        mem_write_u16((uint32_t)pay3 + 0xc4, 4);            // DOCKAT command id
        mem_write_u32((uint32_t)pay3 + 0x98, station);      // target station OBJECT ptr
        mem_write_u32((uint32_t)pay3 + 0xbc, 0xffffffffu);  // bay = -1 (deferred)
        mem_write_u32((uint32_t)pay3 + 0xac, 0);            // params
        mem_write_u32((uint32_t)pay3 + 0xb0, 0);
        mem_write_u32((uint32_t)pay3 + 0xb4, 0);
        int32_t c4 = 0, c98 = 0, cbc = 0, o40 = 0, sid3 = 0;
        mem_read_i32(station + 0x08, &sid3);
        mem_read_i32((uint32_t)pay3 + 0xc4, &c4); mem_read_i32((uint32_t)pay3 + 0x98, &c98);
        mem_read_i32((uint32_t)pay3 + 0xbc, &cbc); mem_read_i32(player + 0x40, &o40);
        snprintf(b, sizeof b, "ok natdock stage3 FIELD-POKE C4=0x%x(want 4) target=0x%08x(station 0x%08x sid 0x%x) bay=0x%x(want ffffffff) obj40=0x%08x",
                 c4 & 0xFFFF, (uint32_t)c98, station, (uint32_t)sid3, (uint32_t)cbc, (uint32_t)o40);
        return b;
    }

    int32_t plSid = 0, stSid = 0, codebase = 0, plKey = 0;
    mem_read_i32(player + 0x08, &plSid);
    if (station) mem_read_i32(station + 0x08, &stSid);
    mem_read_i32((uint32_t)ROOT + 0x08, &codebase);
    mem_read_i32(player + 0x90, &plKey);
    typedef uint32_t (__attribute__((cdecl)) *lookup_t)(uint32_t, uint32_t);
    uint32_t plInst = ((lookup_t)0x0809dfc0u)((uint32_t)ROOT, (uint32_t)plKey);
    if (plInst < 0x08000000u) { snprintf(b, sizeof b, "err plInst=0x%x key=0x%x", plInst, (uint32_t)plKey); return b; }

    // Build the byte-op program in a PERSISTENT static buffer (interpreter + scheduler may re-read it).
    static uint8_t PROG[80];
    int n = 0;
    auto pI32 = [&](int32_t v) { PROG[n++] = 0x07; memcpy(PROG + n, &v, 4); n += 4; };  // push int32
    if (stage == 0) {
        PROG[n++] = 0x01; PROG[n++] = 0x70;                    // push int 0; END-TASK  (mechanism proof)
    } else if (stage == 4) {
        // op-0x85 call-on-object -> StartExecuteCommand([THIS]=player, arg0=cmd7 DOCKAT, arg1=station).
        // op-0x85 pops the object (TOS) + RETARGETS [THIS], and reads the cell BELOW the object as argc.
        // CORRECT frame (agent RE) from TOS: [object=player][argc=2][arg0=cmd][arg1=station]. Physical push
        // order (deepest first): station, cmd7, argc2, player. (My earlier bug omitted the argc cell -> garbage
        // target -> orchestrator crashed in dock_compute_approach_for_bay ~6s later. NOT a context wall.)
        pI32(stSid);                                           // arg1 = station scene-id (deepest)
        PROG[n++] = 0x05; PROG[n++] = 0x07;                    // push i8 7 -> arg0 = cmd COMMAND_DOCKAT
        PROG[n++] = 0x05; PROG[n++] = 0x02;                    // push i8 2 -> argc
        PROG[n++] = 0x07; memcpy(PROG + n, &plKey, 4); n += 4; // push player instance key -> object (TOS)
        int32_t nsid = 0x4d7d;                                 // StartExecuteCommand nameStrId
        PROG[n++] = 0x85; memcpy(PROG + n, &nsid, 4); n += 4;  // op-0x85
        PROG[n++] = 0x70;                                      // END
    } else {
        if (stage == 2) {                                      // R1 fallback: build approach tunnel first (sub-0x09, loops REAL bays -> crash-safe)
            pI32(stSid); pI32(plSid);                          // arg1=station, arg0=ship
            PROG[n++] = 0x05; PROG[n++] = 0x02;                // push i8 2 (argc)
            PROG[n++] = 0x82; PROG[n++] = 0x08; PROG[n++] = 0x00; PROG[n++] = 0x09; PROG[n++] = 0x00;  // op0x82 group8 sub0x09
        }
        PROG[n++] = 0x01; PROG[n++] = 0x01; PROG[n++] = 0x01;  // push 0,0,0 -> params arg2,3,4
        pI32(stSid); pI32(plSid);                              // arg1=station, arg0=ship(actor)
        PROG[n++] = 0x05; PROG[n++] = 0x05;                    // push i8 5 (argc)
        PROG[n++] = 0x82; PROG[n++] = 0x08; PROG[n++] = 0x00; PROG[n++] = 0x0c; PROG[n++] = 0x00;  // op0x82 group8 sub0x0c DOCKAT
        PROG[n++] = 0x70;                                      // END-TASK
    }

    int32_t o7c0 = 0; mem_read_i32(player + 0x7c, &o7c0);
    if (stage >= 1 && o7c0 != 0) { snprintf(b, sizeof b, "err player busy obj+0x7c=0x%08x — clear cmd first", (uint32_t)o7c0); return b; }

    typedef uint32_t (__attribute__((cdecl)) *alloc_t)(uint32_t, int);
    uint32_t task = ((alloc_t)0x080a10e0u)((uint32_t)ROOT, 0x20);   // 32-cell value stack, plenty
    if (task < 0x08000000u) { snprintf(b, sizeof b, "err alloc task=0x%x", task); return b; }
    mem_write_u32(task + 0x30, 0);                              // ctx=0 => no completion-notify (fire-and-forget, safe)
    mem_write_u32(task + 0x34, 0);                             // caller
    mem_write_u16(task + 0x38, 1);                             // mode
    mem_write_u32(task + 0x3c, plInst);                       // instance/scope
    uint32_t ipoff = (uint32_t)(uintptr_t)&PROG[0] - (uint32_t)codebase;  // code_base + ip round-trips to &PROG (32-bit)
    mem_write_u32(task + 0x1c, ipoff);

    // script_task_link_into_scheduler is __regparm3 (task in EAX) + runs slice-1 synchronously. The old
    // asm `call *%1` dereferenced the TASK operand (call *task = garbage-as-code) -> crash; call it as a
    // proper regparm(3) function so `task` lands in EAX.
    typedef int (__attribute__((regparm(3))) *link_t)(uint32_t);
    int32_t ret = ((link_t)0x0809e1d0u)(task);

    int32_t o7c = 0, c4 = 0, c98 = 0, cbc = 0, pay = 0;
    mem_read_i32(player + 0x7c, &o7c);
    if (mem_read_i32(player + 0x50, &pay) && (uint32_t)pay >= 0x08000000u) {
        mem_read_i32((uint32_t)pay + 0xc4, &c4);
        mem_read_i32((uint32_t)pay + 0x98, &c98);
        mem_read_i32((uint32_t)pay + 0xbc, &cbc);
    }
    if (stage == 0) { snprintf(b, sizeof b, "ok natdock stage0 MECH-PROOF task=0x%08x ret=0x%x ipoff=0x%08x (push0;END ran — game alive)", task, ret, ipoff); return b; }
    snprintf(b, sizeof b, "ok natdock stage%d group8/sub0xc ret=0x%x -> obj7C=0x%08x C4=0x%x(want 4) target=0x%08x(want 0x%x) bay=0x%x(want ffffffff)",
             stage, ret, (uint32_t)o7c, c4 & 0xFFFF, (uint32_t)c98, (uint32_t)stSid, (uint32_t)cbc);
    return b;
}

static std::string overlay_spawnorch(uint32_t station, int stage)
{
    mem_open_rw();
    char b[420];
    int32_t ROOT = 0, SCENE = 0, ctx = 0;
    if (!mem_read_i32(0x08308880, &ROOT)  || (uint32_t)ROOT  < 0x08000000u) return "err no root";
    if (!mem_read_i32(0x0837a0d0, &SCENE) || (uint32_t)SCENE < 0x08000000u) return "err no scene";
    mem_read_i32((uint32_t)SCENE + 0x60, &ctx);
    uint32_t player = player_ship_addr();
    if (!player || !station) return "err no player/station";
    int32_t stSid = 0, plSid = 0; mem_read_i32(player + 0x08, &plSid); mem_read_i32(station + 0x08, &stSid);

    // (a) PLAYER instance (= task+0x3c, the [THIS]/context) + orchestrator entry-ip via stable code OFFSET.
    // The orchestrator's sequential instance-id shifts per run, so resolve the PLAYER instance (its scriptKey
    // *(player+0x90) is stable per ship) and reach the orchestrator's bytecode by its global-code-segment
    // OFFSET (base = *(root+8)) — the offset is run-stable even though the base heap-relocates each launch.
    typedef uint32_t (__attribute__((cdecl)) *lookup_t)(uint32_t, uint32_t);
    int32_t plKey = 0; mem_read_i32(player + 0x90, &plKey);
    uint32_t plInst = ((lookup_t)0x0809dfc0u)((uint32_t)ROOT, (uint32_t)plKey);
    if (plInst < 0x08000000u) { snprintf(b, sizeof b, "err player inst=0x%x key=0x%x", plInst, (uint32_t)plKey); return b; }
    // station objref box for $object (arg1): its MSCI instance (box[0]==stKey==raw *(station+0x90)); deref does
    // key=~box[0] -> handle-map -> station object. Fallback = a static box if the station has no instance.
    int32_t stKey = 0; mem_read_i32(station + 0x90, &stKey);
    uint32_t stInst = ((lookup_t)0x0809dfc0u)((uint32_t)ROOT, (uint32_t)stKey);
    int32_t codebase = 0, trailing = 0;
    mem_read_i32((uint32_t)ROOT + 0x08, &codebase);
    mem_read_i32((uint32_t)plInst + 0x0c, &trailing);
    uint32_t entryOff = 0xd952bu;                        // !ship.cmd.movestation.std entry OFFSET into code segment
    uint32_t entryIp = (uint32_t)codebase + entryOff;   // absolute (report/verify only; task+0x1c takes the OFFSET)
    uint32_t orchInst = resolve_orch_instance((uint32_t)ROOT, entryOff);   // task+0x3c = the ORCHESTRATOR instance
    int cellCount = 10;                                  // locals(0)+7+vshint(3)
    int32_t gs2 = 0; mem_read_i32(0x0829f6b4, &gs2); if (gs2) mem_read_i32(gs2 + 0x418, &ctx);   // owner ctx (=9)
    if (stage <= 0) {
        int32_t orc0 = 0, os0 = 0, oarr = 0; if (orchInst >= 0x08000000u) { mem_read_i32(orchInst + 8, &os0);
            if ((uint32_t)os0 >= 0x08000000u && mem_read_i32((uint32_t)os0 + 0x2c, &oarr) && (uint32_t)oarr >= 0x08000000u) mem_read_i32((uint32_t)oarr, &orc0); }
        snprintf(b, sizeof b, "ok stage0 orchInst=0x%08x rec0=0x%x plInst=0x%08x plKey=0x%x codebase=0x%08x entryIp=0x%08x cellCount=%d ctx=0x%08x stSid=0x%x",
                 orchInst, (uint32_t)orc0, plInst, (uint32_t)plKey, (uint32_t)codebase, entryIp, cellCount, (uint32_t)ctx, (uint32_t)stSid);
        return b;
    }
    // guard: don't spawn over a live command task (leak/conflict)
    int32_t o7c0 = 0; mem_read_i32(player + 0x7c, &o7c0);
    if (o7c0 != 0) { snprintf(b, sizeof b, "err player busy (obj+0x7C=0x%08x) — clear command first", (uint32_t)o7c0); return b; }
    if (orchInst < 0x08000000u) { snprintf(b, sizeof b, "err orchestrator instance not found (map walk, entryOff=0x%x)", entryOff); return b; }

    // (b) alloc task (cdecl(root, cellCount)); (c) init the fields the choke sets
    typedef uint32_t (__attribute__((cdecl)) *alloc_t)(uint32_t, int);
    uint32_t task = ((alloc_t)0x080a10e0u)((uint32_t)ROOT, cellCount);
    if (task < 0x08000000u) { snprintf(b, sizeof b, "err alloc task=0x%x", task); return b; }
    mem_write_u32(task + 0x30, (uint32_t)ctx);     // owner
    mem_write_u32(task + 0x34, 0);                 // caller
    mem_write_u16(task + 0x38, 1);                 // mode (async)
    mem_write_u32(task + 0x3c, (stage == 8) ? plInst : orchInst);   // stage 8 = PLAYER-instance context (like native docking task)
    mem_write_u32(task + 0x1c, entryOff);          // task+0x1c = byte OFFSET into code (abs=codebase+ip), NOT absolute
    int32_t handle = 0, sb = 0, sp = 0;
    mem_read_i32(task + 0x08, &handle); mem_read_i32(task + 0x14, &sb); mem_read_i32(task + 0x18, &sp);
    if (stage <= 1) {
        snprintf(b, sizeof b, "ok stage1 task=0x%08x handle=0x%x stackbase=0x%08x sp=%d (alloc+fields done; no push/link)",
                 task, (uint32_t)handle, (uint32_t)sb, sp);
        return b;
    }

    // (d) BYTE-EXACT run_script_async@0x0809e270: push each arg as a tag-1 INT cell (OBJECT args = their
    // SCENE-ID integer *(obj+0x08), NOT a tag-10 objref), then sp-=1 and push a trailing tag-1 cell =
    // methodrec[0xc] (=localcount, 0 for movestation.std). cell = stack_base + (sp+i)*5.
    auto pushcell = [&](int32_t cellIdx, uint8_t tag, int32_t val) {
        uint8_t buf[5]; buf[0] = tag; memcpy(buf + 1, &val, 4);
        mem_write_bytes((uint32_t)(sb + cellIdx * 5), buf, 5);
    };
    if (stage == 15 || stage == 16) {
        // MIRROR run_script_async@0x0809e270 EXACTLY (decompiled 2026-07-02): push argc tag-1 cells at
        // cell (sp0+i) with sp0 UNCHANGED during the loop, then sp-=1 + trailing tag-1 = methodRec[0xc]
        // (localcount), then script_task_link_into_scheduler@0x0809e1d0 (adds tag-3 + tag-10 NULL, links,
        // RUNS first slice). [THIS]=arg0, $object=arg1 (both tag-1 scene-id ints). The old 8-cell frame
        // was decoded from the WRONG object (0xd952b in x3story.obj is unrelated base code). stage16 swaps
        // arg0/arg1 to A/B the actor/object slot.
        int32_t a0 = (stage == 16) ? stSid : plSid;   // arg0 = [THIS] (player ship scene-id)
        int32_t a1 = (stage == 16) ? plSid : stSid;   // arg1 = $object (station scene-id)
        pushcell(sp + 0, 1, a0);
        pushcell(sp + 1, 1, a1);
        int32_t sp1 = sp - 1; mem_write_u32(task + 0x18, (uint32_t)sp1);
        int32_t os = 0, oarr = 0, recC = 0;
        mem_read_i32((int32_t)orchInst + 8, &os);
        if ((uint32_t)os   >= 0x08000000u) mem_read_i32((uint32_t)os + 0x2c, &oarr);
        if ((uint32_t)oarr >= 0x08000000u) mem_read_i32((uint32_t)oarr + 0xc, &recC);
        pushcell(sp1, 1, recC);
        uint32_t fn = 0x0809e1d0u, ret = 0;   // script_task_link_into_scheduler (regparm; task in EAX)
        __asm__ __volatile__("call *%1" : "=a"(ret) : "a"(task), "r"(fn) : "ecx","edx","memory","cc");
        int32_t o7c=0,c4=0,c54=0,plw=0;
        mem_read_i32(player+0x7c,&o7c); mem_read_i32(player+0x54,&c54);
        if (mem_read_i32(player+0x50,&plw) && (uint32_t)plw>=0x08000000u) mem_read_i32((uint32_t)plw+0xc4,&c4);
        snprintf(b, sizeof b, "ok stage%d MIRROR-RSA a0=0x%x a1=0x%x sp0=%d ret=0x%x -> obj7C=0x%08x C4=%d cont=0x%08x",
                 stage, a0, a1, sp, ret, (uint32_t)o7c, c4 & 0xFFFF, (uint32_t)c54);
        return b;
    }
    // 8-cell caller frame decoded from movestation.std body (thunk: ENSURE_STACK 7 + 5x push-STACKvar w=8):
    // cell i (at stack_base + (sp-i)*5, i=0 shallow .. i=7 deepest) — i=7=[THIS]=ship sceneid, i=6=$object=station
    // sceneid, i=0..5 = tag-1 int 0 (params/slack). stage 12 swaps i6/i7 (the one uncertain [THIS]/$object slot).
    int32_t thisV = (stage == 12) ? stSid : plSid;
    int32_t objV  = (stage == 12) ? plSid : stSid;
    for (int i = 0; i <= 7; i++) {
        int32_t v = (i == 7) ? thisV : ((i == 6) ? objV : 0);
        pushcell(sp - i, 1, v);                   // cell at sb + (sp - i)*5
    }
    sp -= 8; mem_write_u32(task + 0x18, (uint32_t)sp);
    if (stage <= 2) {
        snprintf(b, sizeof b, "ok stage2 task=0x%08x plSid=0x%x stSid=0x%x sp=%d (8-cell caller frame, no link)",
                 task, (uint32_t)plSid, (uint32_t)stSid, sp);
        return b;
    }

    if (stage == 4) {   // ISOLATION: engine's OWN link (seeds [THIS]=null) — does the task then run/persist?
        mem_write_u32(player + 0x7c, (uint32_t)handle);
        mem_write_u32(player + 0x78, 1);
        uint32_t fn = 0x0809e1d0u, ret = 0;
        __asm__ __volatile__("call *%1" : "=a"(ret) : "a"(task), "r"(fn) : "ecx", "edx", "memory", "cc");
        int32_t o7c = 0; mem_read_i32(player + 0x7c, &o7c);
        snprintf(b, sizeof b, "ok stage4 ASMLINK task=0x%08x handle=0x%x ret=0x%x obj7C=0x%08x ([THIS]=null via engine link)",
                 task, (uint32_t)handle, ret, (uint32_t)o7c);
        return b;
    }
    // (e) the [THIS] box IS the player INSTANCE (GROUND-TRUTH 2026-06-30: native dock's bottom tag-10 cell box ==
    // task+0x3c == plInst; box[0]==plKey raw *(player+0x90), box[8]==classChain!=0). The deref does key=~box[0]
    // -> handle-map lookup -> player object. So seed the [THIS] cell with plInst (no hand-made box; my prior
    // ~plKey static box was INVERTED -> resolved wrong -> crash).
    // (f) replicate script_task_link_into_scheduler@0x0809e1d0 but seed the bottom tag-10 [THIS] cell with the
    // PLAYER instance instead of NULL (the launcher's NULL seed is what crashed the orchestrator), then kick.
    int32_t sp2 = sp - 1; mem_write_u32(task + 0x18, (uint32_t)sp2);                 // push tag-3 marker
    { uint8_t z3[5] = {3,0,0,0,0}; mem_write_bytes((uint32_t)((int32_t)sb + sp2*5), z3, 5); }
    sp2 -= 1; mem_write_u32(task + 0x18, (uint32_t)sp2);                             // push tag-10 [THIS]
    { uint8_t z0[5] = {0,0,0,0,0}; mem_write_bytes((uint32_t)((int32_t)sb + sp2*5), z0, 5); }  // zero -> no stale-destroy
    typedef void (__attribute__((cdecl)) *setobj_t)(void*, void*);                  // msci_cell_set_objref_tag10
    // stage13 = ABI-agent fix (2026-07-02): the bottom tag-10 cell IS the script [THIS]; the launcher seeds it
    // NULL and THAT is why the orchestrator ran but never acted on the player. Seed it = plInst (the PLAYER
    // instance box, exactly what the native dock's bottom cell holds — box[0]==plKey). stage3 keeps NULL baseline.
    uint32_t thisBox = (stage == 13) ? (uint32_t)plInst : 0u;
    ((setobj_t)0x080a9150u)((void*)(uintptr_t)(uint32_t)((int32_t)sb + sp2*5), (void*)(uintptr_t)thisBox);
    if (stage == 9) {   // value stack built (args + tag-3 + tag-10 [THIS]) but NOT scheduled/kicked — inspectable
        snprintf(b, sizeof b, "ok stage9 task=0x%08x sb=0x%08x sp=%d thisCell=0x%08x plInst=0x%08x (built, NOT scheduled)",
                 task, (uint32_t)sb, sp2, (uint32_t)((int32_t)sb + sp2*5), plInst);
        return b;
    }
    mem_write_u16(task + 0x3a, 1);   // linkage=1 (disasm: MOV word[EBX+0x3a],1) — THIS makes the scheduler RUN the task
    int32_t head = 0; mem_read_i32((uint32_t)ROOT + 0x12e0, &head);                  // ready-list prepend
    mem_write_u32(task + 0x04, (uint32_t)head);                                      // task->list_prev = head
    if ((uint32_t)head >= 0x08000000u) mem_write_u32((uint32_t)head + 0x00, task);  // head->hash_link_next = task
    mem_write_u32(task + 0x00, (uint32_t)ROOT + 0x12dc);                             // task->hash_link_next = sentinel
    mem_write_u32((uint32_t)ROOT + 0x12e0, task);                                    // head = task
    mem_write_u32((uint32_t)ROOT + 0x1434, task);                                    // cursor = task
    if (stage == 7) {   // verify all writes landed (no kick)
        int32_t l3a=0,t00=0,t04=0,t18=0,rh=0,rc=0,ip=0,pr=0;
        mem_read_i32(task+0x3a,&l3a); mem_read_i32(task+0x00,&t00); mem_read_i32(task+0x04,&t04);
        mem_read_i32(task+0x18,&t18); mem_read_i32(task+0x1c,&ip); mem_read_i32(task+0x3c,&pr);
        mem_read_i32((uint32_t)ROOT+0x12e0,&rh); mem_read_i32((uint32_t)ROOT+0x1434,&rc);
        snprintf(b, sizeof b, "ok stage7 task=0x%08x link3a=0x%x t00=0x%08x t04=0x%08x sp=%d ip=0x%x prog=0x%08x head=0x%08x cursor=0x%08x sentinel=0x%08x",
                 task, l3a & 0xffff, (uint32_t)t00, (uint32_t)t04, t18, (uint32_t)ip, (uint32_t)pr, (uint32_t)rh, (uint32_t)rc, (uint32_t)ROOT+0x12dc);
        return b;
    }
    mem_write_u32(player + 0x7c, (uint32_t)handle);                                  // bind command-task hooks
    mem_write_u32(player + 0x78, 1);
    if (stage == 10) overlay_exectrace(0x08116850u, 0);   // arm SA-dispatch trace AROUND the synchronous kick (isolate my task's ops)
    if (stage == 11) overlay_exectrace(0x080a215bu, 0);   // trace op-0x6e ENSURE-STACK handler (orchestrator's FIRST op) — did the task run at all?
    typedef void (__attribute__((cdecl)) *run_t)(uint32_t, uint32_t);               // KICK: msci_vm_run_task(root,task)
    ((run_t)0x080a1710u)((uint32_t)ROOT, task);
    if (stage == 10 || stage == 11) return overlay_exectrace(0, 0);   // disarm + dump ONLY the kick's traced ops
    int32_t o7c = 0, pl = 0, c4 = 0, c54 = 0;
    mem_read_i32(player + 0x7c, &o7c); mem_read_i32(player + 0x54, &c54);
    if (mem_read_i32(player + 0x50, &pl) && (uint32_t)pl >= 0x08000000u) mem_read_i32(pl + 0xc4, &c4);
    snprintf(b, sizeof b, "ok stage3 SEEDED(plInst=0x%08x)+LINKED task=0x%08x handle=0x%x -> obj+0x7C=0x%08x cmd(C4)=%d container=0x%08x",
             plInst, task, (uint32_t)handle, (uint32_t)o7c, c4 & 0xFFFF, (uint32_t)c54);
    return b;
}
// Set the player's HUD target to `target_addr` (mode lock) — same path as the overlay's
// select_target, so Shift+D=Dock-At then acts on it. Lets us target + dock fully from injection.
static std::string overlay_settarget(uint32_t target_addr)
{
    if (!target_addr) return "err no target";
    mem_open();
    x3::Mem m = make_engine_mem();
    uint32_t scene = 0, player = 0;
    if (!(m.u32(x3::off::SCENE_GLOBAL, scene) && scene &&
          m.u32(scene + x3::off::SCENE_PLAYER_ANCHOR, player) && player)) return "err no scene/player";
    uint32_t grav = x3::find_player_gravidar(m, player);
    if (!grav) return "err no gravidar";
    typedef void (__attribute__((cdecl)) *set_target_t)(void*, int16_t, void*);
    ((set_target_t)(uintptr_t)x3::off::GRAVIDAR_SET_TARGET_FN)(
        (void*)(uintptr_t)grav, x3::off::TARGET_MODE_LOCK, (void*)(uintptr_t)target_addr);
    uint32_t cur = 0; m.u32(grav + x3::off::GRAVIDAR_TARGET, cur);
    char b[112]; snprintf(b, sizeof b, "ok settarget grav=0x%08x target=0x%08x -> cur=0x%08x", grav, target_addr, cur); return b;
}
// Queue a synthetic SDL key event into the game's own input chain (the clean "fake a keypress" —
// e.g. Shift+D for auto-dock). down=1 KEYDOWN / 0 KEYUP. Injected at the next SDL_PeepEvents read,
// so the game runs its full SDL->TranslateKey->Input->dock path with everything engine-filled.
static std::string overlay_synthkey(int scancode, int sym, int mod, int down)
{
    SDL_Event e; memset(&e, 0, sizeof e);
    e.type                = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.timestamp       = SDL_GetTicks();
    e.key.windowID        = g_sdl_window ? SDL_GetWindowID(g_sdl_window) : 0;
    e.key.state           = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.repeat          = 0;
    e.key.keysym.scancode = (SDL_Scancode)scancode;
    e.key.keysym.sym      = sym;
    e.key.keysym.mod      = (Uint16)mod;
    int r = SDL_PushEvent(&e);            // into SDL's REAL queue — what the game's SDL_PollEvent reads
    char b[140]; snprintf(b, sizeof b, "ok synthkey sc=%d sym=0x%x mod=%d down=%d push=%d win=%u",
                          scancode, sym, mod, down, r, (unsigned)e.key.windowID); return b;
}
// Force scancode (+ optional LSHIFT) into SDL's polled keyboard STATE for `frames` frames — the
// approach that actually drives this polling game. synthdock = hold SDL_SCANCODE_D + shift.
static std::string overlay_synthhold(int sc, int shift, int frames)
{
    g_synth_sc = sc; g_synth_shift = shift; g_synth_hold = frames;
    char b[96]; snprintf(b, sizeof b, "ok synthhold sc=%d shift=%d frames=%d", sc, shift, frames); return b;
}
// Diagnostic: is the pump-hook running (hold decrements), does our SDL share the game's keyboard
// state (ks reflects real presses), and is ImGui eating keys (wantkbd)?
static std::string overlay_synthdiag(void)
{
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    int wantkbd = -1; if (g_inited) wantkbd = (int)ImGui::GetIO().WantCaptureKeyboard;
    char b[220];
    snprintf(b, sizeof b, "ok synthdiag hold=%d sc=%d ks_D(7)=%d ks_LSHIFT(225)=%d mod=0x%x wantkbd=%d uivis=%d",
             g_synth_hold, g_synth_sc, ks?ks[7]:-1, ks?ks[225]:-1, (unsigned)SDL_GetModState(),
             wantkbd, (int)g_ui_visible);
    return b;
}

// Resolve a scheduler task HANDLE (= ship obj+0x7C) -> task struct -> script id + THIS scene-id.
// task_handle_map_lookup@0x080a49e0(root, handle) -> task ptr; task+0x3c = program; *program = script id;
// task+0x38 = THIS scene-id (the ship the script acts on).
static std::string overlay_taskid(uint32_t handle)
{
    char b[200];
    int32_t root = 0; mem_read_i32(0x08308880, &root);
    typedef int (__attribute__((cdecl)) *lookup_t)(int, int);
    int task = ((lookup_t)0x080a49e0u)((int)(uint32_t)root, (int)handle);
    int32_t prog = 0, thisid = 0, sid = -1;
    if ((uint32_t)task >= 0x08000000u) {
        mem_read_i32((uint32_t)task + 0x3c, &prog);
        mem_read_i32((uint32_t)task + 0x38, &thisid);
        if ((uint32_t)prog >= 0x08000000u) mem_read_i32((uint32_t)prog, &sid);
    }
    snprintf(b, sizeof b, "ok taskid handle=0x%x root=0x%x task=0x%08x program=0x%08x script_id=%d THIS=%d",
             handle, (uint32_t)root, (uint32_t)task, (uint32_t)prog, sid, thisid & 0xFFFF);
    return b;
}

// Spawn a script-task via run_script_async (full ABI): owner=0, caller=0, scriptKey, entry, THIS, argc, arg.
// THIS (arg4) = the scene-id of the object the script's [THIS] acts on. argc/arg pass the script argument.
static std::string overlay_spawn(uint32_t scriptKey, std::string entry, uint32_t thisId, uint32_t arg, int argc)
{
    char b[224];
    typedef int (__attribute__((cdecl)) *rsa_t)(void*, int, int, const char*, int, unsigned, ...);
    const char* e = entry.empty() ? (const char*)nullptr : entry.c_str();
    int h;
    if (argc <= 0) h = ((rsa_t)0x0809e270u)(nullptr, 0, (int)scriptKey, e, (int)thisId, 0u);
    else           h = ((rsa_t)0x0809e270u)(nullptr, 0, (int)scriptKey, e, (int)thisId, 1u, (int)arg);
    g_msci_async_h = h;
    snprintf(b, sizeof b, "ok spawn key=%d entry=%s THIS=%u arg=%u argc=%d -> handle=%d",
             (int)scriptKey, entry.c_str(), thisId, arg, argc, h);
    return b;
}

// THE dock recipe (button-faithful): spawn the engine's own dock orchestrator on the PLAYER ship.
// run_script_async(ctx, 0, scriptKey, name, mode=1, argc=1, stationSceneId) where
//   ctx      = *(scene+0x60)   (the script-system context the real per-object spawns pass),
//   scriptKey= *(ship+0x90)    (the ship's script-context handle -> binds [THIS]=player), MUST be nonzero,
//   stationSceneId = station obj+0x08 (args are scene-ids, not pointers).
// The spawned task loops in-context: builds the bay-tunnel legs (dock_compute_approach_for_bay runs
// safely inside the task) + advances MOVE_TO_POS/DOCK legs until obj+0x54==station — exactly the button.
static std::string overlay_dock2(uint32_t station, std::string name, uint32_t ctxOvr, uint32_t keyOvr)
{
    mem_open_rw();
    char b[256];
    uint32_t ship = player_ship_addr();
    if (!ship || !station) return "err no ship/station";
    int32_t H = 0, stId = 0;
    mem_read_i32(ship + 0x90, &H);                                   // ship's script-context handle
    mem_read_i32(station + x3::off::OBJ_ID, &stId);
    // ctx default = 0 (the captured NotifyTargetLock launch used ctx=0); scriptKey default = *(ship+0x90).
    uint32_t ctx = ctxOvr;                                           // 0 default; override to test others
    uint32_t key = keyOvr ? keyOvr : (uint32_t)H;
    const char* nm = name.empty() ? "!move.movetostation" : name.c_str();
    typedef int (__attribute__((cdecl)) *rsa_t)(void*, int, int, const char*, int, unsigned, ...);
    int handle = ((rsa_t)0x0809e270u)((void*)(uintptr_t)ctx, 0, (int)key, nm, 1, 1u, (int)stId);
    g_msci_async_h = handle;
    snprintf(b, sizeof b,
             "ok dock2 ship=0x%08x ctx=0x%08x key=0x%08x (ship+0x90=0x%08x) stId=%d name=%s -> handle=%d",
             ship, ctx, key, (uint32_t)H, stId, nm, handle);
    return b;
}

// VM-DOCK: integrate the engine VM. The bare run_script_async spawn fails on an IDLE ship because
// *(ship+0x90) doesn't yet resolve the dock scripts. So FIRST run op-10 (DOCKAT) which sets the
// command AND wires the ship's dock script-context, THEN read *(ship+0x90) FRESH and spawn
// !move.movetostation through it (binds [THIS]=player) — the engine's own VM + scheduler run it.
// Leak-guarded: aborts if a live dock task already owns the ship (the leak that helped OOM the game).
static std::string overlay_vmdock(uint32_t station, int do_op10)
{
    mem_open_rw();
    char b[256];
    uint32_t ship = player_ship_addr();
    if (!ship || !station) return "err no ship/station";
    int32_t cur = 0; mem_read_i32(ship + x3::off::SHIP_NOTIFY_VAL, &cur);   // obj+0x7C = task handle
    if (cur) {                                                              // leak guard
        int32_t root = 0; mem_read_i32(0x08308880, &root);
        typedef int (__attribute__((cdecl)) *lk_t)(int, int);
        int t = ((lk_t)0x080a49e0u)((int)(uint32_t)root, cur);
        if ((uint32_t)t >= 0x08000000u) return "abort: live dock task already on ship (leak guard)";
    }
    int32_t H0 = 0; mem_read_i32(ship + 0x90, &H0);                         // context BEFORE op-10
    if (do_op10) player_cmdengage(ship, station, 10, 4, -1, 0);             // set DOCKAT + wire dock context
    int32_t H = 0; mem_read_i32(ship + 0x90, &H);                           // context AFTER op-10 (fresh)
    // owner = the SAME arg the WORKING input-dock passes to run_script_sync = *(g_game_session+0x418) (=9).
    // Prior spawn attempts used ctx=0 / *(scene+0x60)=8 and failed; this is the untried correct owner.
    int32_t gs = 0, owner = 0; mem_read_i32(0x0829f6b4, &gs);
    if (gs) mem_read_i32((uint32_t)gs + 0x418, &owner);
    int32_t stId = 0; mem_read_i32(station + x3::off::OBJ_ID, &stId);
    typedef int (__attribute__((cdecl)) *rsa_t)(void*, int, int, const char*, int, unsigned, ...);
    // Spawn the FULL dock orchestrator (set command DOCKAT + call !move.movetostation) with [THIS]=player.
    int h = ((rsa_t)0x0809e270u)((void*)(uintptr_t)(uint32_t)owner, 0, (int)H,
                                 "!ship.cmd.movestation.std", 1, 1u, (int)stId);
    g_msci_async_h = h;
    snprintf(b, sizeof b, "ok vmdock op10=%d owner=%d H90: 0x%08x->0x%08x stId=%d -> handle=%d",
             do_op10, owner, (uint32_t)H0, (uint32_t)H, stId, h);
    return b;
}

// Arm HW write-watches on up to 4 addresses; the SIGIO handler captures the writer EIP + stack.
static std::string overlay_watchwrite(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    uintptr_t addrs[4]; int n = 0;
    if (a0) addrs[n++] = a0; if (a1) addrs[n++] = a1; if (a2) addrs[n++] = a2; if (a3) addrs[n++] = a3;
    if (!n) return "err no addrs";
    arm_write_watches(addrs, n);
    char b[64]; snprintf(b, sizeof b, "ok armed %d watch(es)", g_wp_count); return b;
}
// Report the captured writer EIP + .text return addresses from the stack snapshot.
static std::string overlay_watchip(void)
{
    char b[160];
    snprintf(b, sizeof b, "ok writer_ip=0x%08lx hits=%d wrote=0x%08lx callers:",
             g_writer_ip, g_wp_hits, g_writer_addr);
    std::string r = b;
    for (int i = 0; i < g_stack_n && i < 40; ++i) {
        unsigned long v = g_stack[i];
        if (v >= 0x08049000UL && v < 0x08300000UL) { char t[16]; snprintf(t, sizeof t, " %08lx", v); r += t; }
    }
    return r;
}

// Re-arm ONLY the command id on the player payload (+ bit19), preserving bay/target/dest/leg-cursor.
static std::string overlay_setcmd(int cmd)
{
    mem_open_rw();
    char buf[96];
    uint32_t ship = player_ship_addr();
    int32_t plw = 0;
    if (!ship || !mem_read_i32(ship + x3::off::OBJ_PAYLOAD, &plw) || (uint32_t)plw < 0x08000000u)
        return "err no ship/payload";
    if (cmd >= 0) mem_write_u16((uint32_t)plw + x3::off::SHIP_CMD_ID, (uint16_t)cmd);
    // OPEN THE EXECUTOR GATE: set bit19; clear bit27 (0x8000000 physics-idle/arrived) + bit9 (0x200
    // manual-ctrl) — the executor only runs a ship while (obj+0x40 & 0x8000200)==0. Physics re-sets
    // bit27 on each leg-arrival, so this must be re-applied (FSM) to keep the legs advancing.
    { int32_t fl = 0; if (mem_read_i32(ship + x3::off::OBJ_FLAGS, &fl))
          mem_write_u32(ship + x3::off::OBJ_FLAGS,
                        ((uint32_t)fl | x3::off::AUTOPILOT_ENGAGED) & ~0x08000200u); }
    int32_t back = 0; mem_read_i32((uint32_t)plw + x3::off::SHIP_CMD_ID, &back);
    int32_t nfl = 0; mem_read_i32(ship + x3::off::OBJ_FLAGS, &nfl);
    snprintf(buf, sizeof buf, "ok setcmd %d -> cmd(C4)=%d flags=0x%08x", cmd, back & 0xFFFF, nfl);
    return buf;
}

// Spawn the engine's OWN dock orchestrator script `!move.movetostation` on the player ship via
// run_script_async — the leg loop that re-issues MOVE/DOCK legs until obj+0x54==station (real dock).
// `thismode` sweeps how the THIS handle + station arg are encoded (the binding is the open risk:
// player-input sites pass ctx 0x96, the menu binds THIS=ship). handle!=0 = engine accepted the spawn.
static std::string overlay_dockscript(uint32_t station, int thismode)
{
    char buf[208];
    uint32_t ship = player_ship_addr();
    if (!ship || !station) return "err no ship/station";
    int32_t shipId = 0, stId = 0;
    mem_read_i32(ship + x3::off::OBJ_ID, &shipId);
    mem_read_i32(station + x3::off::OBJ_ID, &stId);
    int thisH, arg;
    switch (thismode) {
        case 1:  thisH = shipId;     arg = stId;            break;  // ship scene-id + station scene-id
        case 2:  thisH = (int)ship;  arg = (int)station;    break;  // raw pointers
        case 3:  thisH = 0x96;       arg = (int)station;    break;  // player ctx + station pointer
        default: thisH = 0x96;       arg = stId;            break;  // (0) player ctx + station scene-id
    }
    int exists = msci_find_id("!move.movetostation");                 // -1 if the script isn't loaded
    int h = run_script_async(nullptr, 0, thisH, "!move.movetostation", 1, 1, arg);
    g_msci_async_h = h;
    snprintf(buf, sizeof buf, "ok dockscript mode=%d this=0x%x arg=0x%x ship=0x%08x st=0x%08x exists_id=%d handle=%d",
             thismode, (unsigned)thisH, (unsigned)arg, ship, station, exists, h);
    return buf;
}

// General MSCI script probe — call any script by name with arbitrary THIS handle + one arg, async
// or sync, and report msci_find_id + the returned handle/value. For live ABI/resolution probing.
static std::string overlay_runscript(std::string name, uint32_t thisH, uint32_t arg, int argc, int sync)
{
    char buf[256];
    int found = msci_find_id(name.c_str());
    long r;
    if (sync) {
        r = (argc <= 0) ? run_script_sync(nullptr, 0, (int)thisH, name.c_str(), 1, 0)
                        : run_script_sync(nullptr, 0, (int)thisH, name.c_str(), 1, 1, (int)arg);
        g_msci_sync_v = (int)r;
    } else {
        r = (argc <= 0) ? run_script_async(nullptr, 0, (int)thisH, name.c_str(), 1, 0)
                        : run_script_async(nullptr, 0, (int)thisH, name.c_str(), 1, 1, (int)arg);
        g_msci_async_h = (int)r;
    }
    snprintf(buf, sizeof buf, "ok runscript name=%s this=0x%x arg=0x%x argc=%d sync=%d find_id=%d ret=%ld",
             name.c_str(), thisH, arg, argc, sync, found, r);
    return buf;
}

// Multi-arg MSCI method probe — call a class method (e.g. StartExecuteCommand(cmdId, station))
// with [THIS] = the instance that `thisH`(=*(ship+0x90)) resolves to, passing up to 5 int args.
// run_script_async is variadic cdecl, so trailing args beyond argc are harmless.
static std::string overlay_runscript2(std::string name, uint32_t thisH,
                                      const std::vector<uint32_t>& a, int sync)
{
    char buf[300];
    run_script_t fn = sync ? run_script_sync : run_script_async;
    int n = (int)a.size();
    long r = 0;
    switch (n) {
        case 0:  r = fn(nullptr,0,(int)thisH,name.c_str(),1,0); break;
        case 1:  r = fn(nullptr,0,(int)thisH,name.c_str(),1,1,(int)a[0]); break;
        case 2:  r = fn(nullptr,0,(int)thisH,name.c_str(),1,2,(int)a[0],(int)a[1]); break;
        case 3:  r = fn(nullptr,0,(int)thisH,name.c_str(),1,3,(int)a[0],(int)a[1],(int)a[2]); break;
        case 4:  r = fn(nullptr,0,(int)thisH,name.c_str(),1,4,(int)a[0],(int)a[1],(int)a[2],(int)a[3]); break;
        default: r = fn(nullptr,0,(int)thisH,name.c_str(),1,5,(int)a[0],(int)a[1],(int)a[2],(int)a[3],(int)a[4]); break;
    }
    if (sync) g_msci_sync_v = (int)r; else g_msci_async_h = (int)r;
    snprintf(buf, sizeof buf, "ok runscript2 name=%s this=0x%x nargs=%d sync=%d ret=%ld",
             name.c_str(), thisH, n, sync, r);
    return buf;
}

// Context-menu items: autopilot the PLAYER's own ship to `o`. (Only your own ship — NPCs
// re-assert their own job each tick.) Enabled by object type.
static void command_menu_items(const x3::ObjInfo& o)
{
    if (!o.addr) return;
    uint32_t s = player_ship_addr();
    bool dockable = (o.type == x3::off::TYPE_STATION || o.type == x3::off::TYPE_FACTORY);
    bool isgate   = (o.type == x3::off::TYPE_GATE);
    ImGui::TextDisabled("autopilot your ship:");
    if (ImGui::MenuItem("Fly to"))                              player_autopilot(s, x3::off::CMD_FLYTO,  o.addr);
    if (ImGui::MenuItem("Jump through", nullptr, false, isgate)) player_jumpgate(s, o.addr, 4);
    if (ImGui::MenuItem("Dock at", nullptr, false, dockable))   player_dock_input(o);   // game's real Shift+D dock
    if (ImGui::MenuItem("Stop autopilot"))                      player_autopilot_cancel(s);
}

// Read-only panel: list the objects in the player's current sector.
static void map_panel(void)
{
    if (!ImGui::Begin("x3ui - sector map")) { ImGui::End(); return; }
    mem_open();

    if (g_cmd_dbg_wrote >= 0) {                  // last command-write proof (in-process readback)
        bool landed = (g_cmd_dbg_readback == g_cmd_dbg_wrote);
        ImGui::TextColored(landed ? ImVec4(0.4f,1,0.5f,1) : ImVec4(1,0.5f,0.3f,1),
            "cmd: ship=0x%08x pl=0x%08x tgt=0x%08x wrote=%d back=%d  %s",
            g_cmd_dbg_ship, g_cmd_dbg_payload, g_cmd_dbg_target, g_cmd_dbg_wrote, g_cmd_dbg_readback,
            landed ? "WRITE LANDED" : "MISMATCH/no-op");
    }

    // Throttle: re-walk ~6x/sec. Also FREEZE the refresh while the user is hovering
    // this window, so scrolling the table isn't fighting the live data churn.
    static x3::SectorView view;
    static double last = -1.0;
    static bool frozen = false;          // hover state captured at end of last frame
    double now = ImGui::GetTime();
    if (!frozen && (last < 0.0 || (now - last) > 0.15)) {
        view = x3::read_current_sector(make_engine_mem());
        last = now;
    }

    if (!view.in_sector) {
        ImGui::TextDisabled("not in a sector (main menu / loading?)");
        ImGui::End();
        return;
    }

    // Per-category counts so the visible total can be compared to the game's map.
    int n_ships = 0, n_stations = 0;
    for (const auto& o : view.objects) {
        if (o.type == x3::off::TYPE_SHIP) ++n_ships;
        else if (o.type == x3::off::TYPE_STATION || o.type == x3::off::TYPE_FACTORY) ++n_stations;
    }

    ImGui::Text("sector: %s   (%zu objects: %d ships, %d stations)   scanner %.0f km",
                view.sector_name.empty() ? "(unnamed)" : view.sector_name.c_str(),
                view.objects.size(), n_ships, n_stations,
                view.scanner_range / x3::off::UNITS_PER_KM);

    // EVE-overview filter (independent of the 3D map's filter).
    static int table_rel = 0, table_type = 0;
    map_filter_bar(&table_rel, &table_type);
    int shown = 0;
    for (const auto& o : view.objects) if (obj_passes_filter(o, table_rel, table_type)) ++shown;
    ImGui::SameLine(); ImGui::TextDisabled("|  showing %d", shown);

    // Sortable, with a sticky header and a right-click-header column show/hide menu
    // (id/type hidden by default). Fills the window; size persists via imgui.ini.
    enum { COL_NAME, COL_DIST, COL_POS, COL_ID, COL_TYPE };
    ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                             ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable |
                             ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable |
                             ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("objs", 5, tflags, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch |
                                ImGuiTableColumnFlags_DefaultSort, 0.0f, COL_NAME);
        ImGui::TableSetupColumn("dist (km)", ImGuiTableColumnFlags_WidthFixed, 72.0f, COL_DIST);
        ImGui::TableSetupColumn("pos (km)", ImGuiTableColumnFlags_WidthStretch, 0.0f, COL_POS);
        ImGui::TableSetupColumn("id",   ImGuiTableColumnFlags_DefaultHide, 0.0f, COL_ID);
        ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_DefaultHide, 0.0f, COL_TYPE);
        ImGui::TableSetupScrollFreeze(0, 1);   // sticky header row
        ImGui::TableHeadersRow();

        static std::vector<const x3::ObjInfo*> rows;
        rows.clear();
        for (const auto& o : view.objects)
            if (obj_passes_filter(o, table_rel, table_type)) rows.push_back(&o);
        if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs()) {
            if (ss->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& sp = ss->Specs[0];
                bool asc = (sp.SortDirection == ImGuiSortDirection_Ascending);
                std::sort(rows.begin(), rows.end(),
                    [&](const x3::ObjInfo* a, const x3::ObjInfo* b) {
                        long long c = 0;
                        switch (sp.ColumnUserID) {
                            case COL_NAME: c = a->name.compare(b->name); break;
                            case COL_DIST:
                            case COL_POS:  c = (long long)a->dist - b->dist; break;
                            case COL_ID:   c = (long long)a->id   - b->id;   break;
                            case COL_TYPE: c = (long long)a->type - b->type; break;
                        }
                        return asc ? (c < 0) : (c > 0);
                    });
            }
        }

        const float U = x3::off::UNITS_PER_KM;
        for (const x3::ObjInfo* op : rows) {
            const x3::ObjInfo& o = *op;
            ImGui::TableNextRow();
            ImGui::PushID((int)o.id);
            ImGui::TableSetColumnIndex(COL_NAME);
            ImGui::PushStyleColor(ImGuiCol_Text, faction_color(o));   // EVE-style faction tint
            if (ImGui::Selectable(o.name.empty() ? "(unnamed)" : o.name.c_str(),
                                  o.id == g_sel_id,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                select_target(o);                                    // left-click sets target
            ImGui::PopStyleColor();
            if (view.current_target_addr && o.addr == view.current_target_addr) {  // live game target
                ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRect(ra, rb, IM_COL32(255,170,40,220), 0.0f, 0, 1.5f);
            }
            if (ImGui::BeginPopupContextItem("row")) {       // right-click the row
                const char* rel = relation_label(o.relation);
                ImGui::TextDisabled("%s  (%s%s%s)", o.name.empty() ? "(unnamed)" : o.name.c_str(),
                                    x3::type_label(o.type), *rel ? ", " : "", rel);
                ImGui::Separator();
                char idbuf[32]; snprintf(idbuf, sizeof idbuf, "%u", o.id);
                if (ImGui::MenuItem("Copy id")) ImGui::SetClipboardText(idbuf);
                command_menu_items(o);
                ImGui::EndPopup();
            }
            ImGui::TableSetColumnIndex(COL_DIST);
            if (o.dist >= 0) ImGui::Text("%.1f", o.dist / U); else ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(COL_POS);
            ImGui::Text("%.1f, %.1f, %.1f", o.x / U, o.y / U, o.z / U);
            ImGui::TableSetColumnIndex(COL_ID);   ImGui::Text("%u", o.id);
            ImGui::TableSetColumnIndex(COL_TYPE); ImGui::Text("%s(%d)", x3::type_label(o.type), o.type);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // Pause the live refresh while the user is on this window, so the table holds
    // still while reading/scrolling (state used at the top of the next frame).
    frozen = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                    ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    ImGui::End();
}

// ---- 3D sector map (orbit camera, ImGui draw-list) ------------------------
struct Camera3D {
    float yaw = 0.7f, pitch = 0.5f, dist = 70.0f;
    float cx = 0, cy = 0, cz = 0;   // look-at center (km rel. player), lerped each frame
    float tx = 0, ty = 0, tz = 0;   // target center the camera eases toward
};

// Project a world point (km, target at origin) to screen. False if behind camera.
static bool project3d(const Camera3D& c, ImVec2 ctr, float focal,
                      float wx, float wy, float wz, ImVec2& out, float& depth)
{
    float cp = cosf(c.pitch), sp = sinf(c.pitch), cy = cosf(c.yaw), sy = sinf(c.yaw);
    float ex = c.dist*cp*sy, ey = c.dist*sp, ez = c.dist*cp*cy;   // eye (rel. target)
    float fx = -ex, fy = -ey, fz = -ez, fl = sqrtf(fx*fx+fy*fy+fz*fz);
    if (fl < 1e-6f) return false;
    fx/=fl; fy/=fl; fz/=fl;                                       // forward
    float rx = -fz, ry = 0.0f, rz = fx, rl = sqrtf(rx*rx+rz*rz);  // right = cross(f,up0)
    if (rl < 1e-6f) { rx = 1; rz = 0; rl = 1; }
    rx/=rl; rz/=rl;
    float ux = ry*fz - rz*fy, uy = rz*fx - rx*fz, uz = rx*fy - ry*fx;  // up = cross(r,f)
    float px = wx-ex, py = wy-ey, pz = wz-ez;
    float vx = px*rx + py*ry + pz*rz;
    float vy = px*ux + py*uy + pz*uz;
    float vz = px*fx + py*fy + pz*fz;                            // depth
    if (vz < 0.05f) return false;
    out.x = ctr.x + (vx/vz)*focal;
    out.y = ctr.y - (vy/vz)*focal;
    depth = vz;
    return true;
}

static ImU32 type_color(int16_t t)
{
    switch (t) {
        case x3::off::TYPE_SHIP:     return IM_COL32(120, 200, 255, 255);
        case x3::off::TYPE_STATION:  return IM_COL32(255, 210,  90, 255);
        case x3::off::TYPE_FACTORY:  return IM_COL32(255, 150,  60, 255);
        case x3::off::TYPE_GATE:     return IM_COL32(120, 255, 150, 255);
        case x3::off::TYPE_ASTEROID: return IM_COL32(170, 150, 130, 255);
        case x3::off::TYPE_SUN:      return IM_COL32(255, 245, 200, 255);
        case x3::off::TYPE_PLANET:   return IM_COL32(110, 150, 230, 255);
        default:                     return IM_COL32(185, 185, 185, 255);
    }
}

// Faction color, matching the game's blip IFF coloring (own/friend/neutral/enemy).
// Objects with no faction (gates/suns/asteroids/...) keep their structural type color.
static ImU32 faction_color(const x3::ObjInfo& o)
{
    switch (o.relation) {
        case x3::REL_OWN:     return IM_COL32(255,  90, 255, 255); // magenta
        case x3::REL_FRIEND:  return IM_COL32( 90, 150, 255, 255); // blue
        case x3::REL_ENEMY:   return IM_COL32(255,  70,  70, 255); // red
        case x3::REL_NEUTRAL: return IM_COL32(110, 215,  90, 255); // green
        default:              return type_color(o.type);
    }
}

static const char* relation_label(int8_t rel)
{
    switch (rel) {
        case x3::REL_OWN:     return "own";
        case x3::REL_FRIEND:  return "friend";
        case x3::REL_ENEMY:   return "enemy";
        case x3::REL_NEUTRAL: return "neutral";
        default:              return "";
    }
}

// ───────────────────────── GPU wireframe line renderer ─────────────────────
// A dense preview (100k+ verts / 280k+ edges) made the per-frame ImGui CPU line
// build the bottleneck. Instead we upload the NORMALIZED line geometry to a GL
// buffer ONCE per selection and draw it in one glDrawElements via an ImGui draw
// callback, with the orbit camera as an MVP uniform — per-frame CPU cost drops to
// a 4x4 matrix + one draw call. GL 2.0+ entrypoints load via SDL_GL_GetProcAddress
// (the game's context is GL 3.0+/GLSL 130); GL 1.1 calls are linked directly.
namespace meshgl {
static bool    g_tried = false, g_ok = false;
static GLuint  g_vao = 0, g_vbo = 0, g_ebo = 0, g_prog = 0;
static GLint   g_uMVP = -1, g_uColor = -1;
static GLsizei g_edge_count = 0;
// Holographic solid program (pos+normal, fresnel rim, additive) — shared by all hulls.
static GLuint  g_sprog = 0;
static GLint   g_suMVP = -1, g_suMV = -1, g_suColor = -1;

static PFNGLGENBUFFERSPROC              pGenBuffers;
static PFNGLBINDBUFFERPROC              pBindBuffer;
static PFNGLBUFFERDATAPROC              pBufferData;
static PFNGLGENVERTEXARRAYSPROC         pGenVertexArrays;
static PFNGLBINDVERTEXARRAYPROC         pBindVertexArray;
static PFNGLENABLEVERTEXATTRIBARRAYPROC pEnableVertexAttribArray;
static PFNGLVERTEXATTRIBPOINTERPROC     pVertexAttribPointer;
static PFNGLCREATESHADERPROC            pCreateShader;
static PFNGLSHADERSOURCEPROC            pShaderSource;
static PFNGLCOMPILESHADERPROC           pCompileShader;
static PFNGLCREATEPROGRAMPROC           pCreateProgram;
static PFNGLATTACHSHADERPROC            pAttachShader;
static PFNGLBINDATTRIBLOCATIONPROC      pBindAttribLocation;
static PFNGLLINKPROGRAMPROC             pLinkProgram;
static PFNGLUSEPROGRAMPROC              pUseProgram;
static PFNGLGETUNIFORMLOCATIONPROC      pGetUniformLocation;
static PFNGLUNIFORMMATRIX4FVPROC        pUniformMatrix4fv;
static PFNGLUNIFORM4FPROC               pUniform4f;
static PFNGLDELETESHADERPROC            pDeleteShader;
static PFNGLDELETEBUFFERSPROC           pDeleteBuffers;
static PFNGLDELETEVERTEXARRAYSPROC      pDeleteVertexArrays;

template<class T> static bool load(T& fp, const char* nm) {
    fp = (T)SDL_GL_GetProcAddress(nm); return fp != nullptr;
}
static void init() {
    g_tried = true;
    bool ok = load(pGenBuffers,"glGenBuffers") && load(pBindBuffer,"glBindBuffer")
        && load(pBufferData,"glBufferData") && load(pGenVertexArrays,"glGenVertexArrays")
        && load(pBindVertexArray,"glBindVertexArray") && load(pEnableVertexAttribArray,"glEnableVertexAttribArray")
        && load(pVertexAttribPointer,"glVertexAttribPointer") && load(pCreateShader,"glCreateShader")
        && load(pShaderSource,"glShaderSource") && load(pCompileShader,"glCompileShader")
        && load(pCreateProgram,"glCreateProgram") && load(pAttachShader,"glAttachShader")
        && load(pBindAttribLocation,"glBindAttribLocation") && load(pLinkProgram,"glLinkProgram")
        && load(pUseProgram,"glUseProgram") && load(pGetUniformLocation,"glGetUniformLocation")
        && load(pUniformMatrix4fv,"glUniformMatrix4fv") && load(pUniform4f,"glUniform4f")
        && load(pDeleteShader,"glDeleteShader")
        && load(pDeleteBuffers,"glDeleteBuffers") && load(pDeleteVertexArrays,"glDeleteVertexArrays");
    if (!ok) return;
    const char* vs = "#version 130\nin vec3 aPos;\nuniform mat4 uMVP;\n"
                     "void main(){ gl_Position = uMVP*vec4(aPos,1.0); }\n";
    const char* fs = "#version 130\nuniform vec4 uColor;\nout vec4 frag;\n"
                     "void main(){ frag = uColor; }\n";
    GLuint v = pCreateShader(GL_VERTEX_SHADER);   pShaderSource(v,1,&vs,nullptr); pCompileShader(v);
    GLuint f = pCreateShader(GL_FRAGMENT_SHADER); pShaderSource(f,1,&fs,nullptr); pCompileShader(f);
    g_prog = pCreateProgram(); pAttachShader(g_prog,v); pAttachShader(g_prog,f);
    pBindAttribLocation(g_prog,0,"aPos"); pLinkProgram(g_prog);
    pDeleteShader(v); pDeleteShader(f);
    g_uMVP = pGetUniformLocation(g_prog,"uMVP"); g_uColor = pGetUniformLocation(g_prog,"uColor");
    pGenVertexArrays(1,&g_vao); pGenBuffers(1,&g_vbo); pGenBuffers(1,&g_ebo);
    pBindVertexArray(g_vao);
    pBindBuffer(GL_ARRAY_BUFFER,g_vbo);
    pEnableVertexAttribArray(0);
    pVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,0,(const void*)0);
    pBindBuffer(GL_ELEMENT_ARRAY_BUFFER,g_ebo);
    pBindVertexArray(0);

    // Holographic solid program: pos+normal, fresnel rim, additive. uMV gives view-space
    // pos+normal (camera at origin in view space) for the fresnel term.
    const char* svs =
        "#version 130\n"
        "in vec3 aPos; in vec3 aNormal;\n"
        "uniform mat4 uMVP; uniform mat4 uMV;\n"
        "out vec3 vN; out vec3 vP;\n"
        "void main(){ vP=(uMV*vec4(aPos,1.0)).xyz; vN=normalize(mat3(uMV)*aNormal);\n"
        "             gl_Position=uMVP*vec4(aPos,1.0); }\n";
    const char* sfs =
        "#version 130\n"
        "in vec3 vN; in vec3 vP; uniform vec4 uColor; out vec4 frag;\n"
        "void main(){ vec3 N=normalize(vN); vec3 V=normalize(-vP);\n"
        "  float f=pow(1.0-clamp(abs(dot(N,V)),0.0,1.0),2.5);\n"   // fresnel rim (near & far faces)
        "  float i=0.18+0.9*f;\n"                                   // base fill + rim gain
        "  frag=vec4(uColor.rgb*i, uColor.a*i); }\n";
    GLuint sv = pCreateShader(GL_VERTEX_SHADER);   pShaderSource(sv,1,&svs,nullptr); pCompileShader(sv);
    GLuint sf = pCreateShader(GL_FRAGMENT_SHADER); pShaderSource(sf,1,&sfs,nullptr); pCompileShader(sf);
    g_sprog = pCreateProgram(); pAttachShader(g_sprog,sv); pAttachShader(g_sprog,sf);
    pBindAttribLocation(g_sprog,0,"aPos"); pBindAttribLocation(g_sprog,1,"aNormal");
    pLinkProgram(g_sprog); pDeleteShader(sv); pDeleteShader(sf);
    g_suMVP = pGetUniformLocation(g_sprog,"uMVP");
    g_suMV  = pGetUniformLocation(g_sprog,"uMV");
    g_suColor = pGetUniformLocation(g_sprog,"uColor");

    g_ok = true;
}
static void upload(const std::vector<float>& verts, const std::vector<uint32_t>& edges) {
    if (!g_ok) return;
    pBindVertexArray(g_vao);
    pBindBuffer(GL_ARRAY_BUFFER,g_vbo);
    pBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size()*sizeof(float)), verts.data(), GL_STATIC_DRAW);
    pBindBuffer(GL_ELEMENT_ARRAY_BUFFER,g_ebo);
    pBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(edges.size()*sizeof(uint32_t)), edges.data(), GL_STATIC_DRAW);
    pBindVertexArray(0);
    g_edge_count = (GLsizei)edges.size();
}

// One cached object's GPU geometry (body-local verts+normals, triangle indices).
struct Hull { GLuint vao=0, vbo=0, ebo=0; GLsizei icount=0; };

static Hull hull_create(const std::vector<float>& verts, const std::vector<float>& normals,
                        const std::vector<uint32_t>& tris) {
    Hull h;
    if (!g_ok || verts.empty() || tris.empty()) return h;
    size_t vc = verts.size() / 3;
    std::vector<float> inter(vc * 6);
    for (size_t i = 0; i < vc; ++i) {
        inter[i*6+0]=verts[i*3+0]; inter[i*6+1]=verts[i*3+1]; inter[i*6+2]=verts[i*3+2];
        inter[i*6+3]=normals[i*3+0]; inter[i*6+4]=normals[i*3+1]; inter[i*6+5]=normals[i*3+2];
    }
    pGenVertexArrays(1,&h.vao); pGenBuffers(1,&h.vbo); pGenBuffers(1,&h.ebo);
    pBindVertexArray(h.vao);
    pBindBuffer(GL_ARRAY_BUFFER,h.vbo);
    pBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(inter.size()*sizeof(float)),inter.data(),GL_STATIC_DRAW);
    pEnableVertexAttribArray(0);
    pVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(const void*)0);
    pEnableVertexAttribArray(1);
    pVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(const void*)(3*sizeof(float)));
    pBindBuffer(GL_ELEMENT_ARRAY_BUFFER,h.ebo);
    pBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)(tris.size()*sizeof(uint32_t)),tris.data(),GL_STATIC_DRAW);
    pBindVertexArray(0);
    h.icount = (GLsizei)tris.size();
    return h;
}
static void hull_destroy(Hull& h) {
    if (h.vbo) pDeleteBuffers(1,&h.vbo);
    if (h.ebo) pDeleteBuffers(1,&h.ebo);
    if (h.vao) pDeleteVertexArrays(1,&h.vao);
    h = Hull{};
}

struct DrawParams { float mvp[16]; float col[4]; int vx, vy, vw, vh; };
static DrawParams g_dp;
// Runs inside ImGui's render pass (one GL draw call). ImDrawCallback_ResetRenderState
// (queued right after) restores ImGui's own GL state.
static void draw_cb(const ImDrawList*, const ImDrawCmd*) {
    if (!g_ok || g_edge_count == 0) return;
    GLint old_vp[4]; glGetIntegerv(GL_VIEWPORT, old_vp);
    glViewport(g_dp.vx, g_dp.vy, g_dp.vw, g_dp.vh);
    glEnable(GL_SCISSOR_TEST); glScissor(g_dp.vx, g_dp.vy, g_dp.vw, g_dp.vh);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    pUseProgram(g_prog);
    pUniformMatrix4fv(g_uMVP, 1, GL_FALSE, g_dp.mvp);
    pUniform4f(g_uColor, g_dp.col[0], g_dp.col[1], g_dp.col[2], g_dp.col[3]);
    pBindVertexArray(g_vao);
    glDrawElements(GL_LINES, g_edge_count, GL_UNSIGNED_INT, (const void*)0);
    pBindVertexArray(0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(old_vp[0], old_vp[1], old_vp[2], old_vp[3]);
}

// Batched holographic solid pass: a per-frame list of (hull, MVP, MV, color), drawn
// far->near in one callback with additive blend + depth-write off.
struct SolidItem { const Hull* h; float mvp[16]; float mv[16]; float col[4]; };
static std::vector<SolidItem> g_solids;
static int g_svx, g_svy, g_svw, g_svh;       // viewport (framebuffer px), set per panel
static void solid_cb(const ImDrawList*, const ImDrawCmd*) {
    if (!g_ok || g_solids.empty()) return;
    GLint old_vp[4]; glGetIntegerv(GL_VIEWPORT, old_vp);
    glViewport(g_svx,g_svy,g_svw,g_svh);
    glEnable(GL_SCISSOR_TEST); glScissor(g_svx,g_svy,g_svw,g_svh);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);    // additive holo glow
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_CULL_FACE);
    pUseProgram(g_sprog);
    for (const auto& s : g_solids) {                          // far->near
        if (!s.h || !s.h->icount) continue;
        pUniformMatrix4fv(g_suMVP,1,GL_FALSE,s.mvp);
        pUniformMatrix4fv(g_suMV,1,GL_FALSE,s.mv);
        pUniform4f(g_suColor,s.col[0],s.col[1],s.col[2],s.col[3]);
        pBindVertexArray(s.h->vao);
        glDrawElements(GL_TRIANGLES,s.h->icount,GL_UNSIGNED_INT,(const void*)0);
    }
    pBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    glViewport(old_vp[0], old_vp[1], old_vp[2], old_vp[3]);
}
// column-major 4x4 helpers
static void m_perspective(float* m, float fovy, float aspect, float zn, float zf) {
    float f = 1.0f / tanf(fovy*0.5f);
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0]=f/aspect; m[5]=f; m[10]=(zf+zn)/(zn-zf); m[11]=-1.0f; m[14]=(2.0f*zf*zn)/(zn-zf);
}
static void m_lookat(float* m, float ex,float ey,float ez) {   // look at origin, up +Y
    float fx=-ex, fy=-ey, fz=-ez; float fl=sqrtf(fx*fx+fy*fy+fz*fz); if(fl<1e-6f)fl=1; fx/=fl;fy/=fl;fz/=fl;
    float sx=-fz, sy=0.0f, sz=fx;                                      // right = cross(forward,+Y)
    float sl=sqrtf(sx*sx+sy*sy+sz*sz); if(sl<1e-6f){sx=1;sy=0;sz=0;sl=1;} sx/=sl;sy/=sl;sz/=sl;
    float ux=sy*fz-sz*fy, uy=sz*fx-sx*fz, uz=sx*fy-sy*fx;              // up = cross(right,forward)
    m[0]=sx; m[4]=sy; m[8]=sz;  m[12]=-(sx*ex+sy*ey+sz*ez);
    m[1]=ux; m[5]=uy; m[9]=uz;  m[13]=-(ux*ex+uy*ey+uz*ez);
    m[2]=-fx;m[6]=-fy;m[10]=-fz;m[14]=(fx*ex+fy*ey+fz*ez);
    m[3]=0;  m[7]=0;  m[11]=0;  m[15]=1;
}
static void m_mul(float* o, const float* a, const float* b) {  // o = a*b
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
        o[c*4+r] = a[0*4+r]*b[c*4+0] + a[1*4+r]*b[c*4+1] + a[2*4+r]*b[c*4+2] + a[3*4+r]*b[c*4+3];
}
} // namespace meshgl

// ---- solid hull cache (read ONCE at finest, placed live each frame) -------------
struct HullCache {
    meshgl::Hull gpu;
    float        extent_km = 1.0f;   // body bbox half-extent in km (for dot<->hull + box)
    uint32_t     model_id  = 0;      // root model id at read time (re-read if it changes)
    bool         ok        = false;
};
static std::unordered_map<uint32_t, HullCache> g_hulls;   // key = object address

// Ensure object `addr` (root model `mid`) has its FINEST-LOD hull cached. Reads at most
// once per object (re-reads only if its model id changes) — never re-read for zoom, so
// there's no per-frame geometry cost after the first read. Bulk byte reads (Mem::bytes)
// keep even a 130k-tri station read cheap. Returns nullptr if it has no resolvable mesh.
static HullCache* hull_ensure(uint32_t addr, uint32_t mid, int& budget) {
    auto it = g_hulls.find(addr);
    if (it != g_hulls.end() && it->second.model_id == mid)
        return it->second.ok ? &it->second : nullptr;      // already read (or known mesh-less)
    if (budget <= 0) return nullptr;
    --budget;
    HullCache hc;
    hc.model_id = mid;
    x3::Mem mem = make_engine_mem();
    x3::MeshWire w = x3::read_object_wireframe(mem, addr, 512, 200000, 600000, /*lod=*/0, 1000);
    if (w.ok && !w.tris.empty()) {
        // Un-rotate world-oriented geometry into body frame by R_read^T (root+0x3c).
        uint32_t root = 0; float R[9] = {1,0,0, 0,1,0, 0,0,1};
        if (x3::read_object_root(mem, addr, root)) x3::read_node_orient(mem, root, 0x3c, R);
        float lo[3]={1e30f,1e30f,1e30f}, hi[3]={-1e30f,-1e30f,-1e30f};
        for (size_t i = 0; i + 2 < w.verts.size(); i += 3) {
            float x=w.verts[i], y=w.verts[i+1], z=w.verts[i+2];
            // body = R^T * world : out_i = sum_j R[j*3+i]*v_j
            float bx=R[0]*x+R[3]*y+R[6]*z, by=R[1]*x+R[4]*y+R[7]*z, bz=R[2]*x+R[5]*y+R[8]*z;
            w.verts[i]=bx; w.verts[i+1]=by; w.verts[i+2]=bz;
            for (int k=0;k<3;++k){ float v=(k==0?bx:k==1?by:bz); lo[k]=std::min(lo[k],v); hi[k]=std::max(hi[k],v);} }
        std::vector<float> nrm = x3::compute_vertex_normals(w.verts, w.tris);
        if (!meshgl::g_tried) meshgl::init();
        if (meshgl::g_ok) hc.gpu = meshgl::hull_create(w.verts, nrm, w.tris);
        float ext = std::max(std::max(hi[0]-lo[0], hi[1]-lo[1]), hi[2]-lo[2]) * 0.5f;
        hc.extent_km = (ext > 1e-3f ? ext : 1.0f) / x3::off::UNITS_PER_KM;
        hc.ok = (hc.gpu.icount > 0);
    }
    if (it != g_hulls.end()) meshgl::hull_destroy(it->second.gpu);
    g_hulls[addr] = hc;
    return hc.ok ? &g_hulls[addr] : nullptr;
}

// Drop hulls for objects no longer in `live` (despawned / out of range).
static void hull_gc(const std::vector<x3::ObjInfo>& live) {
    std::unordered_set<uint32_t> keep;
    for (const auto& o : live) keep.insert(o.addr);
    for (auto it = g_hulls.begin(); it != g_hulls.end(); ) {
        if (!keep.count(it->first)) { meshgl::hull_destroy(it->second.gpu); it = g_hulls.erase(it); }
        else ++it;
    }
}

static void map3d_panel(void)
{
    ImGui::SetNextWindowSize(ImVec2(580, 480), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(g_force_opaque ? 1.0f : 0.55f);  // map panel reads clearly (not washed out)
    if (!ImGui::Begin("x3ui - 3D map")) { ImGui::End(); return; }
    mem_open();

    // Refresh every frame so object motion is smooth at the game's frame rate
    // (name lookups are cached, so the per-frame read stays cheap).
    static x3::SectorView view;
    view = x3::read_current_sector(make_engine_mem());
    if (!view.in_sector || !view.player_valid) {
        ImGui::TextDisabled("not in a sector");
        ImGui::End();
        return;
    }
    // Confirm the selected object is still live (so the mesh preview can't deref a
    // despawned pointer).
    g_sel_alive = false;
    if (g_sel_addr)
        for (const auto& o : view.objects)
            if (o.addr == g_sel_addr) { g_sel_alive = true; break; }

    static Camera3D cam;
    static uint32_t focus_addr = 0;     // dbl-clicked object the look-at center tracks (0 = none)

    static int map3d_rel = 0, map3d_type = 0;   // independent of the table's filter
    static bool follow = false;        // lock the camera yaw to the ship's heading
    static bool show_axes = false;     // orientation gizmo on the selected object
    static bool show_cfg = false;      // collapsible settings child

    // Compact header: a settings toggle + the always-useful hint. Everything tweakable
    // lives in the collapsible child below so the panel isn't a wall of buttons.
    if (ImGui::SmallButton(show_cfg ? "settings -" : "settings +")) show_cfg = !show_cfg;
    ImGui::SameLine();
    if (focus_addr) ImGui::TextDisabled("dbl-click focus · MMB pan · (tracking)");
    else            ImGui::TextDisabled("dbl-click focus · MMB pan");
    if (show_cfg) {
        ImGui::BeginChild("map_cfg", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 2.4f), true);
        map_filter_bar(&map3d_rel, &map3d_type);
        ImGui::Checkbox("follow ship", &follow);
        ImGui::SameLine(); ImGui::Checkbox("orient axes (selected)", &show_axes);
        ImGui::SameLine();
        if (ImGui::Button("reset view")) {       // re-center + re-orient (e.g. after undocking)
            cam.tx = cam.ty = cam.tz = 0;
            cam.yaw = 0.7f; cam.pitch = 0.5f; cam.dist = 70.0f;
            focus_addr = 0;
        }
        ImGui::EndChild();
    }
    hull_gc(view.objects);
    int hull_budget = 3;                 // first-read geometry budget per frame (one-time)
    meshgl::g_solids.clear();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 64.0f || avail.y < 64.0f) { ImGui::End(); return; }
    ImVec2 p0  = ImGui::GetCursorScreenPos();
    ImVec2 p1  = ImVec2(p0.x + avail.x, p0.y + avail.y);
    ImVec2 ctr = ImVec2(p0.x + avail.x * 0.5f, p0.y + avail.y * 0.5f);
    float  focal = avail.y * 0.5f / tanf(0.45f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::InvisibleButton("canvas3d", avail,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemActive()) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            // MMB pan: slide the look-at center across the camera's right/up plane.
            float cp = cosf(cam.pitch), sp = sinf(cam.pitch);
            float cyw = cosf(cam.yaw), syw = sinf(cam.yaw);
            float fx = -cp*syw, fy = -sp, fz = -cp*cyw;        // forward (eye->target)
            float rx = -fz, rz = fx, rl = sqrtf(rx*rx + rz*rz);
            if (rl > 1e-6f) { rx /= rl; rz /= rl; }
            float ux = -rz*fy, uy = rz*fx - rx*fz, uz = rx*fy;  // up = cross(right, fwd)
            float sc = cam.dist / focal;                        // ~world units per pixel
            cam.tx += (-d.x*rx + d.y*ux) * sc;
            cam.ty += ( d.y*uy) * sc;
            cam.tz += (-d.x*rz + d.y*uz) * sc;
            cam.cx = cam.tx; cam.cy = cam.ty; cam.cz = cam.tz;  // no lerp lag while panning
            focus_addr = 0;                                     // manual pan drops focus-tracking
        } else if (!follow) {                                   // left-drag orbits
            cam.yaw -= d.x * 0.008f;
            cam.pitch += d.y * 0.008f;
            cam.pitch = cam.pitch > 1.50f ? 1.50f : (cam.pitch < -1.50f ? -1.50f : cam.pitch);
        } else {
            cam.pitch += d.y * 0.008f;                          // follow: pitch only
            cam.pitch = cam.pitch > 1.50f ? 1.50f : (cam.pitch < -1.50f ? -1.50f : cam.pitch);
        }
    }
    // Follow mode: lock the orbit yaw to the ship's heading so its nose points "into"
    // the view (heading from the forward vector's X/Z; flip the sign to taste).
    if (follow && view.player_orient_valid)
        cam.yaw = atan2f(view.player_fwd_x, view.player_fwd_z);
    if (hov) {                                         // wheel zooms (dolly)
        float wh = ImGui::GetIO().MouseWheel;
        if (wh != 0.0f) cam.dist *= powf(0.8f, wh);    // a touch faster (range now 0.1..600 km)
        // min 0.1 km so small ships (~0.05 km) can fill the view: rpx = ext/depth*focal
        // (above the 0.02 km GPU near-plane / 0.05 km projection cutoff).
        cam.dist = cam.dist < 0.1f ? 0.1f : (cam.dist > 600.0f ? 600.0f : cam.dist);
    }
    // Focus-tracking: keep the look-at target on the dbl-clicked object as it moves
    // (the lerp below smooths the chase). Drop it if the object left the sector.
    if (focus_addr) {
        const x3::ObjInfo* fo = nullptr;
        for (const auto& o : view.objects) if (o.addr == focus_addr) { fo = &o; break; }
        if (fo) {
            const float Uf = x3::off::UNITS_PER_KM;
            cam.tx =  (fo->x - view.player_x) / Uf;
            cam.ty = -(fo->y - view.player_y) / Uf;   // match KX/KY/KZ screen convention
            cam.tz = -(fo->z - view.player_z) / Uf;
        } else {
            focus_addr = 0;
        }
    }
    // Ease the look-at center toward its target (frame-rate independent lerp).
    {
        float lf = 1.0f - powf(0.0025f, ImGui::GetIO().DeltaTime);
        cam.cx += (cam.tx - cam.cx) * lf;
        cam.cy += (cam.ty - cam.cy) * lf;
        cam.cz += (cam.tz - cam.cz) * lf;
    }

    dl->PushClipRect(p0, p1, true);
    dl->AddRectFilled(p0, p1, IM_COL32(6, 14, 22, g_force_opaque ? 255 : 234));  // near-solid; fully opaque when game isn't clearing (no trails)

    const float U = x3::off::UNITS_PER_KM;
    auto KX = [&](int32_t a){ return (a - view.player_x) / U; };
    auto KY = [&](int32_t a){ return -(a - view.player_y) / U; };   // engine Y is down; flip for screen-up
    auto KZ = [&](int32_t a){ return -(a - view.player_z) / U; };   // engine Z forward; flip front/back
    // Project a world km-point relative to the (lerped) look-at center.
    auto PROJ = [&](float wx, float wy, float wz, ImVec2& s, float& d) {
        return project3d(cam, ctr, focal, wx - cam.cx, wy - cam.cy, wz - cam.cz, s, d);
    };

    // GPU hull placement (matches project3d's basis so hulls sit on their blips).
    auto build_view = [&](float Vm[16]) {
        float cp=cosf(cam.pitch), sp=sinf(cam.pitch), cyw=cosf(cam.yaw), syw=sinf(cam.yaw);
        float ex=cam.dist*cp*syw, ey=cam.dist*sp, ez=cam.dist*cp*cyw;     // eye rel target
        float fx=-ex,fy=-ey,fz=-ez,fl=sqrtf(fx*fx+fy*fy+fz*fz); if(fl<1e-6f)fl=1; fx/=fl;fy/=fl;fz/=fl;
        float rx=-fz,ry=0,rz=fx,rl=sqrtf(rx*rx+rz*rz); if(rl<1e-6f){rx=1;rz=0;rl=1;} rx/=rl;rz/=rl;
        float ux=ry*fz-rz*fy, uy=rz*fx-rx*fz, uz=rx*fy-ry*fx;
        Vm[0]=rx; Vm[4]=ry; Vm[8]=rz;  Vm[12]=-(rx*ex+ry*ey+rz*ez);      // column-major view
        Vm[1]=ux; Vm[5]=uy; Vm[9]=uz;  Vm[13]=-(ux*ex+uy*ey+uz*ez);
        Vm[2]=-fx;Vm[6]=-fy;Vm[10]=-fz;Vm[14]=(fx*ex+fy*ey+fz*ez);
        Vm[3]=0; Vm[7]=0; Vm[11]=0; Vm[15]=1;
    };
    // body -> flipped-km relative to the look-at center, using the LIVE root orient
    // (Rl, app row-major) + live root pos. Flip = diag(1,-1,-1) == KX/KY/KZ's sign.
    auto build_model = [&](const float Rl[9], int32_t ox, int32_t oy, int32_t oz, float Mm[16]) {
        float tx = ( (float)(ox - view.player_x))/U - cam.cx;
        float ty = (-(float)(oy - view.player_y))/U - cam.cy;
        float tz = (-(float)(oz - view.player_z))/U - cam.cz;
        float s = 1.0f/U; const float F[3]={1.0f,-1.0f,-1.0f};
        for (int c=0;c<3;++c){ for (int r=0;r<3;++r) Mm[c*4+r]=F[r]*Rl[r*3+c]*s; Mm[c*4+3]=0; }
        Mm[12]=tx; Mm[13]=ty; Mm[14]=tz; Mm[15]=1;
    };

    // Homeworld-style sensor sphere: a dim, soft dark-blue shell at scanner range,
    // drawn first so the grid + objects sit in front of it.
    if (view.scanner_range > 0) {
        float r = (float)view.scanner_range / U;
        ImVec2 c; float d0;
        if (PROJ(0, 0, 0, c, d0) && d0 > 0.2f) {           // soft volume + rim (no hard edge)
            float sr = r / d0 * focal;                     // apparent screen radius
            if (sr > 4.0f && sr < 9000.0f) {
                dl->AddCircleFilled(c, sr, IM_COL32(22, 52, 116, 16), 64);
                for (int k = 0; k < 7; ++k) {
                    int a = 44 - k * 7;
                    dl->AddCircle(c, sr - k, IM_COL32(46, 96, 190, a < 0 ? 0 : a), 80, 1.5f);
                }
            }
        }
        // faint wireframe great circles to sell the 3D shell
        ImU32 wc = IM_COL32(50, 100, 190, 30);
        auto great = [&](float ax, float ay, float az, float bx, float by, float bz) {
            ImVec2 prev; bool has = false; float d;
            for (int i = 0; i <= 48; ++i) {
                float t = i / 48.0f * 6.2831853f, ca = cosf(t), sa = sinf(t); ImVec2 s;
                if (PROJ(r*(ax*ca+bx*sa), r*(ay*ca+by*sa), r*(az*ca+bz*sa), s, d)) {
                    if (has) dl->AddLine(prev, s, wc, 1.0f);
                    prev = s; has = true;
                } else has = false;
            }
        };
        great(1,0,0, 0,0,1);   // equator (ecliptic plane)
        great(1,0,0, 0,1,0);   // vertical
        great(0,0,1, 0,1,0);   // vertical
    }

    // ecliptic grid on y=0 (wide, coarse cells for context)
    {
        const float G = 200.0f, step = 40.0f; ImU32 gc = IM_COL32(40, 70, 110, 90);
        for (float g = -G; g <= G + 0.1f; g += step) {
            ImVec2 a, b; float d;
            if (PROJ(g, 0, -G, a, d) && PROJ(g, 0,  G, b, d)) dl->AddLine(a, b, gc, 1.0f);
            if (PROJ(-G, 0, g, a, d) && PROJ( G, 0, g, b, d)) dl->AddLine(a, b, gc, 1.0f);
        }
    }
    // objects: project, depth-sort far->near, draw drop line + glow + dot/hull + label
    struct PObj { float depth; ImVec2 s, ground; bool gok; const x3::ObjInfo* o; HullCache* hull; float rpx; };
    std::vector<PObj> ps; ps.reserve(view.objects.size());
    x3::Mem emem = make_engine_mem();
    for (const auto& o : view.objects) {
        if (!obj_passes_filter(o, map3d_rel, map3d_type)) continue;
        ImVec2 s; float d;
        if (!PROJ(KX(o.x), KY(o.y), KZ(o.z), s, d)) continue;
        PObj e; e.depth = d; e.s = s; e.o = &o; e.hull = nullptr; e.rpx = 0; float gd;
        e.gok = PROJ(KX(o.x), 0, KZ(o.z), e.ground, gd);
        // Hull promotion: read each object's finest mesh once (cached), then show it as a
        // hull when big enough on screen, else a dot. Skip huge Sun/Planet backdrops.
        bool hullable = (o.type != x3::off::TYPE_SUN && o.type != x3::off::TYPE_PLANET);
        uint32_t root = 0, mid = 0;
        if (hullable && x3::read_object_root(emem, o.addr, root)) {
            emem.u32(root + 0x12c, mid);
            HullCache* hc = hull_ensure(o.addr, mid, hull_budget);
            if (hc) { e.rpx = hc->extent_km / d * focal; if (e.rpx > 6.0f) e.hull = hc; }
        }
        ps.push_back(e);
    }
    std::sort(ps.begin(), ps.end(), [](const PObj& a, const PObj& b){ return a.depth > b.depth; });

    // GPU hull pass: emit the callback now (renders UNDER the dots/labels added below),
    // but g_solids is filled in the draw loop and read at render time.
    float Vm[16], Pm[16];
    build_view(Vm);
    meshgl::m_perspective(Pm, 0.9f, avail.x / avail.y, 0.02f, 50000.0f);
    {
        ImGuiIO& io = ImGui::GetIO();
        float fbx=io.DisplayFramebufferScale.x, fby=io.DisplayFramebufferScale.y;
        int fbh=(int)(io.DisplaySize.y*fby);
        meshgl::g_svx=(int)(p0.x*fbx); meshgl::g_svw=(int)(avail.x*fbx);
        meshgl::g_svh=(int)(avail.y*fby); meshgl::g_svy=fbh-(int)(p1.y*fby);
    }
    if (meshgl::g_ok) {
        dl->AddCallback(meshgl::solid_cb, nullptr);
        dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }
    for (const auto& e : ps) {
        const auto& o = *e.o; ImU32 col = faction_color(o);
        if (e.gok) {
            dl->AddLine(e.ground, e.s, (col & 0x00FFFFFFu) | 0x55000000u, 1.0f);
            dl->AddCircleFilled(e.ground, 1.6f, (col & 0x00FFFFFFu) | 0x66000000u); // ground tick
        }
        float rad = 70.0f / e.depth; rad = rad < 1.5f ? 1.5f : (rad > 9.0f ? 9.0f : rad);
        if (e.hull && meshgl::g_ok) {                        // solid holographic hull
            uint32_t root = 0; float Rl[9] = {1,0,0, 0,1,0, 0,0,1};
            if (x3::read_object_root(emem, o.addr, root)) x3::read_node_orient(emem, root, 0x3c, Rl);
            meshgl::SolidItem si; si.h = &e.hull->gpu;
            float Mm[16], MV[16];
            build_model(Rl, o.x, o.y, o.z, Mm);
            meshgl::m_mul(MV, Vm, Mm);
            meshgl::m_mul(si.mvp, Pm, MV);
            for (int k=0;k<16;++k) si.mv[k]=MV[k];
            si.col[0]=((col)&0xff)/255.0f; si.col[1]=((col>>8)&0xff)/255.0f;
            si.col[2]=((col>>16)&0xff)/255.0f; si.col[3]=0.85f;
            meshgl::g_solids.push_back(si);
        } else {                                             // blip dot
            dl->AddCircleFilled(e.s, rad*2.4f, (col & 0x00FFFFFFu) | 0x22000000u);
            dl->AddCircleFilled(e.s, rad, col);
        }
        if (o.id == g_sel_id) {                              // four-corner selection box
            float r = (e.hull ? std::max(e.rpx, 10.0f) : 9.0f) + 4.0f;
            ImU32 bc = IM_COL32(255,255,255,235); float L = r*0.5f; ImVec2 c = e.s;
            dl->AddLine(ImVec2(c.x-r,c.y-r), ImVec2(c.x-r+L,c.y-r), bc, 1.6f);
            dl->AddLine(ImVec2(c.x-r,c.y-r), ImVec2(c.x-r,c.y-r+L), bc, 1.6f);
            dl->AddLine(ImVec2(c.x+r,c.y-r), ImVec2(c.x+r-L,c.y-r), bc, 1.6f);
            dl->AddLine(ImVec2(c.x+r,c.y-r), ImVec2(c.x+r,c.y-r+L), bc, 1.6f);
            dl->AddLine(ImVec2(c.x-r,c.y+r), ImVec2(c.x-r+L,c.y+r), bc, 1.6f);
            dl->AddLine(ImVec2(c.x-r,c.y+r), ImVec2(c.x-r,c.y+r-L), bc, 1.6f);
            dl->AddLine(ImVec2(c.x+r,c.y+r), ImVec2(c.x+r-L,c.y+r), bc, 1.6f);
            dl->AddLine(ImVec2(c.x+r,c.y+r), ImVec2(c.x+r,c.y+r-L), bc, 1.6f);
            // orientation gizmo (selected only, toggle): forward/up/right axes so heading
            // AND roll are directly visible. Axes are Rl*local-axis, aligned with the hull.
            uint32_t sroot = 0; float SR[9];
            if (show_axes && x3::read_object_root(emem, o.addr, sroot) && x3::read_node_orient(emem, sroot, 0x3c, SR)) {
                float len = e.depth * 0.10f; const float Fp[3] = {1.0f,-1.0f,-1.0f};
                auto axis = [&](int cc, ImU32 col2) {        // cc = column = local axis index
                    float ax=SR[0*3+cc], ay=SR[1*3+cc], az=SR[2*3+cc];
                    ImVec2 ts; float td;
                    if (PROJ(KX(o.x)+Fp[0]*ax*len, KY(o.y)+Fp[1]*ay*len, KZ(o.z)+Fp[2]*az*len, ts, td))
                        dl->AddLine(c, ts, col2, 1.8f);
                };
                axis(0, IM_COL32(255,110,110,200));          // right   = red
                axis(1, IM_COL32(255,230, 90,235));          // up      = yellow (roll)
                axis(2, IM_COL32( 90,200,255,235));          // forward = cyan
            }
        }
        bool lbl = (o.type==x3::off::TYPE_STATION || o.type==x3::off::TYPE_FACTORY ||
                    o.type==x3::off::TYPE_GATE || o.type==x3::off::TYPE_SUN ||
                    o.type==x3::off::TYPE_PLANET);
        if (lbl && !o.name.empty())
            dl->AddText(ImVec2(e.s.x + rad + 3, e.s.y - 6),
                        IM_COL32(220, 225, 235, 200), o.name.c_str());
    }
    // player marker at origin
    {
        ImVec2 s; float d;
        if (PROJ(0, 0, 0, s, d)) {
            dl->AddCircleFilled(s, 5.0f, IM_COL32(120, 255, 160, 255));
            dl->AddCircle(s, 8.0f, IM_COL32(120, 255, 160, 160), 0, 1.5f);
            dl->AddText(ImVec2(s.x + 10, s.y - 6), IM_COL32(150, 255, 180, 230), "YOU");
        }
    }
    dl->PopClipRect();

    // hover: nearest projected object -> tooltip; right-click -> context menu.
    static x3::ObjInfo ctx_obj; static bool ctx_have = false;
    if (hov) {
        ImVec2 m = ImGui::GetIO().MousePos;
        const x3::ObjInfo* best = nullptr; float bd2 = 16.0f * 16.0f;
        for (const auto& e : ps) {
            float dx = e.s.x - m.x, dy = e.s.y - m.y, d2 = dx*dx + dy*dy;
            if (d2 < bd2) { bd2 = d2; best = e.o; }
        }
        if (best) {
            const char* rel = relation_label(best->relation);
            ImGui::BeginTooltip();
            ImGui::TextColored(ImColor(faction_color(*best)), "%s  (%s%s%s)",
                               best->name.empty() ? "(unnamed)" : best->name.c_str(),
                               x3::type_label(best->type), *rel ? ", " : "", rel);
            ImGui::Text("id %u   type %d/%d", best->id, (int)best->type, (int)best->subtype);
            if (best->dist >= 0) ImGui::Text("dist %.1f km", best->dist / U);
            ImGui::Text("pos %.1f, %.1f, %.1f km", best->x / U, best->y / U, best->z / U);
            ImGui::EndTooltip();
            // Left-click (not an orbit drag) selects/targets the object under the cursor.
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                if (fabsf(dd.x) + fabsf(dd.y) < 4.0f) select_target(*best);
            }
            // Double-click eases the camera to focus on the object AND tracks it (the
            // per-frame focus_addr block keeps the look-at center on it as it moves).
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                cam.tx = KX(best->x); cam.ty = KY(best->y); cam.tz = KZ(best->z);
                focus_addr = best->addr;
                select_target(*best);
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                ctx_obj = *best; ctx_have = true; ImGui::OpenPopup("obj3d_ctx");
            }
        }
    }
    if (ctx_have && ImGui::BeginPopup("obj3d_ctx")) {
        ImGui::TextDisabled("%s  (%s)", ctx_obj.name.empty() ? "(unnamed)" : ctx_obj.name.c_str(),
                            x3::type_label(ctx_obj.type));
        ImGui::Separator();
        char idbuf[32]; snprintf(idbuf, sizeof idbuf, "%u", ctx_obj.id);
        if (ImGui::MenuItem("Copy id")) ImGui::SetClipboardText(idbuf);
        command_menu_items(ctx_obj);
        ImGui::EndPopup();
    }

    if (!view.sector_name.empty())
        dl->AddText(ImVec2(p0.x + 6, p0.y + 4), IM_COL32(180, 200, 220, 210),
                    view.sector_name.c_str());

    ImGui::End();
}

// ---- mesh preview: wireframe of the selected object, read from CPU model data
// and drawn with our own projection (no engine render calls, no GL state). ------
static float g_mesh_yaw = 0.7f, g_mesh_pitch = 0.3f, g_mesh_dist = 3.2f;
static int   g_mesh_lod = 2;   // 0 = finest (slow), higher = coarser (fast)

static void mesh_preview_panel(void)
{
    ImGui::SetNextWindowSize(ImVec2(380, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(g_force_opaque ? 1.0f : 0.32f);  // translucent holo viewer
    if (!ImGui::Begin("x3ui - mesh preview")) { ImGui::End(); return; }

    // Re-read geometry only when the selection changes (it's static per object).
    static uint32_t cached = 0;
    static int      cached_lod = -1;
    static x3::MeshWire wire;
    static float cx = 0, cy = 0, cz = 0, extent = 1;
    if (g_sel_addr && g_sel_alive && (g_sel_addr != cached || g_mesh_lod != cached_lod)) {
        wire = x3::read_object_wireframe(make_engine_mem(), g_sel_addr, 512, 200000, 600000, g_mesh_lod, 1000);
        cached = g_sel_addr; cached_lod = g_mesh_lod;
        if (wire.ok) {
            float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
            for (size_t i = 0; i + 2 < wire.verts.size(); i += 3)
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], wire.verts[i + k]);
                    hi[k] = std::max(hi[k], wire.verts[i + k]);
                }
            cx = (lo[0]+hi[0])*0.5f; cy = (lo[1]+hi[1])*0.5f; cz = (lo[2]+hi[2])*0.5f;
            extent = std::max(std::max(hi[0]-lo[0], hi[1]-lo[1]), hi[2]-lo[2]) * 0.5f;
            if (extent < 1e-4f) extent = 1.0f;
            // Upload the normalized line geometry to the GPU once (per selection/LOD).
            if (!meshgl::g_tried) meshgl::init();
            if (meshgl::g_ok) {
                const float s = 1.0f / extent;
                std::vector<float> nv(wire.verts.size());
                for (size_t i = 0; i + 3 <= wire.verts.size(); i += 3) {
                    nv[i+0] = (wire.verts[i+0] - cx) * s;
                    nv[i+1] = (wire.verts[i+1] - cy) * s;
                    nv[i+2] = (wire.verts[i+2] - cz) * s;
                }
                meshgl::upload(nv, wire.edges);
            }
        }
    }
    if (!g_sel_addr)        { ImGui::TextDisabled("left-click an object to select it"); ImGui::End(); return; }
    if (!g_sel_alive)       { ImGui::TextDisabled("#%u not in this sector", g_sel_id);  ImGui::End(); return; }
    if (!wire.ok)           { ImGui::TextDisabled("no mesh for #%u (not loaded?)", g_sel_id); ImGui::End(); return; }

    static bool spin = true;
    static bool mp_cfg = false;
    ImGui::Text("#%u   %zu verts  %zu tris", g_sel_id, wire.vcount(), wire.edges.size() / 6);
    ImGui::SameLine();
    if (ImGui::SmallButton(mp_cfg ? "settings -" : "settings +")) mp_cfg = !mp_cfg;
    if (mp_cfg) {
        ImGui::BeginChild("mp_cfg", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 1.4f), true);
        ImGui::Checkbox("spin", &spin);
        ImGui::SameLine(); ImGui::SetNextItemWidth(70); ImGui::SliderInt("LOD", &g_mesh_lod, 0, 6);
        ImGui::SameLine(); ImGui::TextDisabled("drag · wheel");
        ImGui::EndChild();
    }

    ImVec2 av = ImGui::GetContentRegionAvail();
    if (av.x < 32 || av.y < 32) { ImGui::End(); return; }
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1 = ImVec2(p0.x + av.x, p0.y + av.y);
    ImVec2 ctr = ImVec2(p0.x + av.x * 0.5f, p0.y + av.y * 0.5f);
    float focal = av.y * 0.5f / tanf(0.45f);
    ImGui::InvisibleButton("meshcanvas", av);
    if (ImGui::IsItemActive()) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        g_mesh_yaw -= d.x * 0.01f; g_mesh_pitch += d.y * 0.01f; spin = false;
        g_mesh_pitch = g_mesh_pitch > 1.5f ? 1.5f : (g_mesh_pitch < -1.5f ? -1.5f : g_mesh_pitch);
    }
    if (spin) g_mesh_yaw += ImGui::GetIO().DeltaTime * 0.5f;
    if (ImGui::IsItemHovered()) {
        float wh = ImGui::GetIO().MouseWheel;
        if (wh != 0.0f) g_mesh_dist *= powf(0.85f, wh);
        g_mesh_dist = g_mesh_dist < 1.4f ? 1.4f : (g_mesh_dist > 20.0f ? 20.0f : g_mesh_dist);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, p1, true);
    // no backdrop: the mesh floats holographically over the scene (per design)

    if (meshgl::g_ok) {
        // GPU path: projection happens on the GPU via an MVP uniform; one draw call.
        float cp = cosf(g_mesh_pitch), sp = sinf(g_mesh_pitch);
        float cyw = cosf(g_mesh_yaw), syw = sinf(g_mesh_yaw);
        float ex = g_mesh_dist*cp*syw, ey = g_mesh_dist*sp, ez = g_mesh_dist*cp*cyw;
        float V[16], P[16];
        meshgl::m_lookat(V, ex, ey, ez);
        meshgl::m_perspective(P, 0.9f, av.x / av.y, 0.02f, 1000.0f);
        meshgl::m_mul(meshgl::g_dp.mvp, P, V);
        meshgl::g_dp.col[0] = 120/255.0f; meshgl::g_dp.col[1] = 200/255.0f;
        meshgl::g_dp.col[2] = 1.0f;       meshgl::g_dp.col[3] = 0.55f;
        ImGuiIO& io = ImGui::GetIO();
        float fx = io.DisplayFramebufferScale.x, fy = io.DisplayFramebufferScale.y;
        int fbh = (int)(io.DisplaySize.y * fy);
        meshgl::g_dp.vx = (int)(p0.x * fx);
        meshgl::g_dp.vw = (int)(av.x  * fx);
        meshgl::g_dp.vh = (int)(av.y  * fy);
        meshgl::g_dp.vy = fbh - (int)(p1.y * fy);
        dl->AddCallback(meshgl::draw_cb, nullptr);
        dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);   // restore ImGui GL state
    } else {
        // CPU fallback (GL entrypoints unavailable): project each unique vertex once,
        // AA-off lines.
        Camera3D pc; pc.yaw = g_mesh_yaw; pc.pitch = g_mesh_pitch; pc.dist = g_mesh_dist;
        const float s = 1.0f / extent;
        size_t vc = wire.verts.size() / 3;
        static std::vector<ImVec2> sp2; static std::vector<unsigned char> vis;
        sp2.resize(vc); vis.resize(vc);
        for (size_t i = 0; i < vc; ++i) {
            float dep;
            vis[i] = project3d(pc, ctr, focal, (wire.verts[i*3+0] - cx) * s,
                               (wire.verts[i*3+1] - cy) * s, (wire.verts[i*3+2] - cz) * s, sp2[i], dep) ? 1 : 0;
        }
        bool aa = (dl->Flags & ImDrawListFlags_AntiAliasedLines) != 0;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedLines;
        for (size_t e = 0; e + 1 < wire.edges.size(); e += 2) {
            uint32_t i0 = wire.edges[e], i1 = wire.edges[e + 1];
            if (i0 < vc && i1 < vc && vis[i0] && vis[i1])
                dl->AddLine(sp2[i0], sp2[i1], IM_COL32(120, 200, 255, 110), 1.0f);
        }
        if (aa) dl->Flags |= ImDrawListFlags_AntiAliasedLines;
    }
    dl->PopClipRect();
    ImGui::End();
}

static void overlay_draw(void)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Did the game clear the color buffer for the frame we're about to composite
    // onto? (Its clears happened during its render, before this SwapWindow.) If
    // not (docked/menu: W_RectFill dStub), draw panels opaque so nothing trails.
    g_force_opaque = !g_color_cleared;
    g_color_cleared = false;                 // reset for the next frame
    ImGuiStyle& style = ImGui::GetStyle();
    const float saved_winbg_a = style.Colors[ImGuiCol_WindowBg].w;
    if (g_force_opaque)                        // covers windows that don't set alpha explicitly
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;

    // Draw ImGui's cursor only while hovering the overlay; over the game world
    // draw nothing so the game's own cursor shows through.
    ImGui::GetIO().MouseDrawCursor = ImGui::GetIO().WantCaptureMouse;

    ImGui::SetNextWindowBgAlpha(g_force_opaque ? 1.0f : 0.40f);
    if (ImGui::Begin("x3ui")) {
        ImGui::Text("x3ui overlay  —  %.1f FPS", ImGui::GetIO().Framerate);
        ImGui::Separator();
        ImGui::TextDisabled("ScrollLock: show/hide");

        ImGui::SeparatorText("engine bridge (experimental)");
        // PROOF #1 (safe, no game effect): queue an existing MSCI entry. A
        // NONZERO handle means the engine accepted our call & found the script.
        if (ImGui::Button("MSCI fire (NotifyLaserLow)"))
            g_msci_async_h = run_script_async(nullptr, 0, 0x96, "NotifyLaserLow", 1, 0);
        ImGui::SameLine();
        ImGui::Text("-> handle %d", g_msci_async_h);

        // PROOF #2 (read-only): sync-call a value-returning entry & log result.
        if (ImGui::Button("MSCI read (CurObjInSector)")) {
            void *cockpit = *(void **)0x0837a104;
            g_msci_sync_v = run_script_sync(cockpit, 0, 0x137,
                                            "GetCurrentObjectIDInSector", 1, 0);
            fprintf(stderr, "[x3ui] MSCI CurObjInSector -> %d (0x%x)\n",
                    g_msci_sync_v, g_msci_sync_v);
        }
        ImGui::SameLine();
        ImGui::Text("-> %d", g_msci_sync_v);

        // Call our custom credits script: probe its id by name, then sync-run it.
        static char entry[64] = "x3uimoney";
        ImGui::InputText("script name", entry, sizeof entry);
        if (ImGui::Button("Get Credits (MSCI)")) {
            g_money_id = msci_find_id(entry);
            if (g_money_id >= 0)
                g_credits = run_script_sync(*(void **)0x0837a104, 0,
                                            g_money_id, entry, 1, 0);
            fprintf(stderr, "[x3ui] credits script '%s' id=%d -> %d\n",
                    entry, g_money_id, g_credits);
        }
        ImGui::SameLine();
        if (g_money_id < 0) ImGui::TextDisabled("id=? (not found)");
        else                ImGui::Text("id=%d  Credits=%d", g_money_id, g_credits);
    }
    ImGui::End();

    state_bridge_window();
    ship_window();
    map_panel();
    map3d_panel();
    mesh_preview_panel();
    text_trace_window();

    style.Colors[ImGuiCol_WindowBg].w = saved_winbg_a;   // restore translucency

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// ---- interposed: the game's color/depth clears ---------------------------
X3UI_EXPORT void glClear(GLbitfield mask)
{
    if (!real_Clear)
        real_Clear = (pfn_Clear)dlsym(RTLD_NEXT, "glClear");
    if (mask & GL_COLOR_BUFFER_BIT) g_color_cleared = true;
    if (real_Clear) real_Clear(mask);
}

// ---- interposed: per-frame buffer swap -----------------------------------
X3UI_EXPORT void SDL_GL_SwapWindow(SDL_Window *window)
{
    resolve_originals();

    if (!is_game_process()) {       // launcher / wrapper: never touch
        real_SwapWindow(window);
        return;
    }

    if (!g_inited && SDL_GL_GetCurrentContext()) {
        overlay_init(window);
        mem_open();                                 // open /proc/self/mem for reads
        x3::start_debug_server(make_engine_mem());  // localhost:7373, read-only
    }

    // Service the debug socket every frame on the main thread — even when the
    // overlay is hidden — so socket queries don't hang while ScrollLock is off.
    if (g_inited) {
        x3::debug_server_pump();
        drive_tick();                  // per-frame autopilot gate driver (keeps executor advancing)
    }

    if (g_inited && g_ui_visible)
        overlay_draw();

    real_SwapWindow(window);
}

// Feed one SDL event to ImGui. The mouse wheel is special-cased: sdl2-compat
// (over SDL3) leaves wheel.preciseX/preciseY uninitialized, and ImGui's SDL2
// backend reads those floats -> astronomically large scroll values (table snaps
// to top). Use the integer wheel deltas, which sdl2-compat sets correctly.
static void feed_imgui_event(const SDL_Event &e)
{
    if (e.type == SDL_MOUSEWHEEL) {
        ImGui::GetIO().AddMouseWheelEvent(-(float)e.wheel.x, (float)e.wheel.y);
        return;
    }
    ImGui_ImplSDL2_ProcessEvent(&e);
}

// ---- interposed: event-queue pump ----------------------------------------
// X3 reads input by PEEK-ing the queue (SDL_PeepEvents action=PEEK) and acting
// on it, only GET-ing afterwards to clear it — so filtering GET events is too
// late. Right after the real pump fills the queue we decide per category:
//   - mouse: if ImGui wants it (cursor over an overlay window) we DRAIN it (GET)
//     so the game gets none; otherwise we only PEEK it (feed ImGui for hover
//     tracking) and leave it in the queue for the game — clicking the game still works.
//   - keyboard: drained only while an overlay text box is focused (game gets none);
//     otherwise left for the game so hotkeys work. ScrollLock toggles the overlay.
// We NEVER re-inject events (no SDL_PushEvent), so there is nothing to leak.
X3UI_EXPORT void SDL_PumpEvents(void)
{
    resolve_originals();
    if (real_PumpEvents) real_PumpEvents();

    // Force a synthetic held key into SDL's keyboard STATE right after SDL refreshed it from
    // the OS — this is the clean "fake a keypress" the game (which polls GetKeyboardState) sees.
    if (g_synth_hold > 0) {
        Uint8 *ks = (Uint8 *)SDL_GetKeyboardState(NULL);
        if (ks) {
            if (g_synth_sc)    ks[g_synth_sc] = 1;
            if (g_synth_shift) { ks[SDL_SCANCODE_LSHIFT] = 1;
                                 SDL_SetModState((SDL_Keymod)(SDL_GetModState() | KMOD_LSHIFT)); }
        }
        g_synth_hold--;
    }

    if (!is_game_process() || !g_inited || !g_ui_visible || !real_PeepEvents)
        return;

    ImGuiIO &io = ImGui::GetIO();
    SDL_Event buf[64];

    // Mouse (0x400..0x403)
    if (io.WantCaptureMouse) {
        for (int guard = 0; guard < 64; ++guard) {     // drain: game gets none
            int n = real_PeepEvents(buf, 64, SDL_GETEVENT, SDL_MOUSEMOTION, SDL_MOUSEWHEEL);
            if (n <= 0) break;
            for (int i = 0; i < n; ++i) feed_imgui_event(buf[i]);
            if (n < 64) break;
        }
    } else {                                            // peek: ImGui tracks, game keeps
        int n = real_PeepEvents(buf, 64, SDL_PEEKEVENT, SDL_MOUSEMOTION, SDL_MOUSEWHEEL);
        for (int i = 0; i < n && i < 64; ++i) feed_imgui_event(buf[i]);
    }

    // Keyboard incl. text (0x300..0x303). Drained only while capturing text; the
    // ScrollLock toggle is then caught here, otherwise in the SDL_PeepEvents hook.
    if (io.WantCaptureKeyboard) {
        for (int guard = 0; guard < 64; ++guard) {
            int n = real_PeepEvents(buf, 64, SDL_GETEVENT, SDL_KEYDOWN, SDL_TEXTINPUT);
            if (n <= 0) break;
            for (int i = 0; i < n; ++i) {
                SDL_Event &e = buf[i];
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_SCROLLLOCK) {
                    g_ui_visible = !g_ui_visible;
                    fprintf(stderr, "[x3ui] overlay %s\n", g_ui_visible ? "shown" : "hidden");
                    continue;
                }
                feed_imgui_event(e);
            }
            if (n < 64) break;
        }
    }
}

// ---- interposed: event pump ----------------------------------------------
X3UI_EXPORT int SDL_PeepEvents(SDL_Event *events, int numevents,
                               SDL_eventaction action, Uint32 minType,
                               Uint32 maxType)
{
    resolve_originals();
    int n = real_PeepEvents(events, numevents, action, minType, maxType);

    if (!is_game_process() || !g_inited || action != SDL_GETEVENT)
        return n;

    int kept = 0;
    for (int i = 0; i < n; ++i) {
        SDL_Event &e = events[i];

        // ScrollLock toggles the overlay; consume it so the game never sees it.
        // When the overlay is visible, SDL_PumpEvents already handled the toggle
        // and all input routing; this mainly catches the hidden -> show press.
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_SCROLLLOCK) {
            g_ui_visible = !g_ui_visible;
            fprintf(stderr, "[x3ui] overlay %s\n", g_ui_visible ? "shown" : "hidden");
            continue;
        }

        // ImGui needs window events (resize/focus) for its display size. Input
        // (keyboard/mouse) is already routed in SDL_PumpEvents — don't re-process
        // it here, or re-injected events would be double-counted.
        if (e.type == SDL_WINDOWEVENT)
            ImGui_ImplSDL2_ProcessEvent(&e);

        events[kept++] = e;       // pass everything through to the game
    }

    // Inject synthetic events queued by the synthkey harness (e.g. a faked Shift+D so the
    // game's own input chain auto-docks). Deliver only those whose type is in the game's
    // requested [minType,maxType] range and that fit the buffer; keep the rest queued.
    if (g_synth_n > 0) {
        int out = 0;
        for (int s = 0; s < g_synth_n; ++s) {
            if (kept < numevents && g_synth[s].type >= minType && g_synth[s].type <= maxType)
                events[kept++] = g_synth[s];
            else
                g_synth[out++] = g_synth[s];
        }
        g_synth_n = out;
    }
    return kept;
}
