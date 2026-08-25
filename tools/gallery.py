#!/usr/bin/env python3
"""
gallery.py - decode every DinoPark .PIC with the lifted blitter and tile the
results into one contact sheet.

Every screen is decoded by fn_1907, the game's own RLE routine lifted to C, and
coloured with the palette carried inside each .PIC. So this is both a look at
the art and a wide test of the lift: a screen that decodes wrong shows up as
noise rather than a picture.

    python tools/gallery.py            # -> work/gallery.png
"""
import glob, os, subprocess, sys
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "work", "dino_pic.exe")
COLS, PAD = 5, 6


def nblocks(pic):
    """Block count from the UNCP header: 1 + the size of the offset table."""
    d = open(pic, "rb").read(8)
    off0 = int.from_bytes(d[4:8], "little")
    return 1 + (off0 - 8) // 4 if off0 > 8 else 1


def decode(pic, block=0):
    """Run the lifted decoder over one block; returns a PIL image or None."""
    env = dict(os.environ, DINO_NOWINDOW="1")
    r = subprocess.run([EXE, pic, str(block)], cwd=ROOT, env=env,
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None
    raw = os.path.join(ROOT, "work", "pic_decoded.bin")
    pal = os.path.join(ROOT, "work", "pic_palette.pal")
    dims = os.path.join(ROOT, "work", "pic_dims.txt")
    if not all(os.path.exists(p) for p in (raw, pal, dims)):
        return None
    w, h = (int(x) for x in open(dims).read().split())
    im = Image.frombytes("P", (w, h), open(raw, "rb").read()[:w * h])
    # DAC channels are 6-bit; a stray high bit would push the scaled value
    # past 255, so mask before widening.
    pal6 = [v & 0x3F for v in open(pal, "rb").read()]
    im.putpalette([(v << 2) | (v >> 4) for v in pal6])
    return im.convert("RGB")


def main():
    pics = sorted(glob.glob(os.path.join(ROOT, "original", "*.PIC")))
    if not os.path.exists(EXE):
        sys.exit("build work/dino_pic.exe first: scripts\build_pic.ps1")

    shots = []
    for p in pics:
        name, n, got = os.path.basename(p), nblocks(p), 0
        for b in range(n):
            im = decode(p, b)
            if im:
                shots.append((f"{name}#{b}", im))
                got += 1
        print(f"  {name:<14} {got}/{n} blocks")
    if not shots:
        sys.exit("nothing decoded")

    # Blocks are all sizes -- sprites next to full screens -- so the grid is
    # sized by the largest and everything is centred in its cell.
    w = max(im.width for _, im in shots)
    h = max(im.height for _, im in shots)
    rows = (len(shots) + COLS - 1) // COLS
    sheet = Image.new("RGB", (COLS * (w + PAD) + PAD, rows * (h + PAD) + PAD),
                      (24, 24, 28))
    for i, (_, im) in enumerate(shots):
        x = PAD + (i % COLS) * (w + PAD) + (w - im.width) // 2
        y = PAD + (i // COLS) * (h + PAD) + (h - im.height) // 2
        sheet.paste(im, (x, y))
    out = os.path.join(ROOT, "work", "gallery.png")
    sheet.save(out)
    print(f"{len(shots)} blocks from {len(pics)} files -> {out}")


if __name__ == "__main__":
    main()
