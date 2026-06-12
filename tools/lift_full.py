#!/usr/bin/env python3
"""
lift_full.py - lift the ENTIRE DinoPark program to C (playable bring-up, phase 1).

Lifts every function in work/functions.json against the recomp16 CPU model, in
image-offset space (file - 0x4800, so a Ghidra seg:off maps to seg*16+off). Near
and far calls resolve to fn_XXXXX symbols; everything else (indirect calls/jmps,
INT, port I/O) routes through the runtime. Emits chunked C + a dispatch table so
indirect calls can find their target by address.

Output: src/recomp/gen/recomp_*.c, recomp_all.h, recomp_dispatch.c
"""
import json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = r"<pcrecomp>"
sys.path.insert(0, os.path.join(TOOLS, "tools", "disasm"))
sys.path.insert(0, os.path.join(TOOLS, "tools", "lift"))
from decode16 import Decoder        # noqa: E402
from lift16 import Lifter           # noqa: E402

HDR = 0x4800
EXE = os.path.join(ROOT, "original", "DINOPARK.EXE")
OUT = os.path.join(ROOT, "src", "recomp", "gen")
CHUNK = 60
ENTRY_IMG = 0x0000                 # CS:IP = 0000:0000 -> image offset 0


def main():
    data = open(EXE, "rb").read()
    fmap = json.load(open(os.path.join(ROOT, "work", "functions.json")))["functions"]

    starts = {f["start"] - HDR for f in fmap}        # image offsets of detected funcs
    first_det = min(starts)
    det_end = {f["start"] - HDR: f["end"] - HDR for f in fmap}

    # The Borland c0 startup (image 0 .. first detected func) is one tangled
    # function with internal jmps; the analyzer missed it (no prologue). Lift the
    # WHOLE region as fn_00000 so internal jmps stay as labels, and ALSO expose
    # its near-CALL targets as their own (overlapping) functions so subroutine
    # calls resolve. Call targets are found by one linear decode of the region.
    from decode16 import OpType
    call_tgts = set()
    try:
        for ins in Decoder(data[HDR:first_det + HDR], base_offset=0).decode_all():
            if ins.mnemonic == "call" and ins.op1 and ins.op1.type == OpType.REL16:
                t = ins.op1.disp & 0xFFFF
                if 0 < t < first_det:
                    call_tgts.add(t)
    except Exception:
        pass

    # Iterative bring-up: dispatch misses from the previous boot become forced
    # function starts (mid-function jump-table / init-routine targets). These
    # overlap existing functions; the lifter handles that fine.
    miss_path = os.path.join(ROOT, "work", "dino_misses.txt")
    forced_targets = set()
    if os.path.exists(miss_path):
        for line in open(miss_path):
            line = line.strip()
            if line:
                t = int(line, 16)
                if t not in starts and 0 < t < max(det_end.values()):
                    forced_targets.add(t)

    funcs = [("fn_00000", 0, first_det)]                # whole startup (jmp labels)
    ct = sorted(call_tgts)
    for i, s in enumerate(ct):                          # call-target subroutines
        end = ct[i + 1] if i + 1 < len(ct) else first_det
        funcs.append((f"fn_{s:05X}", s, end))
    for s in sorted(starts):
        funcs.append((f"fn_{s:05X}", s, det_end[s]))
    # forced miss-targets: span to the next detected/forced start
    allpts = sorted(starts | forced_targets)
    for s in sorted(forced_targets):
        i = allpts.index(s)
        end = allpts[i + 1] if i + 1 < len(allpts) else max(det_end.values())
        funcs.append((f"fn_{s:05X}", s, end))
    if forced_targets:
        print(f"  forced miss-target functions: {len(forced_targets)}")
    # de-dup by start (detected funcs win their real end)
    seen = {}
    for name, s, e in funcs:
        if s not in seen or s in det_end:
            seen[s] = (name, s, det_end.get(s, e))
    funcs = sorted(seen.values(), key=lambda t: t[1])
    known = {io: name for name, io, _ in funcs}
    print(f"  startup: fn_00000=[0,0x{first_det:X}) + {len(ct)} call-target subroutines")

    os.makedirs(OUT, exist_ok=True)
    bodies, all_calls, lifted = [], set(), 0
    for name, io, end in funcs:
        fstart, fend = io + HDR, end + HDR
        try:
            insns = Decoder(data[fstart:fend], base_offset=io).decode_all()
            lifter = Lifter(hdr_size=HDR, known_funcs=known)
            lifter.dispatch = True       # emit real recomp_dispatch for indirect call/jmp
            bodies.append(lifter.lift_function(name, insns, io, is_far=True))
            all_calls |= lifter.func_calls
            lifted += 1
        except Exception as e:
            bodies.append(f"/* {name}: lift failed: {e} */\nvoid {name}(CPU*cpu){{(void)cpu;}}")

    names = [n for n, _, _ in funcs]
    nameset = set(names)
    stubs = sorted(c for c in all_calls if c not in nameset)

    # chunked sources
    os.makedirs(OUT, exist_ok=True)
    for old in os.listdir(OUT):
        if old.startswith("recomp_") and old.endswith(".c"):
            os.remove(os.path.join(OUT, old))
    nchunks = 0
    for i in range(0, len(bodies), CHUNK):
        with open(os.path.join(OUT, f"recomp_{i//CHUNK:03d}.c"), "w") as f:
            f.write('#include "recomp_all.h"\n\n')
            f.write("\n\n".join(bodies[i:i + CHUNK]) + "\n")
        nchunks += 1

    with open(os.path.join(OUT, "recomp_all.h"), "w") as f:
        f.write("#ifndef RECOMP_ALL_H\n#define RECOMP_ALL_H\n")
        f.write('#include "../cpu.h"\n#include "../runtime16.h"\n\n')
        for n in names:
            f.write(f"void {n}(CPU *cpu);\n")
        f.write("\n#endif\n")

    # dispatch table: image addr -> fn ptr (for indirect call/jmp by address)
    with open(os.path.join(OUT, "recomp_dispatch.c"), "w") as f:
        f.write('#include "recomp_all.h"\n#include <stdio.h>\n#include <stdlib.h>\n\n')
        f.write("typedef void (*recomp_fn)(CPU *);\n")
        f.write("typedef struct { unsigned addr; recomp_fn fn; } DispEntry;\n")
        f.write(f"static const DispEntry g_disp[{len(names)}] = {{\n")
        for name, io, _ in funcs:
            f.write(f"  {{0x{io:05X}, {name}}},\n")
        f.write("};\n\n")
        f.write(f"#define N_DISP {len(names)}\n")
        f.write(f"#define CODE_END 0x{first_det:05X}u  /* startup end; real code begins here */\n")
        f.write(f"#define CODE_TOP 0x{max(io for _, io, _ in funcs):05X}u\n")
        f.write("""
static int g_disp_trace = -1;
static int g_disp_depth = 0;
/* in-range dispatch misses, deduped, dumped for the next lift round */
#define MAXMISS 4096
static unsigned g_miss[MAXMISS]; static int g_nmiss;
static void note_miss(unsigned addr) {
    if (addr < CODE_END || addr > CODE_TOP) return;   /* code range only */
    for (int i = 0; i < g_nmiss; i++) if (g_miss[i] == addr) return;
    if (g_nmiss < MAXMISS) g_miss[g_nmiss++] = addr;
}
void recomp_dump_misses(const char *path) {
    FILE *f = fopen(path, "w"); if (!f) return;
    for (int i = 0; i < g_nmiss; i++) fprintf(f, "%05X\\n", g_miss[i]);
    fclose(f);
    fprintf(stderr, "[disp] %d distinct in-range misses -> %s\\n", g_nmiss, path);
}

/* total dispatch budget: deep init currently runs into garbage function
   pointers (uninitialized data) and would spin forever — cap it so the boot
   always terminates while that data-setup work is in progress. */
static long g_disp_budget = -1;
/* indirect call/jmp by (seg,off): linear = seg*16+off (image space) */
void recomp_dispatch(CPU *cpu, unsigned seg, unsigned off) {
    unsigned addr = ((seg << 4) + off) & 0xFFFFF;
    if (g_disp_trace < 0) g_disp_trace = getenv("DINO_TRACE") ? 1 : 0;
    if (g_disp_budget < 0) { const char *e = getenv("DINO_BUDGET"); g_disp_budget = e ? atol(e) : 200000; }
    if (addr == 0) return;                              /* uninitialized fnptr */
    if (--g_disp_budget <= 0) { cpu->halted = 1; return; }
    if (g_disp_depth > 240) { if (g_disp_trace) fprintf(stderr, "[disp] depth cap\\n"); return; }
    for (int i = 0; i < N_DISP; i++)
        if (g_disp[i].addr == addr) {
            if (g_disp_trace) fprintf(stderr, "[disp] -> fn_%05X\\n", addr);
            g_disp_depth++; g_disp[i].fn(cpu); g_disp_depth--; return;
        }
    note_miss(addr);
    if (g_disp_trace) fprintf(stderr, "[disp] MISS %04X:%04X (lin 0x%05X)\\n", seg, off, addr);
}
""")
        # empty stubs for unlifted callees
        for s in stubs:
            f.write(f"void {s}(CPU *cpu) {{ (void)cpu; }}\n")

    print(f"Lifted {lifted}/{len(funcs)} functions -> {nchunks} chunks in {OUT}")
    print(f"  dispatch entries: {len(names)}  unresolved-call stubs: {len(stubs)}")
    print(f"  entry image offset: 0x{ENTRY_IMG:05X}"
          + ("  (a detected function)" if ENTRY_IMG in known else "  (NOT detected — startup stub needed)"))


if __name__ == "__main__":
    main()
