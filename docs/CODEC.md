# Sprite Codec — located in the binary (Phase 3)

The `UNC2` sprites are **not** simple RLE (a naive skip/literal decode under-fills
the frame). Ghidra headless decompilation of `DINOPARK.EXE` (839 functions →
`work/ghidra/DINOPARK.EXE_decompiled.c`, 1.27 MB) pinned the real codecs. The
format is **opcode-driven with DPCM/delta color compression**, blitted through
VGA planes.

> Found by importing the MZ into Ghidra (`x86 real mode`), auto-analyzing, then
> `DecompileAll.java`. The loader error strings are **far** strings, so xref
> search didn't reach them — instead the codecs were found by their decompiled
> shape (`& 0x80` / `& 0x40` control-bit loops + VGA `out 0x3ce/0x3cf`).

## The three codec layers (Ghidra `seg:off` names)

### `FUN_1907_00be` — linear RLE / raw copy
Signature `(src far, dst far, count_lo, count_hi, mode)`.
- `mode < 0x41`: straight byte copy — **uncompressed** sprites (the `UNCS`
  variant: `BUS`, `PEOPLE`).
- else: RLE — control byte `c`:
  - `c & 0x80 == 0`: copy a **literal run** of `c+1` source bytes.
  - `c & 0x80 != 0`: **fill run** of `(1 − (int8)c)` copies of the next byte.

### `FUN_1000_0e50` (+ helpers `0f2e`, `103b`, `10de`) — DPCM scanline decode
Decodes one scanline of a sprite struct (`+0` width, `+9` data stream, `+8`
seed color). Per control byte `b`:
- `b & 0x80`: emit one pixel, color `= b << 1`.
- `b & 0x40`: **run** of `b & 0x3f` pixels of the current color.
- else: **delta block** — `op = b >> 4` (1/2/other → 8/4/2-bit groups),
  `n = (b & 0x0f) + 1`. Builds a ±delta table at `0x0e20`, then the helper
  (`0f2e`=8/byte, `103b`=4/byte, `10de`=2/byte) walks the next bytes bit-by-bit,
  adding `+delta`/`−delta` to a running color, **clamped 0–255**. Classic DPCM —
  smooth dino shading stored as deltas, not absolute indices.

### `FUN_191d_08fb` (+ `FUN_191d_0ab7`) — VGA planar blit
The on-screen path. Programs the VGA Graphics Controller (`out 0x3ce` index,
`out 0x3cf` data), walks the sprite control stream (`& 0x80`/`& 0x40`/`& 0x3f`),
and writes to planar VGA memory. References `s_Hypsilophodon` — i.e. this is the
function that actually draws the dinosaurs.

## What this means for rendering

A faithful PNG render needs all three modeled: the per-row control opcodes, the
DPCM delta reconstruction (+ the `0x0e20` delta table), the row-iteration that
calls the scanline decoder, and the VGA palette. That's a focused port of
`FUN_1000_0e50`/`191d_08fb` — the **next Phase 3 step**, now that the exact
functions and algorithm are known.

## Reproducing the Ghidra analysis

```bash
HEADLESS=/c/tools/ghidra/ghidra_12.0.3_PUBLIC/support/analyzeHeadless.bat
# import + auto-analyze (writes work/ghidra/dino project)
"$HEADLESS" work/ghidra dino -import original/DINOPARK.EXE
# decompile everything to one .c
"$HEADLESS" work/ghidra dino -process DINOPARK.EXE \
  -scriptPath ../tools/tools/ghidra -postScript DecompileAll.java work/ghidra
```

Ghidra maps DGROUP at segment `0x4000`; DS-relative string offset =
`file_off − 0x34800` (the true DGROUP file base, confirmed via the loader-string
addresses). The project and the 1.27 MB decompiled C live under `work/`
(gitignored — derived from the copyrighted binary).
