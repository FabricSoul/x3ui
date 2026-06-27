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

// Gate payload: 7-slot destination-object pointer array (each dest's parent chain
// leads to the destination sector). gate_payload = *(gate+0x50).
constexpr uint32_t GATE_DEST_ARRAY = 0x58;

// Owner / race / IFF (matches the game's sector-map blip coloring).
constexpr uint32_t OBJ_RACE      = 0x3c; // int16 owner/race id 0..31 (read via object_get_relation)
constexpr int16_t  SPACEFLY_RACE = 13;   // race 13 = spaceflies (wildlife; filtered out)
constexpr uint32_t NO_IFF_FLAG   = 0x40000000u; // (+0x44 byte3 bit6): object has no IFF relation
// payload (ship/station/factory) relation bitmasks, indexed by (1 << viewer_race):
constexpr uint32_t REL_FRIEND_MASK = 0x48; // *(payload+0x48): friend if bit set
constexpr uint32_t REL_FOE_MASK    = 0x4c; // *(payload+0x4c): foe if bit set
constexpr uint32_t PAYLOAD_OWNER   = 0x0c; // *(payload+0x0c): owner id (== player's => own asset)

// Transform node (*(obj+0x6c)) world orientation 3x3, 16.16 fixed-point (0x10000=1.0).
// Forward/look vector (engine nose = local +Z) = column 2 of the matrix at node+0x3c.
constexpr uint32_t TF_FWD_X = 0x44;
constexpr uint32_t TF_FWD_Y = 0x54;
constexpr uint32_t TF_FWD_Z = 0x64;
constexpr float    FIXED16  = 65536.0f;

// Text database (resolves {textPage,textID} refs in object names to strings).
constexpr uint32_t TEXTDB_ROOT     = 0x08308880u; // *: g_text_db_root host
constexpr uint32_t RENDER_CONFIG   = 0x0829d798u; // *: g_render_config
constexpr uint32_t CFG_LANG        = 0x708;       // render_config + 0x708 = language index
constexpr uint32_t TEXTDB_PAGETBL  = 0x334;       // host + 0x334 + lang*4 = page hashtable

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
