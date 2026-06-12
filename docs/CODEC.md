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

The loader (`FUN_1862_0797`) does **in-place decompression**: it reads the
compressed sprite to the *tail* of a buffer, then `FUN_1000_0e50` expands it
forward — `*src` (first u16) is the decompressed size, `src[8]` the seed color,
the stream at `src+9`. The bit-expansion helpers (`0f2e/103b/10de`) take their
args in **registers** (`CX`=count, `SI`=src, `DI`=dst) against the `0x0e20`
delta table, so the decompiled C alone doesn't show the wiring — the raw asm is
needed to port them exactly.

The on-screen plotter `FUN_191d_0ab7` decompiles to a stub and the real worker
`FUN_191d_0bf7` is flagged by Ghidra with *overlapping instructions*, *bad
instruction data*, and a *recovered jumptable eliminated as dead code* — i.e.
it's **hand-optimized, jump-table-dispatched, self-modifying planar-VGA
assembly**. That's the fastest blit on a 386, and it does not decompile cleanly.

### Recommended path (don't hand-port the hairy asm)

A pixel-perfect render of these is best produced **the static-recomp way: lift
`FUN_1000_0e50` + helpers (and/or the VGA blitter) and execute the original code
against a buffer**, rather than reimplementing self-modifying VGA asm by hand.
That makes the sprite render a *product of the recomp itself* — the right
verification milestone — and folds into the lifting phase instead of a brittle
parallel reimplementation. The palette comes from the VGA DAC the game programs
at startup (to be captured during bring-up).

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
