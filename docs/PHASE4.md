# Phase 4 — The Lift (recomp16 bring-up)

Goal: stop hand-porting the sprite codec and instead **lift the original decode
functions to C and execute them** — the static-recomp way. This phase stands up
the `recomp16` pipeline for DinoPark and proves it runs real game code natively.

## What works ✅

The DPCM sprite decoder group is **lifted and executing as native C**:

```
tools/lift_dinopark.py   ->  src/recomp/gen/dino_decode.c   (4 functions)
    fn_0E50  scanline DPCM decoder        (FUN_1000_0e50)
    fn_0F2E  8-bit-group delta expand     (FUN_1000_0f2e)
    fn_103B  4-bit-group delta expand     (FUN_1000_103b)
    fn_10DE  2-bit-group delta expand     (FUN_1000_10de)
```

- Lifted against the **recomp16 CPU model** (`src/recomp/cpu.[ch]`): a flat 1 MB
  real-mode address space, all registers in a `CPU` struct, `seg:off` translation.
- The four near-call into each other and resolve cleanly within the group — no
  external dependencies except the `cs:0x0E20` delta-table scratch.
- `src/decode_harness.c` builds the far-pointer cdecl frame and calls `fn_0E50`
  on a real `ALBERT.ACT` sprite. **It runs to completion natively** (consumes
  input, drives the DPCM helpers via shared CPU state, writes output) — no
  emulator, no interpreter. This is the first DinoPark machine code executing as
  recompiled C.

Build & run (MSVC):
```
cl /I src /I src\recomp src\decode_harness.c src\recomp\cpu.c \
   src\recomp\gen\dino_decode.c /Fe:work\dino_decode.exe
work\dino_decode.exe original\ALBERT.ACT 0
```

## The remaining gap (what the render needs next)

The decoder reads a 9-byte in-buffer header (`word0` = pixel count, then a few
fields + a seed color) and decodes `word0` pixels. Crucially, that input buffer
is **not** the raw `.ACT` file bytes — the game's loader `FUN_1862_0797` *reframes*
each sprite before decoding:

- reads the real decompressed size and seed from the file header,
- allocates a buffer and copies the compressed stream to its tail,
- calls `fn_0E50` with `word0` = the **full** decompressed size.

Feeding `fn_0E50` the raw file layout (as the harness does now) therefore runs
the decoder correctly but on **mis-framed input** — so it terminates early
instead of producing the full 62×43 image. Confirmed empirically: a single call
consumes 15 bytes / ~58 px then stops; forcing a larger pixel budget loops
forever because the stream framing doesn't match.

**Next step:** lift (or faithfully replicate) `FUN_1862_0797`'s buffer
construction so `fn_0E50` receives a loader-correct input. Then one decode pass
yields the full sprite, the VGA DAC palette (programmed at startup) colors it,
and we render the dinosaur to PNG — produced *by the recompiled code itself*.

## Why this is the right path

We are no longer reverse-engineering the codec by hand (Phase 3 showed the
on-screen blitter is self-modifying VGA asm that doesn't decompile). Instead the
**recomp executes the original logic**. The pipeline now stands; finishing the
render is a matter of framing its input, which is ordinary lifting work — not a
brittle reimplementation.
