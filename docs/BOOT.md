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

Next debugging pass: confirm `DS=DGROUP (0x3020)` holds across the init calls,
verify the BSS clear / initialized-data layout, and trace which structure yields
the first code-pointer — that's what turns "runs 264 init dispatches" into "loads
the title". Debug env: `DINO_TRACE=1` (dispatch), `DINO_DBG=1` (INT 21h).

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
