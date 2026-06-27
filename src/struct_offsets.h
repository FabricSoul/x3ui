#pragma once
#include <cstdint>

// Verified engine struct offsets (X3 32-bit engine, non-PIE @0x08048000).
// Single source of truth — do not hardcode these numbers elsewhere.
namespace x3 { namespace off {

// Global that holds the scene-manager pointer.
constexpr uint32_t SCENE_GLOBAL          = 0x0837a0d0u;

// scene-mgr fields (live-verified)
constexpr uint32_t SCENE_SECTOR        = 0x08; // -> current sector object (type 1)
constexpr uint32_t SCENE_PLAYER_ANCHOR = 0x38; // -> player ship object (type 7)

// object header
constexpr uint32_t OBJ_NEXT      = 0x00;
constexpr uint32_t OBJ_PREV      = 0x04;
constexpr uint32_t OBJ_ID        = 0x08;
constexpr uint32_t OBJ_NAME      = 0x0c; // char*
constexpr uint32_t OBJ_FLAGS     = 0x40; // flag word; bit0|bit4 (0x11) -> hidden/internal contact
constexpr uint32_t OBJ_FLAGS2    = 0x44; // flag word; bit29 (0x20000000) -> camera/debris/wreck/internal
constexpr uint32_t OBJ_TYPE      = 0x48; // int16 type/class id
constexpr uint32_t OBJ_SUBTYPE   = 0x4a; // int16 (adjacent; = high half of the dword at OBJ_TYPE)
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

// type ids at object +0x48 (live-verified against the running game)
constexpr int16_t TYPE_SECTOR   = 1;
constexpr int16_t TYPE_SUN      = 3;
constexpr int16_t TYPE_PLANET   = 4;
constexpr int16_t TYPE_STATION  = 5;     // docks / trading stations
constexpr int16_t TYPE_FACTORY  = 6;     // production complexes (also a "station")
constexpr int16_t TYPE_SHIP     = 7;
constexpr int16_t TYPE_WARE     = 9;     // installed equipment/ware (e.g. shields)
constexpr int16_t TYPE_ASTEROID = 0x11;
constexpr int16_t TYPE_GATE     = 0x12;

// Position units: 1 km = 500000 raw units (object transform x/y/z).
constexpr float   UNITS_PER_KM  = 500000.0f;

// HUD / gravidar — for the scanner-range visibility test (ships beyond range hide).
constexpr uint32_t HUD_MGR_GLOBAL  = 0x0837a100u; // *: HUD mgr; *(it) = gravidar hashmap
constexpr uint32_t HASH_BUCKETS    = 0x00;        // hashtable: bucket-array ptr
constexpr uint32_t HASH_MASK       = 0x04;        // hashtable: capacity (power of 2)
constexpr uint32_t HASH_NODE_NEXT  = 0x00;
constexpr uint32_t HASH_NODE_VALUE = 0x08;
constexpr uint32_t GRAVIDAR_SOURCE = 0x10;        // scanner source == player ship
constexpr uint32_t GRAVIDAR_RANGE  = 0x240;       // cached scanner range (int, 500000/km)
constexpr uint32_t FLAG_KNOWN      = 0x8;         // +0x40 bit3: known/scanned (skips range)

}} // namespace x3::off
