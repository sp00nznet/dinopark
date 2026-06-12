#!/usr/bin/env python3
"""
render_pic.py - render the lifted RLE blitter's output to a PNG.

Reads work/pic_decoded.bin (8bpp indices, produced by the lifted fn_1907 via
src/decode_pic.c) and work/pic_dims.txt, writes work/pic_render.png.

Palette: the game programs the VGA DAC at runtime, which we haven't traced yet,
so this renders grayscale by index for now (the image is fully legible). A
palette hook (--pal file.pal, 768 bytes of 6-bit RGB) is supported for when we
capture it. The output PNG is gitignored — it is decoded copyrighted game art.
"""
import os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, "work")


def main():
    from PIL import Image
    W, H = map(int, open(os.path.join(WORK, "pic_dims.txt")).read().split()[:2])
    buf = open(os.path.join(WORK, "pic_decoded.bin"), "rb").read()[:W * H]

    pal_path = None
    for i, a in enumerate(sys.argv):
        if a == "--pal" and i + 1 < len(sys.argv):
            pal_path = sys.argv[i + 1]

    if pal_path:
        raw = open(pal_path, "rb").read()[:768]
        pal = [min(255, c * 255 // 63) for c in raw]      # 6-bit DAC -> 8-bit
        img = Image.frombytes("P", (W, H), buf)
        img.putpalette(pal + [0] * (768 - len(pal)))
        img = img.convert("RGB")
    else:
        img = Image.frombytes("L", (W, H), buf)           # grayscale by index

    scale = 2
    img = img.resize((W * scale, H * scale), Image.NEAREST)
    out = os.path.join(WORK, "pic_render.png")
    img.save(out)
    print(f"wrote {out}  ({W}x{H}{' + palette' if pal_path else ' grayscale'})")


if __name__ == "__main__":
    main()
