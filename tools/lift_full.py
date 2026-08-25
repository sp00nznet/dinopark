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
from decode16 import Decoder, OpType  # noqa: E402
import lift16                       # noqa: E402  (to set _CODE_SEG per function)
from lift16 import Lifter           # noqa: E402

HDR = 0x4800
EXE = os.path.join(ROOT, "original", "DINOPARK.EXE")
OUT = os.path.join(ROOT, "src", "recomp", "gen")
CHUNK = 60
ENTRY_IMG = 0x0000                 # CS:IP = 0000:0000 -> image offset 0


def build_segmap(data, ends, decode):
    """image offset -> the code segment (CS) each function runs under.

    Large-model Borland splits the program across ~29 code segments, and a
    function's CS decides what `cs:[bx+table]` (every switch) and every near
    indirect dispatch actually read. Nothing in functions.json records it, so
    recover it:

      1. A far call/jmp `9A/EA seg:off` states its target's CS outright.
      2. A near call cannot leave its segment, so both ends share one CS --
         flood-fill the seeds across near-call edges.
      3. Anything still unseeded takes the segment of the nearest seeded
         function below it (the linker lays segments out in order).

    Every assignment is guarded by `0 <= fn - CS*16 < 0x10000`: an offset that
    does not fit in 16 bits cannot be that segment, whatever the edge says.
    """
    import bisect, collections
    seg, near = {}, collections.defaultdict(set)
    for s in sorted(ends):
        for i in decode(s):
            o = i.op1
            if i.mnemonic in ("call", "call far", "jmp", "jmp far") and o and o.type == OpType.FAR:
                seg[(o.far_seg * 16 + o.disp) & 0xFFFFF] = o.far_seg
            elif i.mnemonic == "call" and o and o.type == OpType.REL16:
                t = s + o.disp
                if t in ends:
                    near[s].add(t); near[t].add(s)
    # The MZ header's entry CS:IP is a seed like any far call: DinoPark starts
    # at 0000:0000, so the whole Borland startup region runs under CS=0. Without
    # it nothing is seeded below the first detected function and the startup's
    # subroutines fall through to the paragraph-of-their-own last resort, which
    # gives the init-list walker a CS its own near calls do not share.
    seg[ENTRY_IMG] = 0

    n_seed = sum(1 for s in ends if s in seg)

    fits = lambda fn, cs: 0 <= fn - cs * 16 < 0x10000
    work = [s for s in seg if s in ends]
    while work:
        a = work.pop()
        for b in near[a]:
            if b not in seg and fits(b, seg[a]):
                seg[b] = seg[a]; work.append(b)
    n_prop = sum(1 for s in ends if s in seg)

    ks = sorted(k for k in seg if k in ends or k == ENTRY_IMG)

    def cs_of(fn):
        if fn in seg and fits(fn, seg[fn]):
            return seg[fn]
        i = bisect.bisect_right(ks, fn) - 1
        while i >= 0:                       # nearest seeded below that still fits
            if fits(fn, seg[ks[i]]):
                return seg[ks[i]]
            i -= 1
        return fn >> 4                      # last resort: its own paragraph
    print(f"  segments: {n_seed} far-seeded, {n_prop}/{len(ends)} after near-propagation")
    return cs_of


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
    detlist = sorted(starts)
    import bisect as _bi

    def valid_boundary(t):
        """t is a real instruction start inside its containing detected function
        (filters garbage in-range pointers that aren't entry points)."""
        i = _bi.bisect_right(detlist, t) - 1
        if i < 0:
            return False
        fs = detlist[i]
        fe = det_end[fs]
        if not (fs < t < fe):
            return False
        try:
            offs = {ins.offset for ins in Decoder(data[fs + HDR:fe + HDR], base_offset=fs).decode_all()}
        except Exception:
            return False
        return t in offs

    _dcache = {}

    def decode(s):
        if s not in _dcache:
            try:
                _dcache[s] = Decoder(data[s + HDR:det_end[s] + HDR], base_offset=s).decode_all()
            except Exception:
                _dcache[s] = []
        return _dcache[s]

    cs_of = build_segmap(data, det_end, decode)

    forced_targets = set()
    if os.path.exists(miss_path):
        for line in open(miss_path):
            line = line.strip()
            if line and int(line, 16) not in starts and valid_boundary(int(line, 16)):
                forced_targets.add(int(line, 16))

    # Direct far calls resolve at lift time, so an unresolved one silently
    # becomes an empty stub rather than a runtime miss the convergence loop
    # could catch. Borland omits the push bp prologue for functions with no
    # locals, so the analyzer merges them into whatever precedes them and
    # thousands of calls land mid-function. Anything a far call points at is a
    # function entry by definition -- register the ones that are real
    # instruction boundaries.
    n_farfix = n_nearfix = 0
    for s in sorted(starts):
        try:
            insns = Decoder(data[s + HDR:det_end[s] + HDR], base_offset=s).decode_all()
        except Exception:
            continue
        for ins in insns:
            o = ins.op1
            if ins.mnemonic in ('call', 'call far') and o and o.type == OpType.FAR:
                t = (o.far_seg * 16 + o.disp) & 0xFFFFF
                if t not in starts and t not in forced_targets and valid_boundary(t):
                    forced_targets.add(t)
                    n_farfix += 1
            if ins.mnemonic == 'call' and o and o.type == OpType.REL16:
                # Same hole on the near side: an unresolved near call is
                # named res_XXXXXX and stubbed out, so it silently does
                # nothing. Wrap the target the way the lifter will.
                base = cs_of(s) * 16
                t = base + ((s - base + o.disp) & 0xFFFF)
                if t not in starts and t not in forced_targets and valid_boundary(t):
                    forced_targets.add(t)
                    n_nearfix += 1
    if n_farfix or n_nearfix:
        print(f"  call targets promoted to functions: {n_farfix} far, {n_nearfix} near")

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
    bodies, all_calls, lifted, n_wrapped = [], set(), 0, 0
    segs_used = set()
    for name, io, end in funcs:
        fstart, fend = io + HDR, end + HDR
        cs = cs_of(io)
        segs_used.add(cs)
        try:
            insns = Decoder(data[fstart:fend], base_offset=io).decode_all()
            # A near call/jmp target is CS:(IP + rel) and the offset wraps at 16
            # bits; the lifter resolves it as func_start + disp, which is only the
            # same thing when it does not wrap. Every call backwards past the
            # segment base wraps, and 707 of DinoPark's 1136 near calls landed
            # somewhere arbitrary because of it. Rewrite disp to the wrap-correct
            # target so the lifter's own arithmetic comes out right.
            base = cs * 16
            for ins in insns:
                o = ins.op1
                if ins.mnemonic in ('call', 'jmp') and o and o.type == OpType.REL16:
                    t = base + ((io - base + o.disp) & 0xFFFF)
                    if t != io + o.disp:
                        o.disp = t - io
                        n_wrapped += 1
            lifter = Lifter(hdr_size=HDR, known_funcs=known)
            lifter.dispatch = True       # emit real recomp_dispatch for indirect call/jmp
            # known_funcs is keyed by image offset, and a far seg:off IS that
            # offset here (the loader applies the MZ relocations). Without this
            # the lifter falls back to Civ's overlay-corrected formulas, misses
            # every time, and 3,574 far calls -- nearly every call a large-model
            # program makes -- compile to empty stubs.
            lifter.far_base = 0
            # cs-relative reads (switch tables, `push cs`) must use this constant:
            # cpu->cs is set once at load and goes stale across the C-call dispatch.
            lift16._CODE_SEG = f"{cs:04X}"
            body = lifter.lift_function(name, insns, io, is_far=True)
            # keep the live cpu->cs honest too -- traces and the runtime read it.
            bodies.append(body.replace("(CPU *cpu)\n{",
                                       f"(CPU *cpu)\n{{\n    cpu->cs = SEG_{cs:04X};", 1))
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
        for sg in sorted(segs_used):
            f.write(f"#define SEG_{sg:04X} 0x{sg:04X}\n")
        f.write("\n")
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
    /* merge with the existing file so the forced-function set grows monotonically
     * across convergence rounds (a shallow run must not shrink it). */
    FILE *r = fopen(path, "r");
    if (r) { unsigned a; while (fscanf(r, "%x", &a) == 1) note_miss(a); fclose(r); }
    FILE *f = fopen(path, "w"); if (!f) return;
    for (int i = 0; i < g_nmiss; i++) fprintf(f, "%05X\\n", g_miss[i]);
    fclose(f);
    fprintf(stderr, "[disp] %d cumulative in-range misses -> %s\\n", g_nmiss, path);
}

/* total dispatch budget: deep init currently runs into garbage function
   pointers (uninitialized data) and would spin forever — cap it so the boot
   always terminates while that data-setup work is in progress. */
static long g_disp_budget = -1;
/* indirect call/jmp by (seg,off): linear = seg*16+off (image space) */
int recomp_dispatch(CPU *cpu, unsigned seg, unsigned off) {
    unsigned addr = ((seg << 4) + off) & 0xFFFFF;
    if (g_disp_trace < 0) g_disp_trace = getenv("DINO_TRACE") ? 1 : 0;
    if (g_disp_budget < 0) { const char *e = getenv("DINO_BUDGET"); g_disp_budget = e ? atol(e) : 200000; }
    if (addr == 0) return 0;                            /* uninitialized fnptr */
    if (--g_disp_budget <= 0) { cpu->halted = 1; return 0; }
    if (g_disp_depth > 240) { if (g_disp_trace) fprintf(stderr, "[disp] depth cap\\n"); return 0; }
    static long seq = 0; long n = seq++;
    int garbage = (addr > CODE_TOP + 0x2000);   /* target beyond all code = bad ptr */
    for (int i = 0; i < N_DISP; i++)
        if (g_disp[i].addr == addr) {
            if (g_disp_trace && n < 80) fprintf(stderr, "[%03ld] ds=%04X cs=%04X -> fn_%05X\\n", n, cpu->ds, cpu->cs, addr);
            g_disp_depth++; g_disp[i].fn(cpu); g_disp_depth--; return 1;
        }
    note_miss(addr);
    if (g_disp_trace && (n < 80 || garbage))
        fprintf(stderr, "[%03ld] ds=%04X cs=%04X MISS %04X:%04X%s\\n", n, cpu->ds, cpu->cs, seg, off, garbage ? " <GARBAGE>" : "");
    return 0;
}

/* A near indirect call through a bad pointer must not leave behind the dummy
 * return word the lifted code pushed: the caller reads its own locals off
 * that stack, so one stale slot desynchronises everything it does next. */
void dispatch_near(CPU *cpu, unsigned seg, unsigned off) {
    if (!recomp_dispatch(cpu, seg, off)) cpu->sp += 2;
}
""")
        # empty stubs for unlifted callees
        for s in stubs:
            f.write(f"void {s}(CPU *cpu) {{ (void)cpu; }}\n")

    print(f"Lifted {lifted}/{len(funcs)} functions -> {nchunks} chunks in {OUT}")
    print(f"  code segments: {len(segs_used)}")
    print(f"  wrap-corrected near call/jmp targets: {n_wrapped}")
    print(f"  dispatch entries: {len(names)}  unresolved-call stubs: {len(stubs)}")
    print(f"  entry image offset: 0x{ENTRY_IMG:05X}"
          + ("  (a detected function)" if ENTRY_IMG in known else "  (NOT detected — startup stub needed)"))


if __name__ == "__main__":
    main()
