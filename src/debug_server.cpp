#include "debug_server.h"
#include "struct_offsets.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

namespace x3 {

// Game memory mutates on the main render thread. Walking those live structures
// from the server's background thread races with that mutation and can hand
// snprintf a corrupted name -> SIGSEGV. So the server thread does NO memory
// access itself: it posts the command here and blocks until the MAIN thread (via
// debug_server_pump(), called once per frame between renders) services it against
// a consistent snapshot. One request is in flight at a time.
namespace {
struct Handoff {
    std::mutex              m;
    std::condition_variable cv;
    bool        req_pending = false;
    bool        resp_ready  = false;
    std::string req;
    std::string resp;
    Mem         mem;
};
Handoff g_handoff;

// Server thread: hand the line to the main thread, wait (bounded) for the answer.
std::string dispatch_on_main(const std::string& line) {
    std::unique_lock<std::mutex> lk(g_handoff.m);
    g_handoff.req = line;
    g_handoff.resp.clear();
    g_handoff.resp_ready = false;
    g_handoff.req_pending = true;
    g_handoff.cv.notify_all();
    if (!g_handoff.cv.wait_for(lk, std::chrono::seconds(2),
                               [] { return g_handoff.resp_ready; })) {
        g_handoff.req_pending = false;
        return "err timeout (game not rendering?)";
    }
    return g_handoff.resp;
}
} // namespace

// Accepts "0x..", decimal, and sums like "0x1f6f8d10+0x6c" (so callers can write
// base+offset directly).
static uint32_t parse_addr(const std::string &s) {
    uint32_t total = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t plus = s.find('+', i);
        std::string term = (plus == std::string::npos) ? s.substr(i) : s.substr(i, plus - i);
        total += (uint32_t)strtoul(term.c_str(), nullptr, 0);
        if (plus == std::string::npos) break;
        i = plus + 1;
    }
    return total;
}

std::string handle_request(const Mem &mem, const std::string &line) {
    std::istringstream is(line);
    std::string op;
    is >> op;
    if (op.empty()) return "";

    auto u32 = [&](uint32_t a, uint32_t &o) -> bool { return mem.u32 && mem.u32(a, o); };
    auto i16lo = [](uint32_t w) { return (int)(int16_t)(w & 0xFFFF); };
    auto i16hi = [](uint32_t w) { return (int)(int16_t)(w >> 16); };
    char b[256];

    if (op == "ping") return "pong";

    if (op == "meshscene") {   // resolve the active render scene like mesh_render does
        uint32_t world = 0, view = 0, scene = 0;
        u32(0x082a0640, world);
        if (world) u32(world + 0x64, view);
        uint32_t node = 0; if (world) u32(world + 0x18, node);
        for (int si = 0; si < 64 && node && !scene; ++si) {
            uint32_t sc = 0; u32(node + 0x1c, sc);
            for (int sj = 0; sj < 64 && sc; ++sj) {
                uint32_t v = 0, fl = 0, vp2 = 0;
                u32(sc + 0x1c, v); u32(sc + 0x244, fl); u32(sc + 0x24c, vp2);
                if (v == view && (fl & 1) && vp2) { scene = sc; break; }
                uint32_t nx = 0; u32(sc, nx); sc = nx;
            }
            uint32_t nn = 0; u32(node, nn); node = nn;
        }
        uint32_t vp = 0, scf_w = 0, cx = 0, cy = 0, cz = 0, v0 = 0, v1 = 0, v2 = 0, v3 = 0;
        if (scene) { u32(scene + 0x24c, vp); u32(scene + 0x2c, cx); u32(scene + 0x30, cy); u32(scene + 0x34, cz); }
        if (view) u32(view + 0x2c, scf_w);
        if (vp) { u32(vp, v0); u32(vp + 4, v1); u32(vp + 8, v2); u32(vp + 12, v3); }
        float scf; memcpy(&scf, &scf_w, 4);
        snprintf(b, sizeof b,
                 "ok world=0x%x view=0x%x scene=0x%x scale=%g vp=0x%x[%d,%d,%d,%d] cam=%d,%d,%d",
                 world, view, scene, scf, vp,
                 (int)v0, (int)v1, (int)v2, (int)v3, (int)cx, (int)cy, (int)cz);
        return b;
    }

    if (op == "wire") {   // diagnose the mesh wireframe chain:  wire <obj_addr>
        std::string at; is >> at;
        uint32_t a = parse_addr(at), id = 0;
        u32(a + 0x12c, id);
        MeshWire w = read_mesh_wireframe(mem, a);
        snprintf(b, sizeof b, "ok addr=0x%x id=%d | wire.ok=%d v=%zu tris=%zu",
                 a, (int)id, (int)w.ok, w.vcount(), w.edges.size() / 6);
        return b;
    }

    if (op == "owire") {   // whole-object wireframe: owire <obj_addr> [lod]
        std::string at; is >> at;
        uint32_t a = parse_addr(at);
        int lod = 0; is >> lod;                  // optional LOD level (0 = finest)
        ObjWireStats st;
        MeshWire w = read_object_wireframe(mem, a, 512, 200000, 600000, lod, 1000, &st);
        snprintf(b, sizeof b,
                 "ok addr=0x%x ok=%d v=%zu tris=%zu | nodes=%zu resolved=%zu kept=%zu dropG=%zu sumv=%zu maxc=%.1f",
                 a, (int)w.ok, w.vcount(), w.edges.size() / 6,
                 st.nodes, st.resolved, st.kept, st.dropped_guard, st.sum_resolved_v, (double)st.max_coord);
        return b;
    }

    if (op == "text") {   // resolve a {page,id} text ref:  text <page> <id>
        uint32_t page = 0, id = 0;
        is >> page >> id;
        std::string s = resolve_text(mem, page, id);
        snprintf(b, sizeof b, "ok page=%u id=%u \"%s\"", page, id, s.c_str());
        return b;
    }

    if (op == "rss") {   // in-process memory, for leak watching
        long vmrss = -1, vmsize = -1;
        FILE *fp = fopen("/proc/self/status", "r");
        if (fp) {
            char ln[256];
            while (fgets(ln, sizeof ln, fp)) {
                if (!strncmp(ln, "VmRSS:", 6))       sscanf(ln + 6, "%ld", &vmrss);
                else if (!strncmp(ln, "VmSize:", 7)) sscanf(ln + 7, "%ld", &vmsize);
            }
            fclose(fp);
        }
        snprintf(b, sizeof b, "ok VmRSS=%ld kB VmSize=%ld kB", vmrss, vmsize);
        return b;
    }

    if (op == "read") {
        std::string sa; is >> sa;
        uint32_t a = parse_addr(sa), v = 0;
        if (!u32(a, v)) return "err read";
        snprintf(b, sizeof b, "ok 0x%08x %d", v, (int32_t)v);
        return b;
    }
    if (op == "reads") {
        std::string sa; int n = 0; is >> sa >> n;
        uint32_t a = parse_addr(sa);
        if (n <= 0 || n > 256) n = 8;
        std::string r = "ok";
        for (int i = 0; i < n; ++i) {
            uint32_t v = 0;
            if (!u32(a + i * 4, v)) { r += " err"; break; }
            snprintf(b, sizeof b, " 0x%08x", v);
            r += b;
        }
        return r;
    }
    if (op == "bytes") {
        std::string sa; int n = 0; is >> sa >> n;
        uint32_t a = parse_addr(sa);
        if (n <= 0 || n > 256) n = 16;
        std::string r = "ok";
        for (int i = 0; i < n; i += 4) {
            uint32_t v = 0;
            if (!u32(a + i, v)) { r += " err"; break; }
            for (int k = 0; k < 4 && i + k < n; ++k) {
                snprintf(b, sizeof b, " %02x", (v >> (k * 8)) & 0xFF);
                r += b;
            }
        }
        return r;
    }
    if (op == "cstr") {
        std::string sa; int m = 64; is >> sa >> m;
        uint32_t a = parse_addr(sa);
        if (m <= 0 || m > 1024) m = 64;
        std::string s = (a && mem.cstr) ? mem.cstr(a, (size_t)m) : std::string();
        return "ok \"" + s + "\"";
    }
    if (op == "type") {
        std::string sa; is >> sa;
        uint32_t a = parse_addr(sa), w = 0;
        if (!u32(a + off::OBJ_TYPE, w)) return "err read";
        snprintf(b, sizeof b, "ok type=%d sub=%d", i16lo(w), i16hi(w));
        return b;
    }
    if (op == "obj") {
        std::string sa; is >> sa;
        uint32_t a = parse_addr(sa), id = 0, tw = 0, np = 0, tf = 0, x = 0, y = 0, z = 0;
        std::string nm;
        u32(a + off::OBJ_ID, id);
        u32(a + off::OBJ_TYPE, tw);
        if (u32(a + off::OBJ_NAME, np) && np && mem.cstr) nm = mem.cstr(np, 64);
        if (u32(a + off::OBJ_TRANSFORM, tf) && tf) {
            u32(tf + off::TF_X, x); u32(tf + off::TF_Y, y); u32(tf + off::TF_Z, z);
        }
        snprintf(b, sizeof b,
                 "ok addr=0x%08x id=%u type=%d sub=%d name=\"%s\" pos=%d,%d,%d",
                 a, id, i16lo(tw), i16hi(tw), nm.c_str(), (int32_t)x, (int32_t)y, (int32_t)z);
        return b;
    }
    if (op == "chain") {
        std::string sa; int lim = 6; is >> sa >> lim;
        uint32_t cur = parse_addr(sa);
        if (lim <= 0 || lim > 32) lim = 6;
        std::string r;
        for (int l = 0; l < lim && cur; ++l) {
            uint32_t tw = 0, np = 0; std::string nm;
            u32(cur + off::OBJ_TYPE, tw);
            if (u32(cur + off::OBJ_NAME, np) && np && mem.cstr) nm = mem.cstr(np, 48);
            snprintf(b, sizeof b, "lvl=%d addr=0x%08x type=%d name=\"%s\"",
                     l, cur, i16lo(tw), nm.c_str());
            if (l) r += "\n";
            r += b;
            uint32_t p = 0;
            if (!u32(cur + off::OBJ_PARENT, p)) break;
            cur = p;
        }
        return r.empty() ? "err" : r;
    }
    if (op == "children") {
        std::string sa; int lim = 16; is >> sa >> lim;
        uint32_t a = parse_addr(sa);
        if (lim <= 0 || lim > 256) lim = 16;
        uint32_t payload = 0, node = 0;
        u32(a + off::OBJ_PAYLOAD, payload);
        if (payload) u32(payload + off::SEC_LIST_HEAD, node);
        uint32_t sentinel = payload + off::SEC_LIST_SENTINEL;
        std::string body;
        int i = 0;
        for (; i < lim && node && node != sentinel; ++i) {
            uint32_t tw = 0, np = 0; std::string nm;
            u32(node + off::OBJ_TYPE, tw);
            if (u32(node + off::OBJ_NAME, np) && np && mem.cstr) nm = mem.cstr(np, 48);
            snprintf(b, sizeof b, "\ni=%d addr=0x%08x type=%d name=\"%s\"",
                     i, node, i16lo(tw), nm.c_str());
            body += b;
            uint32_t nx = 0;
            if (!u32(node + off::OBJ_NEXT, nx)) break;
            node = nx;
        }
        snprintf(b, sizeof b, "ok payload=0x%08x count=%d", payload, i);
        return std::string(b) + body;
    }
    if (op == "scene") {
        uint32_t scene = 0, player = 0, sector = 0, ptw = 0, stw = 0, np = 0, ns = 0;
        std::string pn, sn;
        u32(0x0837a0d0u, scene);
        if (scene) {
            u32(scene + off::SCENE_PLAYER_ANCHOR, player);
            u32(scene + off::SCENE_SECTOR, sector);
        }
        if (player) {
            u32(player + off::OBJ_TYPE, ptw);
            if (u32(player + off::OBJ_NAME, np) && np && mem.cstr) pn = mem.cstr(np, 48);
        }
        if (sector) {
            u32(sector + off::OBJ_TYPE, stw);
            if (u32(sector + off::OBJ_NAME, ns) && ns && mem.cstr) sn = mem.cstr(ns, 48);
        }
        snprintf(b, sizeof b,
                 "ok scene=0x%08x player=0x%08x ptype=%d pname=\"%s\" "
                 "sector=0x%08x stype=%d sname=\"%s\"",
                 scene, player, i16lo(ptw), pn.c_str(), sector, i16lo(stw), sn.c_str());
        return b;
    }
    if (op == "sector") {
        SectorView v = read_current_sector(mem);
        if (!v.in_sector) return "ok in_sector=0";
        snprintf(b, sizeof b,
                 "ok in_sector=1 addr=0x%08x name=\"%s\" count=%zu scanner_range=%lld",
                 v.sector_addr, v.sector_name.c_str(), v.objects.size(),
                 (long long)v.scanner_range);
        std::string r = b;
        int i = 0;
        for (const auto &o : v.objects) {
            if (i++ >= 256) break;
            // root transform node + its world-orient forward vector (+0x44/54/64, 16.16)
            // and right vector (+0x3c/4c/5c) so per-object heading is visible live.
            uint32_t root = 0; u32(o.addr + 0x6c, root);
            auto f16 = [&](uint32_t a) -> float { uint32_t v = 0; u32(a, v); return (int32_t)v / 65536.0f; };
            float fx = 0, fy = 0, fz = 0, rx = 0, ry = 0, rz = 0;
            if (root) {
                fx = f16(root + 0x44); fy = f16(root + 0x54); fz = f16(root + 0x64);
                rx = f16(root + 0x3c); ry = f16(root + 0x4c); rz = f16(root + 0x5c);
            }
            snprintf(b, sizeof b,
                     "\n  id=%u addr=0x%08x type=%d sub=%d rel=%d name=\"%s\" fwd=%.2f,%.2f,%.2f rgt=%.2f,%.2f,%.2f",
                     o.id, o.addr, (int)o.type, (int)o.subtype, (int)o.relation,
                     o.name.c_str(), fx, fy, fz, rx, ry, rz);
            r += b;
        }
        return r;
    }
    return "err unknown op";
}

static void server_thread(int port) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return;
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "[x3ui] debug server bind failed on port %d\n", port);
        close(srv);
        return;
    }
    listen(srv, 1);
    fprintf(stderr, "[x3ui] debug server listening on 127.0.0.1:%d (read-only)\n", port);

    for (;;) {
        int c = accept(srv, nullptr, nullptr);
        if (c < 0) continue;
        std::string acc;
        char rbuf[2048];
        for (;;) {
            ssize_t r = recv(c, rbuf, sizeof rbuf, 0);
            if (r <= 0) break;
            acc.append(rbuf, (size_t)r);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string ln = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                // ping answers on the server thread (no game memory) so it still
                // works — and reveals a stuck main thread — when the pump is wedged.
                std::string resp = (ln == "ping") ? std::string("pong")
                                                  : dispatch_on_main(ln);
                resp.push_back('\n');
                if (send(c, resp.data(), resp.size(), MSG_NOSIGNAL) < 0) { r = -1; break; }
            }
            if (r < 0) break;
        }
        close(c);
    }
}

// Main thread: service one pending request against live memory, between frames.
void debug_server_pump() {
    std::unique_lock<std::mutex> lk(g_handoff.m);
    if (!g_handoff.req_pending || g_handoff.resp_ready) return;
    std::string line = g_handoff.req;
    Mem mem = g_handoff.mem;
    lk.unlock();
    std::string r = handle_request(mem, line);   // safe: synchronized with the game
    lk.lock();
    g_handoff.resp = std::move(r);
    g_handoff.resp_ready  = true;
    g_handoff.req_pending = false;
    g_handoff.cv.notify_all();
}

void start_debug_server(const Mem &mem, int port) {
    static bool started = false;
    if (started) return;
    started = true;
    { std::lock_guard<std::mutex> lk(g_handoff.m); g_handoff.mem = mem; }
    std::thread(server_thread, port).detach();
}

} // namespace x3
