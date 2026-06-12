# Phase 4 — The Lift (recomp16 bring-up)

Goal: stop hand-porting the sprite codec and instead **lift the original decode
functions to C and execute them** — the static-recomp way. This phase stands up
the `recomp16` pipeline for DinoPark and proves it runs real game code natively.

## 🎉 Milestone: a DinoPark screen, rendered by recompiled code

`fn_1907` — the lifted **`FUN_1907_00be`** RLE blitter — running on the recomp16
CPU model decodes `AUCTION.PIC` into the full **320×200** auction-hall screen
(attendees in chairs, columns, wall sconces, the auction stage). No emulator, no
interpreter: the original 16-bit blit logic, recompiled to C, executed natively.

```
scripts\build_pic.ps1 original\AUCTION.PIC   # lift -> cl -> decode -> render
# -> work/pic_render.png  (gitignored: it is decoded copyrighted game art)
```

(Rendered grayscale-by-index for now — the VGA DAC palette is programmed at
runtime and not yet traced. The image is fully legible without it.)

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

## Following the loader changed the picture (important)

Lifting toward the loader, the decompiled call tree revealed **two distinct asset
paths** — and they decode differently:

| Asset | Loader | Decode / draw |
|-------|--------|---------------|
| **`.ACT`** actors (the dinosaurs) | `FUN_1d88_0005` → `FUN_1d88_04a3` | Loaded **compressed** into a sprite table (16-byte per-sprite header → two chunks `a168`+`a16a`); drawn on demand by the **self-modifying planar-VGA blitter** `FUN_191d_0bf7`. `fn_0E50` is **not** called. |
| **`.PIC`** screens + **`.ABT`** sprites | `FUN_1862_0797` (open → read → in-place) | Decoded by **`fn_0E50`** (the DPCM decoder we lifted): buffer = decompressed size, compressed stream copied to the buffer *tail*, decode forward in place. |

Caller evidence: `FUN_1862_0797` is invoked for `auction.pic`, `bapa.pic`,
`credits.pic`, and many `.abt` files; the `.act` actors go exclusively through
`FUN_1d88` + the planar blitter.

### Consequence for "render a dinosaur"

The DPCM decoder we successfully lifted renders **`.PIC` full-screen art and
`.ABT` sprites**, not the `.ACT` dinosaur actors. Two honest options:

1. **Render a `.PIC`/`.ABT` now** — the lifted `fn_0E50` already runs; it needs
   the in-place file framing replicated (decompressed size + compressed length
   from the file header) to feed it correctly. This yields a real DinoPark
   *screen* image from recompiled code — the nearest tractable visual.
2. **Render an actual `.ACT` dinosaur** — requires lifting the **self-modifying,
   jump-table planar-VGA blitter** `FUN_191d_0bf7` (which Ghidra cannot
   decompile) and modelling VGA plane writes. Much harder; a dedicated effort.

Either way the lift pipeline stands and executes; the remaining work is framing
(`.PIC`) or a hard blitter lift (`.ACT`).
