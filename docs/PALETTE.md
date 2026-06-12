# Palette trace (Phase 4)

Goal: color the recompiled `.PIC` render. Result: the palette pipeline is fully
traced, and the finding is that DinoPark's palette is **built at runtime in
code** — there is no static palette to lift out.

## The DAC pipeline (traced end to end)

```
FUN_191d_1b1c(start, count, src)     the VGA DAC writer
    out(0x3DA) vsync wait
    out(0x3C8, start)                set DAC write index
    repeat count*3: out(0x3C9, *src) stream 6-bit R,G,B triples
        ▲
FUN_1f0b_1140(pal, _, first, last)   set a DAC range from a palette buffer
    -> FUN_191d_1b1c(first, last-first+1, pal + first*3)
        ▲
DAT_4020_9dbf  (DS:0x9dbf)           the active 256-color, 6-bit palette buffer
    - BSS in the image (all zeros) — filled at runtime, not stored on disk
    - the video-driver entry is indirect: (*DAT_4020_a2aa)(0x1000, &DAT_4020_9dbf)
```

## Where the palette comes from — and doesn't

Confirmed by exhaustive search:

- ❌ **Not in the `.PIC` files.** AUCTION.PIC is `[UNCP][type=8][pad][u32 tail]
  [W=320][H=200][RLE]`; the whole stream `[16:tail]` decodes to the 320×200
  image (the trailing bytes are decode overshoot, not a palette). No 768-byte
  6-bit block exists anywhere in the file.
- ❌ **Not a static blob in `DINOPARK.EXE`.** The only all-≤63 768-byte region is
  a **grayscale gamma ramp** (`0x34B3F`, R=G=B) used for fades — not a color palette.
- ❌ **Not in any asset file** (`.ACT`, `.ABT`, `DINOSG.00x`, `PRODUCT.PF`, …).
- ❌ **Not the standard mode-13h VGA palette** — applying it gives the right
  *structure* but wrong (psychedelic) hues, so the game reprograms the DAC.

What the code actually does to `DAT_4020_9dbf`:
- `FUN_1f0b_0518` **fades** it (`pal[i] = (pal[i] * level) >> 4`, level 16→0).
- `FUN_1d88_165b` / `FUN_27e5_0812` **color-cycle** it (rotate entries by 3 bytes).
- `FUN_3619_06e6` saves/restores it and patches entries for **screen effects**
  (e.g. `FUN_1000_5000(..., 0x3f, 9)` = flash 9 entries to white).

I.e. the palette is **assembled and animated procedurally** at runtime; it is
never read as one block from disk.

## Consequence

Coloring the render exactly requires **executing the palette-build path** — the
same static-recomp approach that produced the image itself: lift the screen-setup
/ palette-init code and run it to capture `DAT_4020_9dbf`. That's the next step.

Until then `tools/render_pic.py` renders **grayscale by index** (fully legible —
see the auction hall). A `--pal file.pal` hook is ready for when we capture the
768-byte 6-bit palette from a recomp run.
