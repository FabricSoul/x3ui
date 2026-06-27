#pragma once
#include <map>
#include <string>
#include "sector_reader.h"

// A fake 32-bit address space: words at addresses, plus C-strings at addresses.
// Unset words read as "unreadable" (u32 returns false) — simulating bad pointers.
struct FakeMem {
    std::map<uint32_t, uint32_t> words;
    std::map<uint32_t, std::string> strings;
    std::map<uint64_t, std::string> texts;   // {page,id} -> resolved string

    void w(uint32_t addr, uint32_t val)            { words[addr] = val; }
    void s(uint32_t addr, const std::string& str)  { strings[addr] = str; }
    void t(uint32_t page, uint32_t id, const std::string& str) {
        texts[((uint64_t)page << 32) | id] = str;
    }

    x3::Mem mem(bool with_bytes = false) {
        x3::Mem m;
        auto* W = &words;
        auto* S = &strings;
        auto* T = &texts;
        if (with_bytes) {
            m.bytes = [W](uint32_t a, void* dst, size_t n) -> bool {
                uint8_t* d = (uint8_t*)dst;
                for (size_t i = 0; i < n; ++i) {           // assemble LE bytes from words (unset -> 0)
                    uint32_t aa = a + (uint32_t)i;
                    auto it = W->find(aa & ~3u);
                    uint32_t w = (it == W->end()) ? 0u : it->second;
                    d[i] = (uint8_t)(w >> ((aa & 3u) * 8));
                }
                return true;
            };
        }
        m.u32 = [W](uint32_t a, uint32_t& o) -> bool {
            auto it = W->find(a);
            if (it == W->end()) return false;
            o = it->second; return true;
        };
        m.cstr = [S](uint32_t a, size_t) -> std::string {
            auto it = S->find(a);
            return it == S->end() ? std::string() : it->second;
        };
        m.text = [T](uint32_t page, uint32_t id) -> std::string {
            auto it = T->find(((uint64_t)page << 32) | id);
            return it == T->end() ? std::string() : it->second;
        };
        return m;
    }
};
