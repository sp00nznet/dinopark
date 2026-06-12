#!/usr/bin/env python3
"""
strings_xref.py - Name DinoPark functions from the strings they reference.

Large-model code reaches a string by pushing its DGROUP-relative offset as a
16-bit immediate. So if we know the DGROUP file base, every `mov r16, imm` /
`push imm` whose imm equals (string_file_off - dgroup_base) is a string xref.

We don't know the DGROUP base a priori, so we recover it by voting: for every
(string F, code immediate V) pair, the implied base is F - V. The true base is
the one that explains the most distinct strings. Then we attribute each xref to
the function that contains the referencing instruction and name the function
after its most distinctive string.

Output: work/xref.json, work/named_functions.csv, and a stdout report.
Builds directly on the Phase 1 map (disasm/largemodel16).

Usage: python tools/strings_xref.py [path/to/DINOPARK.EXE]
"""
import json
import os
import re
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
sys.path.insert(0, os.path.normpath(os.path.join(PROJ, "..", "tools", "tools", "disasm")))

from decode16 import Decoder, OpType            # noqa: E402
from analyze import Analyzer                    # noqa: E402
from largemodel16 import real_functions, build_call_graph  # noqa: E402


def collect_immediates(data, start, end):
    """All 16-bit immediate values appearing as operands in [start,end)."""
    dec = Decoder(data[start:end], base_offset=start)
    insts = dec.decode_all()
    imm_at = []  # (inst_offset, value)
    for ins in insts:
        for op in (ins.op1, ins.op2):
            if not op:
                continue
            v = None
            if op.type == OpType.IMM16:
                v = op.disp & 0xFFFF
            elif op.type in (OpType.MOFFS, OpType.MEM) and not op.base and not op.index:
                v = op.disp & 0xFFFF          # direct [disp] data access
            if v is not None and v != 0:
                imm_at.append((ins.offset, v))
    return imm_at


def find_strings(data, start, end, minlen=4):
    out = []
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minlen, data[start:end]):
        out.append((start + m.start(), m.group().decode("latin1")))
    return out


def recover_dgroup_base(strings, imm_values, lo, hi, align=16):
    """Find the paragraph-aligned DGROUP base that explains the most *distinct*
    strings (a string F is explained if F-base is a code immediate)."""
    immset = set(imm_values)
    str_offs = [f for f, _ in strings]
    first = min(str_offs)
    lo = max(lo, first - 0x10000)            # base within 64K below first string
    lo -= lo % align
    best, best_score = None, -1
    ranked = []
    b = lo - (lo % align)
    while b <= first:
        s = sum(1 for f in str_offs if (f - b) in immset and (f - b) >= 0)
        if s:
            ranked.append((s, b))
        if s > best_score:
            best, best_score = b, s
        b += align
    ranked.sort(reverse=True)
    return best, best_score, ranked[:5]


GAME_HINT = re.compile(r"(dino|park|auction|bank|loan|fence|food|chow|meat|plant|"
                       r"ticket|visitor|buy|sell|sell|pen|egg|hatch|age|extinct|"
                       r"go to|attend|show|item|money|dollar|save|load)", re.I)
NOISE = re.compile(r"^[\s!-/:-@\[-`{-~]+$|%[sd]|^[A-Za-z]:\\")


def score_string(s):
    """Higher = better function name candidate."""
    sc = len(s)
    if GAME_HINT.search(s):
        sc += 100
    if NOISE.search(s) or s.endswith((".XMI", ".ACT", ".PIC", ".ABT")):
        sc -= 40
    return sc


def slug(s):
    return re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_")[:28] or "str"


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PROJ, "original", "DINOPARK.EXE")
    data = open(exe, "rb").read()

    az = Analyzer(data)
    az.find_overlays(); az.detect_all_functions()
    funcs, code_end = real_functions(az)
    callees, callers, _ = build_call_graph(funcs, az.hdr_size, code_end)
    starts = sorted(f.start for f in funcs)
    bounds = {f.start: (f.start, f.end) for f in funcs}

    import bisect

    def containing(off):
        i = bisect.bisect_right(starts, off) - 1
        if 0 <= i < len(funcs) and bounds[starts[i]][0] <= off < bounds[starts[i]][1]:
            return starts[i]
        return None

    imm_at = collect_immediates(data, az.hdr_size, code_end)
    strings = find_strings(data, code_end, az.img_size)
    str_by_off = dict(strings)

    base, score, ranked = recover_dgroup_base(strings, [v for _, v in imm_at],
                                              lo=az.hdr_size, hi=code_end)
    print(f"DGROUP file base = 0x{base:X}  (explains {score} distinct strings)")
    print("  runner-up bases:", "  ".join(f"0x{b:X}:{s}" for s, b in ranked[1:]))

    # Map each immediate to a string (imm == file_off - base) and attribute.
    fn_strings = defaultdict(list)   # func_start -> [(string, ref_off)]
    total_refs = 0
    for off, v in imm_at:
        f = str_by_off.get(base + v)
        if f is None:
            continue
        fn = containing(off)
        if fn is not None:
            fn_strings[fn].append((f, off))
            total_refs += 1

    # Name each function after its best string.
    names = {}
    for fn, refs in fn_strings.items():
        best = max((s for s, _ in refs), key=score_string)
        names[fn] = f"fn_{fn:05X}_{slug(best)}"

    named = sorted(fn_strings.keys(), key=lambda s: len(fn_strings[s]), reverse=True)

    # ---- artifacts ----
    workdir = os.path.join(PROJ, "work"); os.makedirs(workdir, exist_ok=True)
    json.dump({
        "dgroup_base": base, "string_refs": total_refs,
        "named_functions": len(names),
        "functions": {f"{fn:05X}": {
            "name": names[fn], "callers": len(callers[fn]),
            "strings": sorted({s for s, _ in fn_strings[fn]})[:40],
        } for fn in named},
    }, open(os.path.join(workdir, "xref.json"), "w"), indent=1)

    with open(os.path.join(workdir, "named_functions.csv"), "w") as fh:
        fh.write("addr,name,callers,nrefs\n")
        for fn in sorted(names):
            fh.write(f"{fn:X},{names[fn]},{len(callers[fn])},{len(fn_strings[fn])}\n")

    # ---- report ----
    print(f"\nFunctions with string refs: {len(names)} / {len(funcs)}")
    print(f"Total string references attributed: {total_refs}\n")
    print("Most string-heavy functions (likely high-level game logic):")
    print(f"  {'addr':<8} {'callers':>7} {'refs':>5}  sample strings")
    for fn in named[:22]:
        refs = fn_strings[fn]
        samples = sorted({s for s, _ in refs}, key=score_string, reverse=True)[:3]
        samp = " | ".join(s[:24] for s in samples)
        print(f"  0x{fn:05X} {len(callers[fn]):>7} {len(refs):>5}  {samp}")

    # Game-anchor functions specifically
    print("\nFunctions referencing key game actions:")
    anchors = ["Buy Dinos", "Buy Dino Chow", "Attend Auction", "Go to the park",
               "Show dinosaur", "Buy item"]
    for a in anchors:
        hits = [fn for fn in fn_strings if any(a.lower() in s.lower() for s, _ in fn_strings[fn])]
        for fn in hits:
            print(f"  '{a}' -> 0x{fn:05X}  ({len(callers[fn])} callers)")


if __name__ == "__main__":
    main()
