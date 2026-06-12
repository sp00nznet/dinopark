#!/usr/bin/env python3
"""
unc_parse.py - Parser for DinoPark Tycoon's "UNC" asset containers.

DinoPark stores graphics in a family of containers tagged by a 4-char magic
(validated by the engine's ReadAct/ReadPic: "Unrecognized header '%4.4s'"):

  UNC2  actor file (.ACT) - compressed sprite set + brush table + opt. scripts
  UNCS  actor file (.ACT) - sprite set variant
  UNCP  picture file (.PIC) - indexed collection of sub-images

Header layout reversed from real files (all multi-byte LE):

  UNC2  (.ACT actor)                  UNCP  (.PIC picture)
  ----------------------------------  --------------------------------
  +0  char[4] "UNC2"                  +0  char[4] "UNCP"
  +4  u16    sprite_count             +4  u16    image_count
  +6  u32    table_off                +6  u16    (pad, 0)
  +10 u16,u16 (flags, often 0)        +8  u32[image_count] offset table
  +14 sprite data (LZSP) ...              (file offsets, monotonic)
  [table_off] u32[sprite_count]
             per-sprite file offsets
             (first entry = 14)

  Sprite i occupies bytes [tbl[i], tbl[i+1]) (last sprite runs to table_off).
  UNCS (.ACT variant, 2 files: BUS/PEOPLE) shares the count but uses an inline
  dimension table instead of a trailing offset table -- layout still TBD.

Validation (this tool): the UNC2 trailing table holds exactly sprite_count u32
offsets, monotonic and in [14, table_off]; UNCP's +8 table is monotonic and
in-bounds. Run across all assets to confirm.

The LZSP sprite codec (ReadActLZSP) and the per-sprite bitmap layout are the
next layer (decode_sprite, TODO). Container parsing alone already gives us the
asset inventory and is enough to drive extraction.

Usage:
    python tools/unc_parse.py [dir]      # default: original/  -- summarize all
    python tools/unc_parse.py file.ACT   # dump one file
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)


def u16(d, o): return struct.unpack_from("<H", d, o)[0]
def u32(d, o): return struct.unpack_from("<I", d, o)[0]


def parse(path):
    d = open(path, "rb").read()
    magic = d[:4].decode("latin1", "replace")
    n = len(d)
    r = {"file": os.path.basename(path), "size": n, "magic": magic}

    if magic == "UNC2":
        cnt = u16(d, 4)
        tbl = u32(d, 6)
        r["sprite_count"] = cnt
        r["table_off"] = tbl
        ok = 0 < tbl <= n and tbl + 4 * cnt <= n
        offs = [u32(d, tbl + 4 * i) for i in range(cnt)] if ok else []
        mono = all(offs[i] <= offs[i + 1] for i in range(len(offs) - 1))
        inb = all(14 <= o <= tbl for o in offs)
        r["sprites_head"] = offs[:4]
        r["sizes_head"] = [offs[i + 1] - offs[i] for i in range(min(4, len(offs) - 1))]
        r["table_ok"] = ok and mono and inb and len(offs) == cnt
    elif magic == "UNCP":
        cnt = u16(d, 4)
        r["image_count"] = cnt
        ok = 8 + 4 * cnt <= n
        offs = [u32(d, 8 + 4 * i) for i in range(cnt)] if ok else []
        mono = all(offs[i] <= offs[i + 1] for i in range(len(offs) - 1))
        inb = all(14 <= o <= n for o in offs)
        atlas = ok and mono and inb and len(offs) == cnt
        # fullscreen variant: a single near-EOF pointer at +8, then pixel data
        first = offs[0] if offs else 0
        fullscreen = bool(offs) and (n - 12) <= first <= n
        r["subtype"] = "atlas" if atlas else "fullscreen" if fullscreen else "?"
        r["offsets_head"] = offs[:5]
        r["table_ok"] = atlas or fullscreen
    elif magic == "UNCS":
        r["sprite_count"] = u16(d, 4)
        r["dims_head"] = [u16(d, 10 + 2 * i) for i in range(8)]
        r["table_ok"] = None  # variant layout, not yet modeled
    else:
        r["unknown"] = True
    return r


def summarize(d):
    files = []
    for f in sorted(os.listdir(d)):
        if f.upper().endswith((".ACT", ".PIC")):
            try:
                files.append(parse(os.path.join(d, f)))
            except Exception as e:
                files.append({"file": f, "error": str(e)})

    by_magic = {}
    for r in files:
        by_magic.setdefault(r.get("magic", "?"), []).append(r)

    print(f"Parsed {len(files)} asset files from {d}\n")
    for magic, rs in sorted(by_magic.items()):
        ok = sum(1 for r in rs if r.get("table_ok"))
        tot = sum(1 for r in rs if r.get("table_ok") is not None)
        print(f"== {magic} ==  {len(rs)} files   structurally valid: {ok}/{tot}"
              + ("  (variant - not modeled)" if tot == 0 else ""))

    print("\n  UNC2 actors (sprite_count, table_off, first sprite sizes):")
    print(f"  {'file':<16}{'sprites':>8}{'table_off':>10}{'size':>9}  ok  sprite sizes")
    for r in files:
        if r.get("magic") == "UNC2":
            print(f"  {r['file']:<16}{r['sprite_count']:>8}{r['table_off']:>10}"
                  f"{r['size']:>9}  {'Y' if r['table_ok'] else 'N'}   {r['sizes_head']}")

    print("\n  UNCP pictures (subtype, image_count, +8 table):")
    print(f"  {'file':<16}{'subtype':<11}{'images':>7}{'size':>9}  ok   first offsets")
    for r in files:
        if r.get("magic") == "UNCP":
            print(f"  {r['file']:<16}{r['subtype']:<11}{r['image_count']:>7}{r['size']:>9}"
                  f"  {'Y' if r['table_ok'] else 'N'}    {r['offsets_head'][:4]}")

    odd = [r["file"] + ":" + r.get("magic", "?") for r in files if r.get("unknown")]
    if odd:
        print("\n  non-UNC / unknown:", odd)


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PROJ, "original")
    if os.path.isdir(arg):
        summarize(arg)
    else:
        import json
        print(json.dumps(parse(arg), indent=2))


if __name__ == "__main__":
    main()
