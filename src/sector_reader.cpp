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
