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
