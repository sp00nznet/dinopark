# `.ACT` actor sprites & the planar blitter (Phase 4) — SOLVED ✅

Goal: render an actual dinosaur (`.ACT` actor). **Done.** `tools/decode_act.py`
decodes **ALBERT.ACT → Albert the Albertosaurus** (62×43, 585/585 pixels exactly),
implementing the control semantics taken straight from the **lifted** planar
blitter. The Phase 3 "self-modifying, can't lift" conclusion was wrong.

## Correction: the blitter is NOT self-modifying

Disassembling `FUN_191d_08fb` / `FUN_191d_0bf7` with our own decoder (Ghidra's
decompiler choked on them) shows what they really are:

- **CS-relative scratch variables, not self-modification.** The `mov cs:[0x10],ax`
  writes target offsets `0x0A–0x2E` — a small **data area at the start of segment
  `0x191d`, before the code** (which begins at `0x8fb`). They're per-blit globals
  parked in the code segment, not patched instructions. Ghidra flagged the writes
  as self-modifying; they aren't.
- **Planar VGA writes.** It programs the Graphics Controller (`out 0x3CE/0x3CF`,
  write mode) and the Sequencer **Map Mask** (`out 0x3C4/0x3C5`) to select planes,
  then writes to `0xA000`. Standard 4-plane (mode-X-style) blitting.
- **A "compiled blit" computed jump.** The inner plotter `FUN_191d_0bf7` does
  `bx = 0x0D9C - 4*count; jmp bx` — a jump into an **unrolled copy block** (4 bytes
  per pixel). That's the "recovered jumptable" Ghidra discarded.

⇒ It **is** liftable. The only non-mechanical part is the computed `jmp bx`, which
needs a manual override (replace with a `count`-iteration copy loop).

## The `.ACT` sprite container — solved ✅

Each sprite (located via the `UNC2` offset table) is:

```
 +0   u16 width            (ALBERT 62)
 +2   u16 height           (ALBERT 43)
 +4   u16, u16, u16        origin / hotspot fields
 +12  u16 control_size     (ALBERT 112)
 +14  u16 pixel_size       (ALBERT 585)
 +16  u8[control_size]     CONTROL stream  (run/skip, plane-aware)
 +…   u8[pixel_size]       PIXEL stream    (raw color indices, opaque pixels only)
```

Verified: `16 + 112 + 585 = 713` = ALBERT sprite-0 length. The **pixel stream is
raw color indices** (0x8c = body color, 111 px; 0x27 = 90 px; … the dino
silhouette). These load as two chunks via `FUN_1d88_04a3` and become the blitter's
`bp` (control, struct+9) and `si` (pixels, struct+0xD).

## The control encoding — recovered from the lifted blitter

Lifting `FUN_191d_08fb` (`tools/lift_dinopark.py` → readable C) made its control
walk explicit. Each control byte:

```
 bit 7 (0x80): start a new scanline   (x = 0, y += 1)
 bit 6 (0x40): DRAW (byte & 0x3f) pixels from the pixel stream
               (clear => SKIP that many transparent pixels)
 0x00        : end of sprite
```

So `9e 48 9d 49 9c 4c …` = *(new row, skip 30, draw 8), (new row, skip 29,
draw 9), (new row, skip 28, draw 12)…* — the dino's head/body centering and
widening. Verified: ALBERT sprite-0 consumes **exactly 585/585** pixels over 43
rows.

The inner plotter `FUN_191d_0bf7` writes those pixels across the 4 VGA planes via
a computed-jump de-interleave (`bx = 0x0D9C − 4·count; jmp bx` into an unrolled
`movsb; add si,3` block). That plane spread is pure storage: de-interleaving
reconstructs **natural x-order**, so the logical image is just the pixel stream
laid left-to-right. `tools/decode_act.py` therefore reproduces the blitter's
output without modelling VGA hardware.

## Render

```
python tools/decode_act.py ALBERT.ACT 0 --pal work/THEORIES.pal
```

→ Albert the Albertosaurus, full sprite, in colour. The palette is per-screen
(from the `.PIC` the dino is shown on); colours shift with the screen, the shape
is exact. Output PNG is gitignored (decoded copyrighted art).
