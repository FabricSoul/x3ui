# X3 AP MSCI `.pck` codearray format — hand-authoring reference

How to **read and hand-write** X3: Albion Prelude compiled MSCI scripts (`.pck`) without the
in-game Script Editor. Everything here was **decoded statically** from the game's own shipped
scripts (tag **(S)** = static-decoded, cross-verified across ≥2 stock scripts; **(L)** = live
engine-verified). Companion to memory [[x3-msci-scripting]] (official command syntax) and
[[x3-pck-scripts-gzip]] (the gzip container). First used to build `plugin.x3ui.dockwatch` (the
dock bridge) — see the recipe at the end.

> **Provenance:** decoders read `al.plugin.bw.detector.event`, `!ship.cmd.movestation.std`,
> `!wing.cmd.jump.std`, `!move.movetostation`, `!move.stay`, `!config.add.wares.one.one`, and
> the user's own unsigned `x3uimoney`/`x3ui.dockat`. Runtime execution of a *hand-built* file is
> still **(L)-pending** at time of writing.

## Container
`.pck` = **gzip of UTF-8 XML** (see [[x3-pck-scripts-gzip]]). The XML `<script>` has:
`<name>`, `<version>`, `<engineversion>`, `<description>`, `<arguments>`, `<sourceplaintext>`
(CDATA, human text), `<sourcetext>` (SE markup — editor display only), **`<codearray>`** (what
the engine actually runs), and optionally `<signature>`.

- **The engine runs `<codearray>`.** `<sourceplaintext>`/`<sourcetext>` are for the editor;
  they do not affect execution. A wrong codearray silently no-ops even if the source looks right.
- **`<signature>`: OMIT it entirely** for hand-authored/unsigned scripts (S). Of 875 addon
  scripts, the only unsigned ones are the user's own X-Studio customs, and they have **no
  `<signature>` element at all** (not an empty tag). The signature is Egosoft's authenticity
  stamp, not a runtime gate — the engine executes the codearray regardless. Unsigned scripts run
  only once the **Script Editor is unlocked** (pilot renamed `Thereshallbewings`).
- **Match the stock gzip header:** produce `1f 8b 08 00 …` (no FNAME, `mtime=0`). Python:
  `gzip.GzipFile(fileobj=fh, mode="wb", filename="", mtime=0)` — `gzip.open` would set the FNAME
  flag (`…08 08…`) and embed a filename. zlib decodes FNAME fine, but match stock to be safe.
- **Filename == `<name>` == codearray slot[0].** The loader keys on the loose filename; keep all
  three identical (see [[x3-msci-scripting]] reason #1). Drop the file in `addon/scripts/`
  (X3AP reads addon, not the base `scripts/`).

## `<codearray>` top: a `type="array" size="10"` wrapper
| slot | type | meaning |
|---|---|---|
| 0 | string | script name (== `<name>` == filename) |
| 1 | int | engineversion — **40** for stock TC, 50 for X-Studio |
| 2 | string | description (may be empty) |
| 3 | int | script version (`<version>`) |
| 4 | int | reserved, **always 0** |
| 5 | array | **variable-NAME table** — arguments first (in `<arguments>` order) then locals (first-appearance order). A code var-ref `(131074,idx)` or a bare ret-var index both index THIS array. **Empty ⇒ bare `int 0`, not an empty array.** |
| 6 | array | **CODE** — one sub-array per statement, in source order |
| 7 | array | per-**declared-argument** type+desc: entries `[typeId, descString]` (typeId 41=Var/Station-Carrier, 21=Var/Ship). Empty ⇒ `int 0` |
| 8 | array | SE **source-reconstruction** table (comments/blank/`end` lines): entries `[attachIdx, kind(, text)]`, kind 1=comment 2=blank 4=end. **Editor-only; does NOT affect execution.** Empty ⇒ `int 0` |
| 9 | int | **command-association id** — 7 (=COMMAND_DOCKAT) for the dock command scripts; **0** for non-command scripts |

## CODE statement format
Each statement is `<sval type="array" size="N">` whose **first int is the source-operator
opcode**, followed by operands. Operands are mostly **value-pairs** `(tag, payload)`:

| value kind | encoding |
|---|---|
| int literal | `(4, <int>)` |
| null | `(0, 0)` |
| string literal | a single `<sval type="string">` — **self-describing, NO leading tag int** |
| variable value `$x` | `(131074, varIdx)`  (0x20002; varIdx → slot[5]) |
| object-ref / game constant | `(131075, constIdx)` (0x20003) |
| enum constant (e.g. COMMAND_DOCKAT) | `(groupTag, enumVal)` e.g. `(18, 7)` |

**Not** value-pairs (bare ints): the leading opcode; an **argc** count in a call; an
**assignment/ret-var TARGET** (a bare varIdx, e.g. get-global's dest, `get player money` = `[626,0]`);
the `-2147483647`/`-2147483646` mode markers; and builtin **operator-ref ids** (large negatives).

### Object-ref / game-constant table (tag 131075) (S)
`1 = [THIS]`, `3 = [PLAYERSHIP]`, `9 = [FALSE]`, `10 = [TRUE]`. (More exist; these are verified.)

### Opcode table (verified subset) (S)
| op | meaning | shape |
|---|---|---|
| 100 | `goto label` | `[100, absIdx]` — operand = **absolute 0-based statement index** into slot[6] |
| 101 | label definition | `[101, "name"]` — occupies the jump-target index; referenced **by index, not string** |
| 102 | **call script** (START *and* inline) | `[102, "name", MODE, refobjPair, argc, argPairs…]` — see below |
| 103 | return | `[103, valuePair]` — `[103,0,0]`=null, `[103,131074,idx]`=`return $v` |
| 104 | expression / assignment / `if` cond | 2nd int negative = operator-ref id (if/compare/engine op); small positive = plain-assign DEST var |
| 105 | `@ = wait N ms` | `[105, -2147483647, 4, N]` (the `-2147483647` is the `@`/discard marker; `(4,N)`=ms) |
| 112 | if-branch-end **unconditional jump** | `[112, endIdx]` — compiler-generated at the tail of each true-branch; holds the `end` index |
| 129 | array element read `$a[i]` | — |
| 157 | set global variable | `[157, 5,"name", valuePair]` |
| 158 | get global variable | `[158, 5,"name", destVarIdx]` — dest is a **bare** var index |
| 1167 / 1168 | gosub / endsub | `[1167, absIdx]` / `[1168]` |
| 538 / 519 | set command / set destination | actor `(131075,1)`=[THIS] then value-pairs; DOCKAT = `[538, 131075,1, 18,7, target, 0,0,0,0,0,0,0]` (sets the command WORD only — the executor has nothing to follow) |
| **514** | **START command** — *schedules* a ship command (runs the mapped command-script on task-0) | `[514, shipTag,shipIdx, 18,CMD_ENUM, arg1pair,arg2pair,arg3pair,arg4pair]` (size 13). **This is how the game issues a dock/attack/etc.**, NOT op-102. Legal on `[THIS]` (native scripts do `START [THIS] -> command`). Verified: `COMMAND_PROTECT`=4, `COMMAND_JOINWITHFLEET`=84, **`COMMAND_DOCKAT`=7 with arg1=station**. Command enum group tag = **18**. Dock the player = `[514, 131075,3([PLAYERSHIP]), 18,7, 131074,$stationIdx, 0,0,0,0,0,0]`. `!init.ship.globalscriptmap.std` maps `COMMAND_DOCKAT → !ship.cmd.movestation.std`. |

### `if`/`end` model (S) — there is **no explicit conditional jump**
`if COND` compiles to ONE `[104, opRefId, argc, …cond operands…]`. The body statements follow
**immediately**. Each true-branch is terminated by a compiler-generated `[112, endIdx]` whose
operand is the absolute index just **past** the whole `if/else if/else` chain (where source `end`
sits). On a FALSE condition the VM resumes structurally at that same end index. `else if` = another
`[104,…]` at the fall-through; `else` = statements at fall-through; `end` = no own statement (it is
the index the `112` targets). Plain-variable truthiness (`if $x`) uses opRefId **-1610582525**
with shape `[104, -1610582525, 1, 131074, idx, 1, -1]` (this id appears in 6+ stock scripts).

### call-script: START vs inline is ONLY the mode word (op 102) (S)
`[102, "<name>", MODE, refTag, refIdx, argc, <argc value-pairs>]`
- **MODE `>= 0`** = inline call, and this int IS the dest var index (`$v = … call script`).
- **MODE `0x80000001` (-2147483647)** = inline, discard return (`= [THIS] -> call script`).
- **MODE `0x80000002` (-2147483646)** = **START** (spawn concurrent task on RefObj, discard).
- **RefObj** `(refTag,refIdx)` is encoded identically for both: `[THIS]`=`(131075,1)`,
  `[PLAYERSHIP]`=`(131075,3)`, a `$ship` var = `(131074,varIdx)`.
- Per the official spec ([[x3-msci-scripting]]): START binds `[THIS]`=RefObj inside the callee and
  **terminates all task-0 scripts on that RefObj**; **task-0 scripts cannot START on `[THIS]`** —
  so a watcher that STARTs on `[PLAYERSHIP]` must itself not be task-0 on the player ship.

## Worked recipe: the dock-bridge watcher `plugin.x3ui.dockwatch`
Poll a global var and START the player's dock. slot[5]=`["t"]`, slot[7/8/9]=`0`, no signature.
Source:
```
loop:
@ = wait 500 ms
$t = get global variable: name='X3UI.DOCKAT'
if $t
  set global variable: name='X3UI.DOCKAT' value=null
@ START [PLAYERSHIP] -> call script '!ship.cmd.movestation.std' : object=$t
end
goto label loop
```
CODE (slot[6], 9 statements): `[101,"loop"]` · `[105,-2147483647,4,500]` ·
`[158,5,"X3UI.DOCKAT",0]` · `[104,-1610582525,1,131074,0,1,-1]` · `[157,5,"X3UI.DOCKAT",0,0]` ·
**`[102,"!ship.cmd.movestation.std",-2147483646,131075,3,1,131074,0]`** (START on [PLAYERSHIP],
arg object=$t) · `[112,7]` (end→idx7) · `[100,0]` (goto→loop) · `[103,0,0]`.
The dock call itself is `!ship.cmd.movestation.std` (arg `object`, acts on `[THIS]`) → sets
COMMAND_DOCKAT + calls `!move.movetostation` (the undock→fly→dock FSM). See [[x3-ship-set-command]].

## Open runtime question (L-pending)
No stock script ever engages **autopilot** — NPCs just `fly to station` and the engine moves them.
For the **player** ship (player-control bit `obj+0x40&0x200` set), a script-issued dock may set the
command but **not move the ship** unless autopilot (`obj+0x40&0x80000`, SA op 0x120
`SA_SetAutopilot`@0x08118b25) is engaged separately (the in-game menu does this via the INPUT class,
not the command script). Two-test plan: (1) watcher alone; (2) watcher + overlay engages autopilot.
See [[x3-ship-set-command]], [[x3-player-dock-drive]].

## 2026-07-02 — WHY hand-authored loose .pck LIST but never RUN (Agent: engine ingest path)

Static RE of X3AP_main ingest/load/run path (Ghidra). ROOT CAUSE of the reproducible wall:

- The C engine executes ONLY the compiled byte-op CODE blob (root+8, len root+0x10; opcodes
  0..0xb1). Scripts are resolved as CLAS records at root+0x20 (count root+0x1c) via
  `script_lookup_by_id_or_namehash`@0x0809dfc0 — which NEVER lazily loads from disk on a miss.
- The ONLY C loader that builds executable script records is
  `script_archive_load_one_class_chunk`@0x0809b550. It parses the BINARY KC chunk format
  (STOR/CODE/CLAS/SYMB/VARS/STRG/STRU/PTCH, big-endian) from `<name>.obj` (x3story.obj) — the
  precompiled master archive. `script_codearray_relocate_byteops`@0x0809cb60 only byteswaps the
  already-compiled CODE stream (it is NOT a compiler).
- Loose addon/scripts/*.pck hold SOURCE codearray (operators 100–999, e.g. 157/538/706), NOT
  byte-ops. NO C function lowers/compiles a loose codearray into CODE + installs a CLAS record.
  `msci_sval_xml_to_cell`@0x080a8560 (SA op 0x68 in msci_string_command_interp) parses the <sval>
  tree into a generic array CELL only — it is data, never a program. XML tag literals
  `<script>/<codearray>/<engineversion>/<sourcetext>` are NOT in the binary (search_bytes = 0);
  only `<sval>` is. So the C engine never tag-parses a .pck as a script.
- The SE "list" = `vfs_glob_enumerate_dir`@0x08151810 (glob addon/scripts/*.pck) + gz + SA-op
  0x68 reading only <name>. That is why ANY well-formed .pck LISTS. Listing ≠ installed/runnable.
- SIGNATURE is a red herring for the C path: per-.pck `<signature>` is NEVER read by C. The only
  signature verifier `x3story_verify_signature_xegosoft`@0x08158a?? reads addon/x3files.xml (RSA/MD5
  vs XEGOSOFT), and only toggles root+0x12f0 bit 0x10 = achievements/logbook disable. No per-script
  signature/verify gate on load or run.

=> Statement 0 never runs because the hand-authored .pck has NO compiled CODE + NO CLAS record;
   'r' (run_script_async→script_resolve_and_alloc_task→script_lookup) resolves nothing. The
   codearray→byteop compile is done by the in-game Script Editor MSCI code (SE_*/__scriptOpAddString
   ops), not by C — dropping a prebuilt .pck in the folder never triggers it.

FIX: abandon hand-authored loose-.pck execution. Drive behavior by scheduling an ALREADY-COMPILED
stock script that lives in x3story.obj (e.g. `!move.movetostation`) from the overlay via
run_script_async(ctx,0,scriptKey=*(int*)(playerShip+0x90),"!move.movetostation",1,1,station) — the
route already documented in memory [[x3-msci-vm-architecture]] / [[x3-ship-set-command]].
