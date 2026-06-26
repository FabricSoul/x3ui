#include "debug_server.h"
#include "struct_offsets.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

namespace x3 {

static uint32_t parse_addr(const std::string &s) {
    return (uint32_t)strtoul(s.c_str(), nullptr, 0);  // 0x.. or decimal
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
        snprintf(b, sizeof b, "ok in_sector=1 addr=0x%08x name=\"%s\" count=%zu",
                 v.sector_addr, v.sector_name.c_str(), v.objects.size());
        std::string r = b;
        int i = 0;
        for (const auto &o : v.objects) {
            if (i++ >= 128) break;
            snprintf(b, sizeof b, "\n  id=%u type=%d sub=%d name=\"%s\" pos=%d,%d,%d",
                     o.id, (int)o.type, (int)o.subtype, o.name.c_str(), o.x, o.y, o.z);
            r += b;
        }
        return r;
    }
    return "err unknown op";
}

static void server_thread(Mem mem, int port) {
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
                std::string resp = handle_request(mem, ln);
                resp.push_back('\n');
                if (send(c, resp.data(), resp.size(), MSG_NOSIGNAL) < 0) { r = -1; break; }
            }
            if (r < 0) break;
        }
        close(c);
    }
}

void start_debug_server(const Mem &mem, int port) {
    static bool started = false;
    if (started) return;
    started = true;
    std::thread(server_thread, mem, port).detach();
}

} // namespace x3
