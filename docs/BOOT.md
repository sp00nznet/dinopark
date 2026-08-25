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

## Convergence harness + the current frontier

The indirect-call misses are addressed by an **iterative convergence loop**:
`recomp_dispatch` records in-range misses → `recomp_dump_misses` writes them to
`work/dino_misses.txt` → `lift_full.py` reads them back as forced function starts
(mid-function jump-table / init-routine entry points) → re-lift → repeat.

Round 1 found **5 in-range misses** — all mid-function targets (e.g. `0x1C0F` =
`+0x23` into `fn_01BEC`). Registering them lets the boot run **much** deeper:
~**264 indirect dispatches** through the game's init.

**The frontier now is data-setup correctness.** Past the first init routines, the
dispatch starts reading *garbage* function pointers — e.g. `06EC:8B55`, whose
bytes `55 8B EC` are `push bp; mov bp,sp`, i.e. a pointer landing in **code**. So
some structure is read with the wrong segment/offset or before it's initialized,
and the game enters a **busy-wait** on bad state. Safeguards keep the boot
bounded:
- a **dispatch budget** (`DINO_BUDGET`, default 200k) for dispatch-driven spins;
- a **watchdog thread** (`DINO_WATCHDOG`, default 8s) that force-exits any
  non-dispatch busy-wait.

### Diagnosis, round 2 — the segment map (fixed) and the `_INIT_` walk (open)

Traced it. Three suspects: two ruled out, one **fixed**, one still open.

- ✅ **DS is correct.** Every dispatch runs with `ds=3020` (DGROUP) — not a
  segment bug.
- ✅ **Initialised data loads correctly.** The DGROUP hook pointers read right
  from the image (`DAT_4020_7624 = 0000:15AC`, init count = 0).
- ✅ **FIXED — CS was stale for every function.** `lift16` keeps a per-function
  code-segment constant (`_CODE_SEG`) that every `cs:`-relative read and every
  near indirect dispatch resolves through; when it is unset the lifter falls back
  to `cpu->cs`, which `runtime16.c` sets once at load and never maintains across
  the C-call dispatch. `lift_full.py` never set it, so **all 53 `jmp word
  cs:[bx+table]` switch tables (and every `push cs`) read from the wrong
  segment** — which is where the "code bytes read as function pointers" came
  from. `build_segmap()` now recovers each function's CS and the lifter emits
  `SEG_xxxx` constants; see *The segment map* below. Effect: the cumulative
  dispatch-miss set drops from **88 to 7**, i.e. 81 of the 88 were artefacts of
  the wrong segment. (Those 88 were also being fed back as forced function
  starts, which corrupted the lift — delete `work/dino_misses.txt` when
  changing anything upstream of it.)
- ❌ **Open — the Borland `_INIT_` table walk runs on garbage bounds.** All 42
  dispatches in a boot come from one loop, in the startup region (`fn_00000`,
  `SEG_0000`), at image `0x1ED..0x230`:

  ```
  001F8  cmp byte es:[bx], 0xFF        ; walk ES:SI .. ES:DI,
  001FE  mov cl,  byte es:[bx+0x1]     ; 6-byte records {flag, priority, far ptr}
  0020C  add bx, 0x6                   ; pick the lowest-priority unrun entry
  ...
  00222  call far word es:[bx+0x2]     ; ...and call it
  00229  call word es:[bx+0x2]         ; (near variant)
  ```

  That is Borland's init/exit-list walker over the linker-built `_INIT_`
  segment. `ES:SI`/`ES:DI` are linker-defined segment bounds materialised as
  **relocated immediates**; if they do not land on the real table the loop walks
  arbitrary memory calling whatever it reads. The remaining 7 misses confirm it:
  none of them is a valid instruction boundary, so they are still bad pointers,
  not switch arms. Next step is to check what `ES:SI..ES:DI` actually hold at
  entry against the relocation-applied image.

  (Switch-arm recovery — lifting `case` blocks inline instead of
  tail-dispatching them, upstream's `3412d02` for the 32-bit path — is still
  wanted, but the boot does not reach a `switch` yet, so it is not the blocker.)

## The segment map

Large-model Borland splits DinoPark across **29 code segments**, and a function's
CS decides what its switch tables and near indirect dispatches read.
`work/functions.json` records only flat image offsets, so `build_segmap()` in
`tools/lift_full.py` recovers CS:

1. A far `call`/`jmp` `9A/EA seg:off` states its target's CS outright — **258
   detected functions** seeded directly, with **zero conflicts**.
2. A near call cannot leave its segment, so both ends share one CS — flood-fill
   the seeds across near-call edges (**380/693**).
3. Anything still unseeded takes the nearest seeded function below it (the linker
   lays segments out in order).

Every assignment is guarded by `0 <= fn - CS*16 < 0x10000`: an offset that does
not fit in 16 bits cannot belong to that segment, whatever the edge says.

Independently checked: brute-forcing CS at each `jmp word cs:[bx+disp]` site
under the constraint that *every* table entry must land on a valid instruction
boundary in the enclosing function solves **50 of the 53 tables**, and every
answer agrees with the map above (`fn_15764` → `SEG_13C5`, and so on). The 3
that do not solve are the sparse switch form — a `cmp/je/add bx,2/loop` scan
over `{value, offset}` pairs — which needs its own table reader.

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
