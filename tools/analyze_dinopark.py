#!/usr/bin/env python3
"""
analyze_dinopark.py - Phase 1 function map for DinoPark Tycoon (DINOPARK.EXE).

Wraps the shared pcrecomp 16-bit decoder/analyzer (tools/disasm) and adds what
the civ-tuned analyze.py doesn't do for a generic Borland large-model DOS exe:

  1. Code/data boundary detection. The shared analyzer treats the whole MZ
     image as code, so the DGROUP data section decodes into one bogus ~55 KB
     "function". We detect where real prologue-driven functions stop and the
     initialized-data segment begins, and clip the map there.
  2. A clean call graph over *real* functions only: roots (uncalled), leaves
     (call nothing local), hottest callees, biggest functions.
  3. JSON + CSV artifacts in work/ and a human summary to stdout.

Nothing here is DinoPark-specific beyond the default paths; the boundary
heuristic is a candidate to push upstream into pcrecomp/disasm/analyze.py.

Usage:
    python tools/analyze_dinopark.py [path/to/DINOPARK.EXE]
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
PCRECOMP_DISASM = os.path.normpath(os.path.join(PROJ, "..", "tools", "tools", "disasm"))
sys.path.insert(0, PCRECOMP_DISASM)

from analyze import Analyzer  # noqa: E402  (shared pcrecomp tool)
from largemodel16 import real_functions, build_call_graph  # noqa: E402  (upstreamed here)


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PROJ, "original", "DINOPARK.EXE")
    if not os.path.exists(exe):
        sys.exit(f"not found: {exe}\nDrop DINOPARK.EXE into original/ (bring your own game).")

    data = open(exe, "rb").read()
    az = Analyzer(data)
    az.find_overlays()
    az.detect_all_functions()
    az.extract_strings()

    # Code/data boundary + near+far call graph (upstreamed: disasm/largemodel16).
    funcs, code_end = real_functions(az)
    callees, callers, edge_stats = build_call_graph(funcs, az.hdr_size, code_end)
    far_total, far_resolved = edge_stats["far"], edge_stats["far_resolved"]

    roots = [f for f in funcs if not callers[f.start]]            # uncalled (entry/startup/indirect-only)
    leaves = [f for f in funcs if not callees[f.start]]           # call nothing local -- fuzzable units
    hot = sorted(funcs, key=lambda f: len(callers[f.start]), reverse=True)[:25]
    biggest = sorted(funcs, key=lambda f: f.size, reverse=True)[:25]

    total_insts = sum(f.inst_count for f in funcs)

    # ---- artifacts -------------------------------------------------------
    workdir = os.path.join(PROJ, "work")
    os.makedirs(workdir, exist_ok=True)

    fmap = [{
        "name": f"fn_{f.start:05X}",
        "start": f.start, "end": f.end, "size": f.size,
        "insts": f.inst_count, "stack": f.local_size, "far": f.is_far,
        "callees": sorted(callees[f.start]), "callers": sorted(callers[f.start]),
    } for f in funcs]
    json.dump({
        "exe": os.path.basename(exe),
        "image": {"hdr": az.hdr_size, "img_size": az.img_size, "code_end": code_end},
        "totals": {"functions": len(funcs), "instructions": total_insts,
                   "strings": len(az.strings), "roots": len(roots), "leaves": len(leaves)},
        "functions": fmap,
    }, open(os.path.join(workdir, "functions.json"), "w"), indent=1)

    with open(os.path.join(workdir, "functions.csv"), "w") as fh:
        fh.write("start,end,size,insts,stack,far,callers,callees\n")
        for f in funcs:
            fh.write(f"{f.start:X},{f.end:X},{f.size},{f.inst_count},{f.local_size},"
                     f"{int(f.is_far)},{len(callers[f.start])},{len(callees[f.start])}\n")

    # ---- report ----------------------------------------------------------
    print("=" * 68)
    print("  DinoPark Tycoon (1993) - Phase 1 function map")
    print("=" * 68)
    print(f"  Image      : hdr=0x{az.hdr_size:X}  img_end=0x{az.img_size:X}")
    print(f"  Code region: 0x{az.hdr_size:X} - 0x{code_end:X}   "
          f"({(code_end-az.hdr_size)//1024} KB)")
    print(f"  Data region: 0x{code_end:X} - 0x{az.img_size:X}   "
          f"({(az.img_size-code_end)//1024} KB DGROUP)")
    print()
    print(f"  Functions  : {len(funcs)}   (dropped data-blob + >8KB false hits)")
    print(f"  Instructions: {total_insts}")
    print(f"  Strings    : {len(az.strings)}")
    print(f"  Roots (uncalled) : {len(roots)}   Leaves (no local calls): {len(leaves)}")
    print(f"  Far calls  : {far_resolved}/{far_total} resolved into code region")
    print()
    print("  Hottest functions (most callers):")
    print("    addr        callers  callees  size  insts")
    for f in hot:
        print(f"    0x{f.start:05X}    {len(callers[f.start]):5d}   {len(callees[f.start]):5d}"
              f"  {f.size:5d}  {f.inst_count:5d}")
    print()
    print("  Largest functions:")
    print("    addr        size   insts  stack  callees")
    for f in biggest:
        print(f"    0x{f.start:05X}   {f.size:5d}  {f.inst_count:5d}  {f.local_size:5d}"
              f"   {len(callees[f.start])}")
    print()
    print(f"  Wrote work/functions.json and work/functions.csv")


if __name__ == "__main__":
    main()
