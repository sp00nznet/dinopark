#!/usr/bin/env python3
"""
decode_act.py - decode a DinoPark .ACT actor sprite to a PNG.

Implements the exact control-stream semantics recovered by LIFTING the planar
blitter FUN_191d_08fb (see src/recomp/gen/dino_decode.c / docs/SPRITES.md). Each
sprite (located via the UNC2 offset table) is:

    [u16 w][u16 h][3x u16][u16 control_size][u16 pixel_size]
    [control stream][pixel stream = raw color indices, x-order]

Control byte encoding (from the lifted walker):
    bit 7 (0x80): start a new scanline (x = 0, y += 1)
    bit 6 (0x40): DRAW (byte & 0x3f) pixels from the stream, else SKIP that many
    0x00: end
The blitter stores pixels across 4 VGA planes; that de-interleave reconstructs
natural x-order, so the logical image is just the pixel stream laid left-to-right.

Usage:
    python tools/decode_act.py ALBERT.ACT [sprite_index] [--pal palette.pal]
"""
import os, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def u16(d, o): return struct.unpack_from("<H", d, o)[0]
def u32(d, o): return struct.unpack_from("<I", d, o)[0]


def decode_sprite(act_bytes, index):
    d = act_bytes
    assert d[:4] in (b"UNC2", b"UNCS"), "not a UNC2/UNCS .ACT"
    cnt, tbl = u16(d, 4), u32(d, 6)
    offs = [u32(d, tbl + 4 * i) for i in range(cnt)] + [tbl]
    sp = d[offs[index]:offs[index + 1]]
    w, h, csz, psz = u16(sp, 0), u16(sp, 2), u16(sp, 12), u16(sp, 14)
    ctrl = sp[16:16 + csz]
    pix = sp[16 + csz:16 + csz + psz]

    img = bytearray([255]) * (w * h)       # 255 = transparent
    x = y = pp = 0
    for c in ctrl:
        if c == 0:
            break
        if c & 0x80:                        # new scanline
            y += 1
            x = 0
        n = c & 0x3F
        if c & 0x40:                        # draw n
            for _ in range(n):
                if 0 <= x < w and 0 <= y < h and pp < len(pix):
                    img[y * w + x] = pix[pp]
                pp += 1
                x += 1
        else:                               # skip n
            x += n
    return w, h, img, pp, len(pix)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    path = args[0] if args else "ALBERT.ACT"
    if not os.path.isabs(path) and not os.path.exists(path):
        path = os.path.join(ROOT, "original", path)
    index = int(args[1]) if len(args) > 1 else 0
    pal_path = None
    if "--pal" in sys.argv:
        pal_path = sys.argv[sys.argv.index("--pal") + 1]

    from PIL import Image
    w, h, img, used, total = decode_sprite(open(path, "rb").read(), index)
    print(f"{os.path.basename(path)} sprite {index}: {w}x{h}, "
          f"{used}/{total} pixels {'OK' if used == total else 'MISMATCH'}")

    scale = 6
    if pal_path:
        pal = open(pal_path, "rb").read()[:768]
        pal8 = [min(255, c * 255 // 63) for c in pal]
        # transparent index 255 -> a flat backdrop colour
        im = Image.new("RGB", (w, h), (90, 110, 90))
        px = im.load()
        for i, v in enumerate(img):
            if v != 255:
                px[i % w, i // w] = (pal8[3 * v], pal8[3 * v + 1], pal8[3 * v + 2])
    else:
        im = Image.new("L", (w, h), 210)
        for i, v in enumerate(img):
            if v != 255:
                im.putpixel((i % w, i // w), v)
    out = os.path.join(ROOT, "work", "act_render.png")
    im.resize((w * scale, h * scale), Image.NEAREST).save(out)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
