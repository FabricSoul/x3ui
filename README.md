# x3ui — an injected, drivable UI for *X3: Albion Prelude*

An `LD_PRELOAD`'d [Dear ImGui](https://github.com/ocornut/imgui) overlay for **X3: Albion
Prelude / Terran Conflict** on Linux. Not a read-only HUD: it reads the live sector out of
the running engine's memory, renders it as an EVE-style object table plus a Homeworld-style
3D holographic map with real ship hulls pulled from the engine's own model data — and then
**drives the game back**, setting your target and issuing ship commands from the overlay.

The game binary is never modified. Nothing is patched on disk. The overlay interposes two
SDL2 symbols the engine already calls and does everything else by reading and writing the
process's own memory.

> This was built by reverse-engineering a 32-bit, non-PIE, stripped-ish ELF (`X3AP_main`,
> image base `0x08048000`) with no source, no symbols beyond the dynamic table, and no
> modding API for any of this. The `docs/` tree is the map of what was found.

---

## What it does

| | |
|---|---|
| **Sector readout** | Walks the engine's live sector object list and shows every object — ships, stations, gates, asteroids, wares — with id, name, type, faction, distance. Sortable/hideable columns, sticky header, EVE-overview-style filters (all / ships / stations / gates / roids / other), independent filter sets for the table and the 3D map. |
| **Correct visibility** | Matches what the *game* lets you see: objects are gated by the player's gravidar scanner range and the engine's internal/despawn flags, so the overlay never leaks information the in-game sector map wouldn't give you. |
| **Names, properly resolved** | Object names come from the engine's text database, including unresolved `{page,id}` references expanded through the engine's own `get_text_simple`. |
| **3D holographic map** | Orbit / pan / focus-follow camera, faction-colored objects, height cues and ground ticks, a sensor sphere at scanner range, hover tooltips, double-click to focus. |
| **Real ship hulls** | The 3D map draws actual engine geometry — not icons. Hulls are read out of the engine's resident CPU model data, un-rotated into body frame, normal-generated, cached, and drawn through our own GL program with an additive fresnel "hologram" look. |
| **Target bridge** | Click an object in the table or the 3D map and the *game's* HUD target changes. The game's live target is read back and highlighted in both views, so the overlay and the cockpit never disagree. |
| **Command bridge** | Right-click → "Move to / Dock at / Attack / Follow / Stop", issued by writing the ship's AI command struct, which the engine's per-frame executor then runs. |
| **Live debug socket** | A read-only introspection server on `127.0.0.1:7373` (`read`/`reads`/`bytes`/`cstr`/`children`/`scene`/`sector`/`owire`) serviced between frames. This is the tool that made the whole project tractable — see below. |

Aesthetically it's built as a **game mod, not a dev tool**: translucent holographic panels,
lean and cinematic, meant to sit over the cockpit.

---

## How it was done

The interesting part of this project isn't the UI, it's the method. Five techniques carried
all of it.

### 1. Non-invasive injection (Route B)

Rather than patching the executable, `libx3ui.so` is `LD_PRELOAD`ed into the engine and
interposes two symbols confirmed present in `nm -D X3AP_main`:

- **`SDL_GL_SwapWindow`** — called once per frame right before the buffer flip. We draw an
  ImGui frame on top, then forward to the real swap via `dlsym(RTLD_NEXT, ...)`.
- **`SDL_PeepEvents`** — the engine's event pump. Events are fed to ImGui and, when the
  overlay has focus, swallowed so they never reach the game. `ScrollLock` toggles the overlay.

Getting this to load at all inside Steam's `pressure-vessel` container was its own
sub-project: a 32-bit nix shell so `$CC` targets i686 natively, `-static-libstdc++` so we
don't depend on the container's older ABI, `patchelf --remove-rpath` so the loader resolves
libraries inside the container instead of dead `/nix/store` paths, weak stubs for two SDL2
symbols the shipped SDL is too old to provide, a process-name gate so the `.so` goes dormant
in every other process of Steam's launch chain, and a custom launcher pair
(`x3run.sh` → `x3engine.sh`) that replaces the game's flaky GL probe and starts the engine
directly with `-skipintro -faststart <slot>`.

### 2. Reads that can't crash the game

Every engine read goes through `pread` on `/proc/self/mem`, and every write through `pwrite`
on a second `O_RDWR` fd. We're in-process, so this is our own memory — but routing through
the kernel means a stale or bogus pointer returns `-1` instead of raising SIGSEGV. A wrong
offset produces a blank panel, never a crash. All walks are additionally bounded by explicit
caps (node count, vertex count, edge count) so a corrupt or cyclic structure can't hang a frame.

### 3. The live debug socket — the thing that made RE fast

A read-only text server on `127.0.0.1:7373`, with commands serviced **on the main thread,
between frames** (so a walk never races the engine mutating the structure it's walking).
That one detail turned reverse-engineering from "edit, rebuild, relaunch, click into
flight, guess" into an interactive session: with the game sitting in flight you can read
arbitrary addresses, dump structs, walk child lists, chain up parent pointers, and get a
whole-object wireframe's statistics — with **no rebuild**.

The engine-memory abstraction is a `struct Mem` of `std::function` read/write primitives.
That one indirection is why the same walk code runs against the live game *and* against a
`FakeMem` in host unit tests.

### 4. Decompile → live-verify → document (and never guess)

The working loop for every engine fact:

1. **Decompile** the relevant function in Ghidra (driven over MCP, so function and data
   renames land back in the project and the knowledge base compounds instead of evaporating).
2. **Live-verify** the claim through the debug socket against the running game.
3. **Document** it with the evidence tagged **(D)** decompiled or **(L)** live-verified.
4. Only then build on it.

[`docs/x3-render-pipeline.md`](docs/x3-render-pipeline.md) is the clearest artifact of this:
a complete, claim-by-claim-attributed account of how the engine actually draws an object —
the two object representations and which one the renderer walks, the drawable-enumeration
gates, the static per-part "is-real-mesh" gate vs. the per-frame visibility flag, the LOD
selector, and the fixed-point transform convention (vertices ÷16384, orientation and scale
16.16, orientation stored column-major so our row-major convention needs a transpose).

The corollary learned the hard way, and now a standing rule: **verify the data before
assuming a logic bug.** A render that looks wrong is very often perception, not math. Half
the "bugs" in the commit history were correct code fed a misread field.

### 5. Tests for the parts that don't need a game

`src/sector_reader.cpp` — the sector walk, the scene-graph traversal, transform composition,
LOD selection, normal generation, text-reference expansion — is pure logic over fixed-width
types with no SDL, no GL, no ImGui, and no architecture dependence. So it's covered by host
unit tests that run in any shell (`make test`, currently 129 checks): happy paths plus the
nasty ones — null names, null transforms, reference cycles, cap exhaustion, edge rebasing
across unioned parts, bulk-read equivalence. The graphics and the injection get verified
live; everything else gets verified in milliseconds.

### 6. A knowledge base built to survive the session

This is a long, multi-session reverse-engineering effort, and the binding constraint is
**remembering what was already learned**. So the repo carries its own navigation layer:
[`CLAUDE.md`](CLAUDE.md) as the front door (build/run/test, conventions, what to read when),
[`docs/README.md`](docs/README.md) as the catalog, a dated **spec + plan** pair per feature
under `docs/superpowers/`, and the authoritative engine reference in
`docs/x3-render-pipeline.md`. New verified facts are appended as they're found rather than
at the end — the hard-won offsets are the asset, and an un-recorded finding is a finding
you will pay for twice.

---

## Status

| Feature | State |
|---|---|
| Sector-map readout (table, filters, visibility, names, distance) | **done** |
| Object wireframe from engine model data | **done** |
| Holographic solid hulls in the 3D map (GL program, caching, LOD) | **done** |
| Target bridge (overlay selection ⇄ game HUD target, both directions) | **done** |
| Read-only debug socket | **done** |
| Command bridge (right-click → Move to / Dock at / Attack / Follow / Stop) | **built, partially live-verified** |
| Player auto-dock | **partial.** A per-frame injected script-access op drives the player's dock *approach*; final entry/clearance is still open. The RE write-up is the deliverable here, not a finished feature. |

The deep dive behind that last row — the MSCI script VM's bytecode interpreter, its task
scheduler, the C↔VM coupling, and why the player's ship is the one ship the stock dock
orchestrator won't accept — is the largest body of reverse-engineering in the project, and
the one place where documentation outran working code.

---

## Build and run

Requires Linux, a Steam copy of X3: Albion Prelude (or Terran Conflict), and `nix-shell`.

```bash
git clone --recurse-submodules https://github.com/FabricSoul/x3ui
cd x3ui

nix-shell --run 'make'     # 32-bit build -> libx3ui.so (MUST be inside the nix shell)
make test                  # host unit tests for the pure logic (any shell)
```

Then point Steam's launch options for the game at the outer wrapper:

```
/path/to/x3ui/x3run.sh %command%
```

and launch:

```bash
./x3launch.sh          # auto-load the newest save, straight into flight
./x3launch.sh 13       # auto-load save slot 13
./x3launch.sh menu     # overlay on, boot to the main menu
```

`ScrollLock` toggles the overlay. Engine stdout/stderr lands in `x3engine.out`.

**Paths are currently hardcoded** to the author's machine in `x3run.sh`, `x3engine.sh`, and
`x3launch.sh` (`/home/fabric/x3ui`, the Steam library path, the `EgoSoft/X3AP` save dir).
Edit those three scripts before first run.

### Layout

```
src/overlay.cpp        SDL/GL interposition, ImGui panels, 3D map, GL hull renderer
src/sector_reader.*    pure engine-memory walks: sector, scene graph, meshes, text  (unit-tested)
src/debug_server.*     the localhost introspection server + per-frame pump          (unit-tested)
src/struct_offsets.h   every verified engine offset and constant, in one place
tests/                 host unit tests over a FakeMem address space
docs/                  the knowledge base — engine pipeline, per-feature specs + plans
tools/x3cat.py         .cat/.dat archive reader (cat = rolling XOR 0xDB, dat = XOR 0x33, pck = gzip)
tools/x3dbg            one-shot client for the debug socket
addon/scripts/         hand-built MSCI .pck scripts used in the dock-bridge experiments
x3run.sh/x3engine.sh   Steam launch-chain wrappers (host side / inside pressure-vessel)
x3launch.sh            dev-loop relauncher with save auto-load
```

### Optional tooling

- **Ghidra + [GhidraMCP](https://github.com/pinksawtooth/GhidraMCP)** (`ext/GhidraMCP`
  submodule) for the decompilation half of the loop.
- **X-Studio**, the community MSCI script editor, is *not* vendored here — download it from
  the Egosoft forums if you want to work on the script side.

---

## Notes

This is independent reverse-engineering of a game the author owns, for interoperability and
modding — the long tradition of the X-series modding scene. No game code or game assets are
redistributed in this repository: `docs/` contains original analysis, and `addon/scripts/`
contains scripts written for this project. Not affiliated with or endorsed by Egosoft.

The overlay writes to the running game's memory. Use a save you don't mind losing.

MIT licensed — see [LICENSE](LICENSE). Dear ImGui (`third_party/imgui`) is MIT, © Omar Cornut.
