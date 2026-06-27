#pragma once
#include <string>
#include "sector_reader.h"

namespace x3 {

// Execute ONE text command against `mem` and return the response text (which may
// span multiple lines). Pure and side-effect-free — unit-testable with FakeMem.
// Commands (addresses accept 0x.. hex or decimal):
//   ping
//   read  <addr>                 -> ok 0x<hex> <signed-dec>
//   reads <addr> <count>         -> ok 0x.. 0x.. ...
//   bytes <addr> <n>             -> ok <hexdump>
//   cstr  <addr> [max=64]        -> ok "<string>"
//   type  <addr>                 -> ok type=<i16> sub=<i16>   (reads +0x48)
//   obj   <addr>                 -> ok addr=.. id=.. type=.. sub=.. name=".." pos=x,y,z
//   chain <addr> [limit=6]       -> one line/level walking parent (+0x54)
//   children <addr> [limit=16]   -> walk *(addr+0x50) child list; type+name per node
//   scene                        -> scene, *(scene+0x38) anchor, its type+name
//   sector                       -> read_current_sector(): header + one line per object
std::string handle_request(const Mem& mem, const std::string& line);

// Start the localhost debug server on a detached background thread (idempotent).
// Read-only: the handler only reads memory via `mem`.
void start_debug_server(const Mem& mem, int port = 7373);

// Call once per frame on the MAIN render thread. Services any pending socket
// command against live memory while the game is between frames (so the walk
// doesn't race the engine's mutation). Cheap no-op when nothing is pending.
void debug_server_pump();

} // namespace x3
