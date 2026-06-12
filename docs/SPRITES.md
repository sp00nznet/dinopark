# `.ACT` actor sprites & the planar blitter (Phase 4)

Goal: render an actual dinosaur (`.ACT` actor). Progress: the sprite **container
is cracked** and the blitter **architecture is mapped** — and the Phase 3
"self-modifying, can't lift" conclusion was **wrong**.

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

## What remains for the render

The control stream is **planar** — processed plane-by-plane by the inner plotter,
not a flat per-row RLE (every linear interpretation overruns the 62-px width). So
a pixel-exact dino needs the blitter **executed**, not the format guessed:

1. Lift `FUN_191d_08fb` + `FUN_191d_0bf7`, overriding the `jmp bx` compiled-blit
   with an equivalent copy loop.
2. Model the VGA planar target in recomp16 (Map-Mask plane select + `0xA000`
   writes), then de-plane to a linear buffer.
3. Feed the sprite struct (the two chunks) + a palette (from the screen `.PIC`).

That's the genuinely hard remaining piece — a focused multi-step lift, now that
the container and architecture are fully understood.
