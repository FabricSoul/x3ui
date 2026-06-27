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

static void resolve_originals(void)
{
    if (!real_SwapWindow)
        real_SwapWindow = (pfn_SwapWindow)dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");
    if (!real_PeepEvents)
        real_PeepEvents = (pfn_PeepEvents)dlsym(RTLD_NEXT, "SDL_PeepEvents");
    if (!real_PumpEvents)
        real_PumpEvents = (pfn_PumpEvents)dlsym(RTLD_NEXT, "SDL_PumpEvents");
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
    for (int id = 0; id < 0x800; ++id)
        if (script_exists(id, entry)) return id;
    return -1;
}

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
    // TODO(target-bridge): write the engine's current-target global so the game's
    // targeting brackets follow this object.
}

// Read-only panel: list the objects in the player's current sector.
static void map_panel(void)
{
    if (!ImGui::Begin("x3ui - sector map")) { ImGui::End(); return; }
    mem_open();

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
            if (ImGui::BeginPopupContextItem("row")) {       // right-click the row
                const char* rel = relation_label(o.relation);
                ImGui::TextDisabled("%s  (%s%s%s)", o.name.empty() ? "(unnamed)" : o.name.c_str(),
                                    x3::type_label(o.type), *rel ? ", " : "", rel);
                ImGui::Separator();
                char idbuf[32]; snprintf(idbuf, sizeof idbuf, "%u", o.id);
                if (ImGui::MenuItem("Copy id")) ImGui::SetClipboardText(idbuf);
                ImGui::BeginDisabled();
                ImGui::MenuItem("Move here");
                ImGui::MenuItem("Dock at");
                ImGui::MenuItem("Attack");
                ImGui::EndDisabled();
                ImGui::TextDisabled("(commands: coming with the script bridge)");
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
    ImGui::SetNextWindowBgAlpha(0.55f);               // map panel reads clearly (not washed out)
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
    dl->AddRectFilled(p0, p1, IM_COL32(6, 14, 22, 234));  // near-solid so the sensor map reads clearly

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
        ImGui::BeginDisabled();
        ImGui::MenuItem("Move here"); ImGui::MenuItem("Dock at"); ImGui::MenuItem("Attack");
        ImGui::EndDisabled();
        ImGui::TextDisabled("(commands: coming with the script bridge)");
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
    ImGui::SetNextWindowBgAlpha(0.32f);               // translucent holo viewer
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

    // Draw ImGui's cursor only while hovering the overlay; over the game world
    // draw nothing so the game's own cursor shows through.
    ImGui::GetIO().MouseDrawCursor = ImGui::GetIO().WantCaptureMouse;

    ImGui::SetNextWindowBgAlpha(0.40f);
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

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
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
    if (g_inited)
        x3::debug_server_pump();

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

    if (!is_game_process() || !g_inited || action != SDL_GETEVENT || n <= 0)
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
    return kept;
}
