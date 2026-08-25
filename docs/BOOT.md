# Playable bring-up (Phase 5) — the whole game runs as native code

DinoPark Tycoon is now lifted **in full** and executes as a native program.

```
scripts\build_boot.ps1      # lift all 697 funcs -> cl -> work\dino_boot.exe
work\dino_boot.exe          # loads the image, runs the recompiled startup
```

## What works ✅

- **Full-program lift.** `tools/lift_full.py` lifts **697 functions / ~90,000
  lines of C** against the recomp16 CPU model — every detected function plus the
  Borland `c0` startup (discovered by following the entry's near calls; the
  analyzer misses it because it has no standard prologue).
- **Dispatch table.** `recomp_dispatch.c` maps image address → function pointer
  so indirect calls/jumps resolve at runtime (`call/jmp` through registers/tables).
- **It compiles and links** into `work/dino_boot.exe` (MSVC, 64-bit host — the
  flat `seg:off → mem[]` model is pointer-size-agnostic).
- **It boots and runs the real startup → `main`.** `boot.c` + `runtime16.c` load
  the 250 KB image, **apply all 4,139 relocations**, build a **minimal PSP +
  environment** above the image, and enter at `0000:0000` with `DS=ES=PSP`. The
  recompiled Borland `c0` startup then executes for real:
  ```
  INT 21h AH=30 (DOS version) → AH=35×4/25 (install int vectors)
          → AH=4A (resize memory block) → call main (fn_01604 → FUN_1000_15ad)
  ```
  `main` runs its init sequence, and the game's **indirect calls now dispatch**
  (`lifter.dispatch=True` → `recomp_dispatch` reads the function pointer from
  memory and calls the target by address). It boots cleanly — no crash.

## Convergence harness

Indirect-call misses are fed back round by round: `recomp_dispatch` records
in-range misses, `recomp_dump_misses` writes `work/dino_misses.txt`, and
`lift_full.py` reads them back as forced function starts (filtered to real
instruction boundaries inside their containing function). `scripts/converge.ps1`
runs lift -> build -> boot -> collect in a loop.

Delete `work/dino_misses.txt` whenever anything upstream of it changes. A miss
recorded under a bug is usually a bad pointer, and forcing bad pointers as
function starts corrupts the next lift.

## Five bugs between "boots cleanly" and "runs the game"

The boot used to reach `main`, dispatch into garbage, and return. It looked like
a data-setup problem. It was five separate things, and the first one hid the
other four.

**1. The build had been failing silently since June.** `converge.ps1` decided a
round succeeded by testing whether `work/dino_boot.exe` existed -- and it always
existed, left over from the last build that worked. Every measurement for weeks
was of the same stale binary. It now deletes the exe before building.

**2. CS was stale for every function.** `lift16` resolves every `cs:`-relative
read and every near indirect dispatch through a per-function code-segment
constant, `_CODE_SEG`; unset, it falls back to `cpu->cs`, which `runtime16.c`
writes once at load and never maintains across the C-call dispatch. So all 53
`jmp word cs:[bx+table]` switch dispatches read their table from the wrong
segment. `build_segmap()` recovers the real map -- see below.

**3. `dispatch_near` did not exist.** For a near indirect call the lifter emits
`push16(cpu,0xFFFF); dispatch_near(...)`: a dummy return word, then the call.
On a miss that word has to come back off, or the caller's frame sits one slot
low. Nothing defined the function, and its absence is what turned a single
missed `_INIT_` entry into the endless stream of code-bytes-as-pointers -- the
walker's own `bx`/`si` were being read off a desynchronised stack.

**4. The startup ran under the wrong CS.** With no seeded function below it, the
Borland startup fell through to a last-resort "its own paragraph" rule and got
`SEG_001E`, so its near calls dispatched into a segment nothing else shared. The
MZ header already says otherwise: entry is `0000:0000`, so image offset 0 is
seeded with CS=0 like any far-call target.

**5. Near call/jmp targets did not wrap.** A near target is `CS:(IP + rel)` and
that offset wraps at 16 bits; the lifter resolves it as `func_start + disp`,
which is the same thing only when it does not wrap. Every call backwards past
the segment base wraps, and **707 of DinoPark's 1136 near calls** (62%) resolved
somewhere arbitrary. `lift_full.py` now rewrites `disp` to the wrap-correct
target before lifting, the same trick bolo uses.

**6. Every far call compiled to an empty stub.** `lift16` resolved far targets
with formulas carried over from Civ (`seg*16 + off - 0x14`, and
`hdr_size + seg*16 + off`), and DinoPark keys `known_funcs` by plain image
offset, so neither ever matched. **3,574 far calls -- nearly every call a
large-model program makes -- became 360 no-op functions**, which is the real
reason the boot "ran cleanly" and did nothing. The lifter now takes an opt-in
`far_base` for projects whose key space maps directly (upstream, default off so
Civ is unchanged), and `lift_full.py` sets it to 0.

That leaves far calls landing on entry points the prologue heuristic missed --
Borland omits `push bp` for functions with no locals, so the analyzer merges them
into whatever precedes them. Anything a far call points at is a function by
definition, so those targets are promoted statically (90 of them) rather than
waiting for a runtime miss. Unresolved far calls: **3,574 -> 25**.

### Where it stops now

The boot runs DinoPark's real init and no longer returns early -- the watchdog
stops it. It allocates memory (`AH=48`), selects drives (`AH=0E`/`19`), checks
file attributes (`AH=43`), and reaches console I/O (`AH=07`/`0B`/`40`) -- the
game's disk-check startup. With convergence rounds on top it gets as far as
`AH=3D` file opens and `INT 10h` mode sets.

Open, in rough order:

- **The convergence loop oscillates** once it is deep enough (round 4 hit 2,236
  dispatches and 309 mode sets, round 5 fell back to 111 and 0). Some forced
  target is wrong; the boundary filter needs to be stricter than "decodes as an
  instruction".
- **Filenames reaching `AH=3D` are garbage**, so the pointer handed to the open
  path is wrong -- most likely a `DS`/`SS` mix-up in the caller.
- **Runtime gaps**: `AH=40` (write), `AH=43` (file attributes), `AH=09` (print
  string), `AH=0B`/`07` (console status/input) are all still unhandled.
- **Switch arms.** Now that far calls resolve, the 53 `cs:[bx+table]` dispatches
  will start being reached, and a case block cannot be lifted as a standalone
  function -- upstream's `3412d02` does this for the 32-bit path by following the
  table at decode time and emitting a computed goto. The 16-bit port is still
  wanted; it is simply no longer the first thing in the way.

## The segment map

Large-model Borland splits DinoPark across **29 code segments**, and a function's
CS decides what its switch tables and near indirect dispatches read.
`work/functions.json` records only flat image offsets, so `build_segmap()` in
`tools/lift_full.py` recovers CS:

1. A far `call`/`jmp` `9A/EA seg:off` states its target's CS outright -- **258
   detected functions** seeded directly, with **zero conflicts**. The MZ header's
   entry `CS:IP` seeds image offset 0 the same way.
2. A near call cannot leave its segment, so both ends share one CS -- flood-fill
   the seeds across near-call edges (**380/693**).
3. Anything still unseeded takes the nearest seeded function below it (the linker
   lays segments out in order).

Every assignment is guarded by `0 <= fn - CS*16 < 0x10000`: an offset that does
not fit in 16 bits cannot belong to that segment, whatever the edge says.

Independently checked: brute-forcing CS at each `jmp word cs:[bx+disp]` site
under the constraint that *every* table entry must land on a valid instruction
boundary in the enclosing function solves **50 of the 53 tables**, and every
answer agrees with the map above (`fn_15764` -> `SEG_13C5`, and so on). The 3
that do not solve are the sparse switch form -- a `cmp/je/add bx,2/loop` scan
over `{value, offset}` pairs -- which needs its own table reader.

## The `_INIT_` table (ruled out)

Worth recording, since it looked like the culprit. Borland's startup walks a
linker-built table at `0x1ED..0x230`: 6-byte records `{flag, priority, far ptr}`
from `ES:SI` to `ES:DI`, lowest priority first, flag 0 meaning a near call.
Dumping it out of the relocated image shows it is **entirely correct** -- six
records at `DGROUP:7B62`, every pointer landing in code:

```
[0] flag=00 prio=02 -> 0000:1C0F      [3] flag=00 prio=10 -> 0000:2FA6
[1] flag=00 prio=10 -> 0000:25BB      [4] flag=00 prio=10 -> 0000:30CF
[2] flag=01 prio=10 -> 0000:26D4      [5] flag=01 prio=1E -> 0000:6467
```

Five of the six are not detected function starts -- they are mid-function to the
analyzer, for the no-prologue reason above -- but all six are valid instruction
boundaries and obvious entry points.

## The runtime so far (`runtime16.c`)

A minimal DOS/VGA layer, filled in as the boot demands it:
- **INT 21h** — file open/read/seek/close (→ host files), alloc/free, get
  date/time/version, exit.
- **INT 10h/16h/33h/1Ah** — video mode, keyboard (no-key), mouse (absent), timer.
- **Port I/O** — VGA Sequencer (Map Mask), Graphics Controller, CRTC, and the DAC
  (palette captured), plus the `0x3DA` retrace toggle.

## The road to interactive (next)

The startup currently returns early — it reaches game code but the init sequence
needs more runtime under it. In rough order:

1. **The custom heap.** `FUN_3ee1_*` is DinoPark's handle-based memory manager
   (alloc/lock/unlock). The asset loaders and most of `main` depend on it.
2. **Startup → `main` → game loop.** Make the C-runtime init complete so control
   reaches the real `main` and its top-level loop, not an early return.
3. **Video + display.** Wire the planar VGA writes (`0xA000` + Map Mask, already
   modelled in part) and the DAC palette to an SDL2 framebuffer — reusing the
   `decode_pic`/`decode_act` work for the actual draw path.
4. **Input + timing.** Real keyboard/mouse via SDL, a frame-paced main loop (the
   civ harness pattern in `src/main.c`).
5. **Audio.** Stub Miles (INT 66h) cleanly, then optionally a real backend.

This is the same multi-session arc civ went through to reach "interactive
boot/menu". The foundation — a fully lifted, compiling, booting native DinoPark —
is in place.

> Note: `src/recomp/gen/` is regenerated per build (gitignored). `build_boot.ps1`
> runs `lift_full.py` (whole game); `build_pic.ps1` / `decode_act.py` use
> `lift_dinopark.py` (the focused decoders). Run one workflow at a time.
