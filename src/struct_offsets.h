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

// Player current target (HUD targeting brackets) — fields on the gravidar/HUD object,
// plus the engine setter. See memory x3ui-player-target.
constexpr uint32_t GRAVIDAR_TARGET        = 0x1bc;        // *: current target OBJECT POINTER
constexpr uint32_t GRAVIDAR_TARGET_MODE   = 0x1c0;        // i16: 0 clear, 3 lock+brackets
constexpr uint32_t GRAVIDAR_SET_TARGET_FN = 0x08131d10u;  // void __cdecl(gravidar, i16 mode, void* targetObj)
constexpr int16_t  TARGET_MODE_LOCK       = 3;            // lock + brackets (+ NotifyTargetLock)
constexpr int16_t  TARGET_MODE_CLEAR      = 0;            // clear target

// Ship AI command — issue dock/move/attack from injected code. Engine has NO callable
// setter; write the command struct on the ship PAYLOAD (= *(ship+0x50)) and the AI tick
// auto-runs the bound built-in command script. Only gate is class==7, no owner check.
// See memory x3-ship-set-command.
// Autopilot command struct on the ship PAYLOAD (= *(ship+0x50)). The validated-DOCKAT engage
// block @0x0811cb14 (and the MOVE/FLYTO handler @0x08123250) write these; the bound
// !move.* MSCI script then navigates. CRITICAL: +0xAC/B0/B4 is the navigation DESTINATION
// (fly-to: target world pos; dock: the computed approach point) — leaving it 0 = no goal =
// raw thrust. See memory x3-ship-set-command.
constexpr uint32_t SHIP_CMD_ID        = 0xC4;  // int16: command id (on payload)
constexpr uint32_t SHIP_CMD_TARGET    = 0x98;  // ptr:   command target object (on payload)
constexpr uint32_t SHIP_CMD_DEST      = 0xAC;  // int32 x3: navigation DESTINATION x,y,z (on payload)
constexpr uint32_t SHIP_CMD_PAR1      = 0xAC;  // alias (legacy name)
constexpr uint32_t SHIP_CMD_BAY       = 0xBC;  // int32: dock-bay index (dock only) (on payload)
constexpr uint32_t SHIP_CMD_AUXTGT    = 0xBC;  // alias
constexpr uint32_t SHIP_CMD_FAMILY    = 0xC0;  // int32: command-family index (validated-dock = 9) (on payload)
constexpr uint32_t SHIP_CMD_STARTTIME = 0x84;  // u32: cmd start time (on payload)
constexpr uint32_t SHIP_NOTIFY_OBJ    = 0x78;  // on SHIP obj: command-active handle (set 1 to arm)
constexpr uint32_t SHIP_NOTIFY_VAL    = 0x7C;  // on SHIP obj: engine-computed handle (leave 0)
constexpr uint32_t RENDER_CFG_TIME    = 0x6b8; // *(*RENDER_CONFIG + this) = current sim time
constexpr int32_t  CMD_FAMILY_DOCK    = 9;     // +0xC0 value for validated DOCKAT
// dock_compute_approach_for_bay(int station, int bayIdx, int* outWorldPos3, int ship, 0,0,0) cdecl —
// fills outWorldPos with the world-space dock approach standoff for that bay (the engine's own math).
constexpr uint32_t DOCK_APPROACH_FN   = 0x08104ff0u;
constexpr int16_t  CMD_NONE     = 0x00;        // idle / abort
constexpr int16_t  CMD_FLYTO    = 0x05;        // autopilot fly-to position/object (dest = target pos)
constexpr int16_t  CMD_DOCKAT   = 0x04;        // dock at target (station/dockable)
constexpr int16_t  CMD_ATTACK   = 0x14;        // attack target ship
constexpr int16_t  CMD_FOLLOW   = 0x15;        // follow target ship
constexpr int16_t  CMD_MOVE_OBJ = 0x18;        // fly to target object
constexpr int16_t  CMD_MOVE_POS = 0x19;        // move to POSITION (dest = world x,y,z @+0xAC; no obj arrival)
// Autopilot engage/disengage — DECOMPILED (don't guess this again):
//  * obj+0x40 bit19 (0x80000): when SET, the arrival/auto-stop physics @0x0810cdc6 is SKIPPED
//    so the autopilot keeps driving toward the destination; when CLEAR, arrival logic reclaims
//    the ship and it stops. ENGAGE = set bit19 (MSCI engage @0x08118b49 ORs it in). DISENGAGE =
//    clear bit19 ONLY (player_input_flight_handler @0x0805ad8b does `flags &= ~0x80000`).
//  * bit18 (0x40000) is INERT for autopilot (permanent "player-controlled object" marker) — the
//    executor never reads it; do NOT set/clear it.
//  * The per-frame command executor @0x08105f00 runs the ship's command only while the gate
//    `(obj+0x40 & 0x800200)==0 && (obj+0x47 & 2)==0` holds (bit9=0x200 manual-direct, bit23=
//    0x800000 script-disable-AI). bit19 is NOT part of that gate.
constexpr uint32_t AUTOPILOT_ENGAGED = 0x80000u;   // obj+0x40 bit19 (arrival-skip / engage)
constexpr uint32_t EXEC_GATE_MASK    = 0x800200u;  // obj+0x40: any of these set => executor skips ship

}} // namespace x3::off
