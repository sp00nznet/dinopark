# Palette trace (Phase 4) — SOLVED ✅

Goal: color the recompiled `.PIC` render. **Done** — the auction screen now
renders in its real DinoPark colors via the recompiled blitter + the palette
embedded in the `.PIC` itself.

## The answer: the palette is in the `.PIC`, at offset 16

Reading the real `.PIC` loader `FUN_1d88_0f75` (called as
`FUN_1d88_0f75("auction.pic", …, param_6=1)` — `param_6` = "load palette") gave
the true full-screen layout. The "RLE stream" I first decoded from offset 16 was
actually **palette + image**:

```
 +0   char[4] "UNCP"
 +4   u32     block offset (= 8)
 +8   u32     block size
 +12  u16     width  (320)
 +14  u16     height (200)
 +16  u8[768] PALETTE   ← 256 × 6-bit RGB  (index 0 forced to black on load)
 +784 …       RLE image stream  (decoded by the lifted fn_1907)
```

The loader copies that 768-byte block into the master palette `DAT_4020_9abf`
(`FUN_1d88_13ad`), which is then faded into the live `DAT_4020_9dbf` and pushed to
the DAC. So once we decode the image from **784** (not 16) and use the bytes at
**[16:784]** as the palette, the colors are exact — verified: index `0x81`=(43,35,32)
tan, `0x83`=(30,23,20) brown — the auction hall's barn-wood walls.

`decode_pic.c` now emits both the image (`pic_decoded.bin`) and the palette
(`pic_palette.pal`); `render_pic.py` applies the palette automatically.

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

## How the palette flows at runtime

```
.PIC[16:784]  ──FUN_1d88_13ad memcpy──▶  DAT_4020_9abf   (master palette)
                                              │  fade-in: pal[i]=(src[i]*level)>>4
                                              ▼
                                          DAT_4020_9dbf   (live palette)
                                              │  color-cycle, screen-effect patches
                                              ▼
                                          DAC (out 0x3C8/0x3C9)
```

The palette is **shipped per-screen in the `.PIC`** and then animated in place:
- `FUN_1f0b_05a9/05f4` copy/fade `DAT_4020_9abf → DAT_4020_9dbf` (fade in/out).
- `FUN_1d88_165b` / `FUN_27e5_0812` **color-cycle** entries (the auction's shimmer).
- `FUN_3619_06e6` patches entries for **screen-effect flashes**.

(The grayscale gamma ramp at EXE `0x34B3F`, R=G=B, is just the fade table — not a
color palette, which is what threw the first pass off.)

## Result

`scripts/build_pic.ps1 original\AUCTION.PIC` now renders the auction hall in its
true colors — image decoded by the **lifted `fn_1907`**, colored by the palette
**lifted straight out of the `.PIC`**. No emulator, no guesswork.

Until then `tools/render_pic.py` renders **grayscale by index** (fully legible —
see the auction hall). A `--pal file.pal` hook is ready for when we capture the
768-byte 6-bit palette from a recomp run.
