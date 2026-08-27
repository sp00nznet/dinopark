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


def _pcrecomp_home():
    """Where the pcrecomp toolkit lives.

    PCRECOMP_HOME if it is set, otherwise a sibling checkout next to this one --
    which is how the recomp projects are laid out. Never a hardcoded absolute
    path: that is one person's drive letter, and it belongs in an environment
    variable rather than in a public repository.
    """
    env = os.environ.get("PCRECOMP_HOME")
    cands = [env] if env else []
    cands += [os.path.join(os.path.dirname(ROOT), "tools"),
              os.path.join(ROOT, "..", "pcrecomp")]
    for c in cands:
        if c and os.path.isdir(os.path.join(c, "tools", "disasm")):
            return os.path.abspath(c)
    raise SystemExit(
        "cannot find the pcrecomp toolkit.%s"
        "Set PCRECOMP_HOME to your checkout of%s"
        "  https://github.com/sp00nznet/pcrecomp%s"
        "(looked in: %s)" % (chr(10), chr(10), chr(10), ", ".join(str(c) for c in cands)))


TOOLS = _pcrecomp_home()
sys.path.insert(0, os.path.join(TOOLS, "tools", "disasm"))
sys.path.insert(0, os.path.join(TOOLS, "tools", "lift"))
from decode16 import Decoder, OpType  # noqa: E402
import lift16                       # noqa: E402  (to set _CODE_SEG per function)
from lift16 import Lifter           # noqa: E402

HDR = 0x4800
EXE = os.path.join(ROOT, "original", "DINOPARK.EXE")
OUT = os.path.join(ROOT, "src", "recomp", "gen")
CHUNK = 60
SPCHECK = os.environ.get("DINO_SPCHECK") == "1"   # wrap each fn to audit its SP delta

# Functions we implement natively in src/recomp/dino_impl.c instead of lifting.
# Some C-runtime routines are more trouble lifted than rewritten -- civ reached
# the same conclusion about its own CRT. They stay in the dispatch table and
# keep their fn_XXXXX names, so callers and indirect dispatch are unchanged.
# Compiled blits: `jmp bx` into an unrolled copy block.
#
# The sprite plotter sets bx = base - unit*count and jumps into a run of
# identical copy units, so entering part-way executes exactly `count` of them --
# a loop the compiler unrolled and the caller indexes into. C has no computed
# goto, and dispatching the address is meaningless because the targets are
# mid-block, so each one is replaced by the loop it stands for. The block's own
# trailing jump lands on the instruction after `jmp bx`, which is exactly where
# the lifted code continues, so the replacement simply falls through.
#
#   base -> (unit size, the body of one unit)
COMPILED_BLITS = {
    0xD9C: (4, 'movsb; add si, 3   -- copy forward'),
    0xF9F: (8, 'lodsb; es:[di] = al; dec di; add si, 3   -- copy backward'),
}

OVERRIDES = {
    0x02A35: 'int86x -- Borland assembles `int nn` on the stack and far-calls'
             ' it, which lands on no lifted function at all. The mouse probe'
             ' goes through here',
    0x02A04: 'int86, same construction',
    0x05BE8: 'sprintf -- the Borland printf core reads its format from the'
             ' wrong segment however it is driven (see src/test_sprintf.c)',
    0x00419: 'Miles driver load -- the .COM driver is a separate binary we do'
             ' not lift, and a zero handle makes the game quit',
    0x005E0: 'MIDPAK load -- same as 0x419 for the MIDI driver',
    0x0172F: 'Borland 32-bit divide -- multi-entry with near-to-far frame'
             ' juggling; lifted it returns without popping its arguments',
    0x02FA6: '_setargv -- returns through a computed jmp and leaves DS on the'
             ' PSP; the game takes no command-line arguments',
}
ENTRY_IMG = 0x0000                 # CS:IP = 0000:0000 -> image offset 0


def init_list_targets(data, first_det):
    """Entry points named only by Borland's _INIT_ table.

    The startup walks a linker-built list of 6-byte records --
    {flag, priority, far ptr} -- and calls each one. Nothing else in the
    program references them, so recursive descent never reaches them and they
    surface as runtime dispatch misses. That matters more than it sounds:
    the first record is the routine that marks Borland's stream table free,
    and without it every fopen in the game returns NULL.

    The startup names the table itself --
        mov dx, DGROUP      (image offset 0)
        mov si, _INIT_      mov di, _INITEND
    -- so read the bounds out of the code rather than hardcoding them.
    """
    from decode16 import Decoder, OpType
    try:
        insns = Decoder(data[HDR:first_det + HDR], base_offset=0).decode_all()
    except Exception:
        return set()

    dgroup = si = di = None
    for ins in insns:
        o1, o2 = ins.op1, ins.op2
        if ins.mnemonic != 'mov' or not o1 or o1.type != OpType.REG16:
            continue
        if not o2 or o2.type not in (OpType.IMM8, OpType.IMM16):
            continue
        reg = repr(o1)
        if reg == 'dx' and dgroup is None:
            dgroup = o2.disp            # the first `mov dx, imm` is DGROUP
        elif reg == 'si' and di is None:   # keep the first complete si/di pair
            si = o2.disp
        elif reg == 'di' and si is not None and di is None:
            di = o2.disp

    if dgroup is None or si is None or di is None or not (si < di):
        return set()
    if (di - si) % 6:                   # records are 6 bytes; anything else
        return set()                    # is not the table we are looking for

    out = set()
    base = dgroup * 16
    for a in range(base + si, base + di, 6):
        rec = data[a + HDR:a + HDR + 6]
        if len(rec) < 6:
            break
        off = int.from_bytes(rec[2:4], 'little')
        seg = int.from_bytes(rec[4:6], 'little')
        t = (seg * 16 + off) & 0xFFFFF
        if 0 < t < base:                # must land in code, not in data
            out.add(t)
    return out


def jump_tables(data, insns, cs, offs):
    """Arms of each `jmp word cs:[bx+table]` -- a Borland switch.

    The table is a run of near offsets sitting in the code segment, and
    nothing names the arms but this one instruction. The preceding
    `cmp bx, N` / `jbe` bounds it; failing that, take entries while they
    still land on an instruction boundary inside this function.

    Returns {jmp instruction address: [absolute arm addresses]}.
    """
    out = {}
    base = cs * 16
    for idx, ins in enumerate(insns):
        o = ins.op1
        if ins.mnemonic != 'jmp' or not o or o.type != OpType.MEM:
            continue
        if o.base != 'bx' or o.seg != 'cs':
            continue
        # Two shapes, both ending in `jmp word cs:[bx+disp]`.
        #
        # Dense -- bx is the case index, doubled, and disp is the table:
        #     cmp bx, N / jbe / jmp default / shl bx, 1 / jmp cs:[bx+table]
        #
        # Sparse -- bx walks a list of case VALUES and the arm offsets sit
        # immediately after them, so disp is exactly the value list's length:
        #     mov cx, N / mov bx, values / mov ax, cs:[bx] / cmp / je hit
        #     add bx, 2 / loop / jmp default
        #   hit: jmp cs:[bx + N*2]
        bound = bx_imm = cx_imm = None
        for j in insns[max(0, idx - 10):idx]:
            if not (j.op1 and j.op1.type == OpType.REG16 and j.op2
                    and j.op2.type in (OpType.IMM8, OpType.IMM16)):
                continue
            reg = repr(j.op1)
            if j.mnemonic == 'cmp':
                bound = j.op2.disp + 1
            elif j.mnemonic == 'mov' and reg == 'bx':
                bx_imm = j.op2.disp
            elif j.mnemonic == 'mov' and reg == 'cx':
                cx_imm = j.op2.disp
        if bx_imm is not None and cx_imm and o.disp == cx_imm * 2:
            bound, start = cx_imm, base + bx_imm + o.disp
        else:
            start = base + o.disp
        arms, tbl = [], start
        for k in range(bound if bound else 256):
            a = tbl + k * 2 + HDR
            if a + 2 > len(data):
                break
            t = base + int.from_bytes(data[a:a + 2], 'little')
            if t not in offs:
                break
            arms.append(t)
        if arms and (bound is None or len(arms) == bound):
            out[ins.address] = arms
    return out


def patch_compiled_blits(body):
    """Replace `jmp bx` dispatches that index into an unrolled copy block."""
    out, n = [], 0
    lines = body.split(chr(10))
    for idx, line in enumerate(lines):
        if '/* jmp bx */' not in line or 'recomp_dispatch' not in line:
            out.append(line)
            continue
        base = None
        for prev in lines[max(0, idx - 4):idx]:
            for b in COMPILED_BLITS:
                if f'add bx, 0x{b:X} */' in prev:
                    base = b
        if base is None:
            out.append(line)
            continue
        unit, what = COMPILED_BLITS[base]
        # `movsb` does not touch AL; `lodsb` does. Setting it either way
        # clobbers the plane mask the caller is rotating in AL between
        # passes, which silently turns 11/22/44/88 into nonsense.
        lodsb = (unit == 8)
        back = 'di--' if lodsb else 'di++'
        out.append(f'    {{ /* compiled blit: {what} */')
        out.append(f'      unsigned _n = (unsigned)((0x{base:X} - cpu->bx) / {unit});')
        out.append( '      while (_n--) {')
        out.append( '          uint8_t _b = mem_read8(cpu, cpu->ds, cpu->si);')
        out.append( '          mem_write8(cpu, cpu->es, cpu->di, _b);')
        out.append(f'          {"cpu->al = _b; " if lodsb else ""}cpu->{back}; cpu->si += 4;')
        out.append( '      } }')
        n += 1
    return chr(10).join(out), n


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

    def valid_boundary(t, strict=False):
        """t is a real entry point: either a decodable instruction boundary
        inside its containing detected function, or a Borland prologue.

        The boundary test alone is not enough. A function that embeds data --
        a sparse switch table of {value, offset} pairs, say -- desynchronises
        the linear decode that walks through it, so real entries past the table
        do not line up. `push bp; mov bp, sp` is what the compiler emits and is
        proof enough on its own.
        """
        # A Borland function entry, by its prologue. Two forms, and either is
        # proof on its own -- neither appears anywhere but at a function start:
        #
        #   push bp [push si] [push di] / mov bp, sp    -- has locals or args
        #   [pushes] / cmp [stack_base], sp / ja / call far <overflow>
        #                                               -- the stack probe, for
        #                                                  one with none
        #
        # Registers the function saves come first in both, so step over any
        # leading pushes. Missing the multi-push form left a family of
        # tail-jump thunks unlifted: fn_01736 sets cx and jumps to the shared
        # body at 01749, which opens `push bp / push si / push di / mov bp,
        # sp` -- the dispatch found nothing there, so the call did nothing.
        # Missing the stack-probe form cost three whole game screens.
        k = t + HDR
        saw_bp = False
        for _ in range(4):
            if k + 5 > len(data):
                break
            if data[k:k + 2] == b'\x39\x26' and data[k + 4:k + 5] == b'\x77':
                return True
            if saw_bp and data[k:k + 2] == b'\x8b\xec':
                return True
            if 0x50 <= data[k] <= 0x57:          # push reg
                if data[k] == 0x55:
                    saw_bp = True
                k += 1
            else:
                break
        # Past this point the evidence is only that the address lines up with
        # an instruction, which is true of the middle of every function. A
        # caller that got the address from a dispatch miss needs more than that:
        # a miss is any address the guest jumped to, and forcing the tails of
        # functions reached by indirect jumps unbalances their frames.
        if strict:
            return False
        i = _bi.bisect_right(detlist, t) - 1
        if i < 0:
            return False
        fs = detlist[i]
        fe = det_end[fs]
        if not (fs < t < fe):
            # Not inside any detected function. If it is in the gap *between*
            # two of them the analyzer simply never claimed that ground, and a
            # call target in unclaimed space is an entry point by definition --
            # accept it if it decodes. The memory manager is written in
            # assembly and has no Borland prologues, so its helpers all landed
            # here: the block-move that compaction slides blocks with was being
            # stubbed to nothing, which is why compacting could never make
            # progress and the allocator span forever on a fragmented heap.
            nxt = detlist[i + 1] if i + 1 < len(detlist) else len(data) - HDR
            if not (fe <= t < nxt):
                return False
            try:
                ins = Decoder(data[t + HDR:t + HDR + 32], base_offset=t).decode_all()
            except Exception:
                return False
            return bool(ins)
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
            if not line:
                continue
            t = int(line, 16)
            # 0xFFFF is the sentinel the lifter pushes as a near call's return
            # address, not somewhere the guest meant to go. It reaches the miss
            # list whenever something returns through the dispatcher.
            if t == 0xFFFF:
                continue
            if t not in starts and valid_boundary(t, strict=True):
                forced_targets.add(t)

    # Direct far calls resolve at lift time, so an unresolved one silently
    # becomes an empty stub rather than a runtime miss the convergence loop
    # could catch. Borland omits the push bp prologue for functions with no
    # locals, so the analyzer merges them into whatever precedes them and
    # thousands of calls land mid-function. Anything a far call points at is a
    # function entry by definition -- register the ones that are real
    # instruction boundaries.
    # Interrupt handlers the guest installed on a previous run. The address is
    # computed at run time, so nothing static finds it, and the analyzer wants a
    # Borland prologue that a handler does not have. Missing the INT 8 entry is
    # why the timer never ran and the game sat in its main loop with the clock
    # stopped.
    # The tail of the code, past the last function the analyzer found.
    #
    # DinoPark's memory manager ends in a run of hand-written assembly with no
    # Borland prologues, so nothing above 2F831 was ever detected -- and that is
    # where its storage backends live. The allocator registers three of them in
    # a table at DGROUP:7442 and reaches them with `call word ds:[si]`, so they
    # are never named by a direct call either. All three were missing from the
    # lift, which means the whole paging layer silently did nothing.
    #
    # Carve the region the way flow_end ends a single function: a terminator
    # that nothing decoded so far jumps past is the end of one function, and the
    # next instruction starts another. That is a fact about the code rather than
    # a guess about it, which is what "an address something jumped to" was not.
    n_tail = 0
    tail_from = max(det_end.values())
    # DGROUP is where the data starts, so the code cannot run past it. The
    # startup's first instruction is `mov dx, DGROUP` -- a relocated
    # immediate at image offset 1.
    dgroup_img = (data[HDR + 1] | (data[HDR + 2] << 8)) * 16
    tail_to = dgroup_img if dgroup_img > tail_from else tail_from
    tail_to = min(len(data) - HDR, tail_to)
    if tail_to > tail_from:
        try:
            tins = Decoder(data[tail_from + HDR:tail_to + HDR],
                           base_offset=tail_from).decode_all()
        except Exception:
            tins = []
        reach = tail_from
        nxt = tail_from
        for ins in tins:
            o = ins.op1
            if (ins.mnemonic.startswith(('j', 'loop')) and o
                    and o.type in (OpType.REL8, OpType.REL16)):
                reach = max(reach, tail_from + (o.disp & 0xFFFF))
            term = ins.mnemonic in ('ret', 'retf', 'ret far', 'iret', 'jmp')
            if nxt and nxt not in starts and nxt not in forced_targets:
                forced_targets.add(nxt)
                n_tail += 1
            nxt = 0
            if term and ins.offset >= reach:
                nxt = ins.offset + ins.length
        if n_tail:
            print(f"  functions carved from the unclaimed tail: {n_tail}")

    n_vec = 0
    vec_path = os.path.join(ROOT, "work", "dino_vectors.txt")
    if os.path.exists(vec_path):
        for line in open(vec_path):
            line = line.strip()
            if not line:
                continue
            t = int(line, 16)
            if t not in starts and t not in forced_targets and valid_boundary(t):
                forced_targets.add(t)
                n_vec += 1
    if n_vec:
        print(f"  installed interrupt handlers promoted to functions: {n_vec}")

    n_init = 0
    for t in sorted(init_list_targets(data, first_det)):
        if t not in starts and t not in forced_targets and valid_boundary(t):
            forced_targets.add(t)
            n_init += 1
    if n_init:
        print(f"  _INIT_ table entries promoted to functions: {n_init}")

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
                if 0 < t < first_det:
                    call_tgts.add(t)          # inside the startup region
                elif t not in starts and t not in forced_targets and valid_boundary(t):
                    forced_targets.add(t)
                    n_farfix += 1
            if ins.mnemonic == 'call' and o and o.type == OpType.REL16:
                # Same hole on the near side: an unresolved near call is
                # named res_XXXXXX and stubbed out, so it silently does
                # nothing. Wrap the target the way the lifter will.
                base = cs_of(s) * 16
                t = base + ((s - base + o.disp) & 0xFFFF)
                # A target inside the Borland startup has no containing detected
                # function -- the analyzer never found one down there -- so
                # valid_boundary can never accept it and it was becoming a stub.
                # The startup's own subroutines are already carved out this way.
                if 0 < t < first_det:
                    call_tgts.add(t)
                elif t not in starts and t not in forced_targets and valid_boundary(t):
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
    def flow_end(t, cap):
        """Where the function at t ends, by following its control flow.

        The next start is the usual answer, but the memory manager's assembly
        helpers sit above every function the analyzer found, so the last of them
        has no next start and the old fallback (the highest detected end) came
        out BELOW the target -- an empty range, and the block-move compaction
        depends on lifted to nothing.

        Decode forward and stop after the first ret/retf that nothing decoded so
        far jumps past. Trailing bytes are data as often as code, so cap it.
        """
        try:
            insns = Decoder(data[t + HDR:cap + HDR], base_offset=t).decode_all()
        except Exception:
            return None
        reach = t
        for ins in insns:
            o = ins.op1
            if (ins.mnemonic.startswith(('j', 'loop')) and o
                    and o.type in (OpType.REL8, OpType.REL16)):
                reach = max(reach, t + (o.disp & 0xFFFF))
            if ins.mnemonic in ('ret', 'ret far', 'retf') and ins.offset >= reach:
                return ins.offset + ins.length
        return None

    # forced miss-targets: span to the next detected/forced start
    allpts = sorted(starts | forced_targets)
    for s in sorted(forced_targets):
        i = allpts.index(s)
        if i + 1 < len(allpts):
            end = allpts[i + 1]
        else:
            end = flow_end(s, min(len(data) - HDR, s + 0x400)) or (s + 0x40)
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
    wrappers = []
    names_overridden = set()
    n_arms = 0
    n_blits = 0
    for name, io, end in funcs:
        if io in OVERRIDES:
            segs_used.add(cs_of(io))
            print(f"  native override: {name}  ({OVERRIDES[io]})")
            names_overridden.add(name)
            continue
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
            # Switch arms belong to THIS function, not to themselves: they are
            # its loop body, and dispatching one as a standalone function
            # returns from the enclosing C function with its epilogue never
            # run -- SP is left un-restored and the caller's BP takes a
            # call-frame sentinel.
            lifter.jump_tables = jump_tables(
                data, insns, cs, {x.offset for x in insns})
            n_arms += sum(len(v) for v in lifter.jump_tables.values())
            body = lifter.lift_function(name, insns, io, is_far=True)
            body, n_blit = patch_compiled_blits(body)
            n_blits += n_blit
            # keep the live cpu->cs honest too -- traces and the runtime read it.
            body = body.replace("(CPU *cpu)" + chr(10) + "{",
                                f"(CPU *cpu)" + chr(10) + "{" + chr(10) + f"    cpu->cs = SEG_{cs:04X};", 1)
            if SPCHECK:
                # Rename the body and call it through a wrapper that records
                # SP either side. A function that returns must net-POP its
                # return frame, so a negative delta is a function eating stack.
                body = body.replace(f"void {name}(", f"void {name}__body(", 1)
                wrappers.append(f"void {name}(CPU *cpu) {{"
                                f" uint16_t _s = cpu->sp, _b = cpu->bp;"
                                f" recomp_enter(cpu, 0x{io:05X});"
                                f" {name}__body(cpu);"
                                f" recomp_leave(cpu, 0x{io:05X}, _s, cpu->sp, _b, cpu->bp); }}")
            bodies.append(body)
            all_calls |= lifter.func_calls
            lifted += 1
        except Exception as e:
            bodies.append(f"/* {name}: lift failed: {e} */\nvoid {name}(CPU*cpu){{(void)cpu;}}")

    names = [n for n, _, _ in funcs]
    lifted_names = [n for n in names if n not in names_overridden]
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

    if SPCHECK:
        with open(os.path.join(OUT, "recomp_wrap.c"), "w") as f:
            f.write(chr(35) + 'include "recomp_all.h"\n\n')
            f.write(chr(10).join(wrappers) + chr(10))

    with open(os.path.join(OUT, "recomp_all.h"), "w") as f:
        f.write("#ifndef RECOMP_ALL_H\n#define RECOMP_ALL_H\n")
        f.write('#include "../cpu.h"\n#include "../runtime16.h"\n\n')
        for sg in sorted(segs_used):
            f.write(f"#define SEG_{sg:04X} 0x{sg:04X}\n")
        f.write("\n")
        if SPCHECK:
            f.write("void recomp_enter(CPU *cpu, unsigned addr);\n")
            f.write("void recomp_leave(CPU *cpu, unsigned addr, unsigned sp0, unsigned sp1, unsigned bp0, unsigned bp1);\n")
            f.write("extern int g_recomp_quiet;\n")
            f.write("void call_hist_dump(const char *path);\n")
            for n in lifted_names:
                f.write(f"void {n}__body(CPU *cpu);\n")
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
    /* Two different things share this file. It is the lifter's work list, so
     * it merges with what is already there and grows monotonically -- an
     * address stays forced once it has been forced, and a shallow run must
     * not shrink the set. But that makes the total useless as a health
     * signal: an address lifted three rounds ago is still listed and reads as
     * an outstanding failure. Report what THIS run failed to dispatch too. */
    int seen_here = g_nmiss;
    FILE *r = fopen(path, "r");
    if (r) { unsigned a; while (fscanf(r, "%x", &a) == 1) note_miss(a); fclose(r); }
    FILE *f = fopen(path, "w"); if (!f) return;
    for (int i = 0; i < g_nmiss; i++) fprintf(f, "%05X\\n", g_miss[i]);
    fclose(f);
    fprintf(stderr, "[disp] %d missed this run, %d on the list -> %s\\n",
            seen_here, g_nmiss, path);
}

/* total dispatch budget: deep init currently runs into garbage function
   pointers (uninitialized data) and would spin forever — cap it so the boot
   always terminates while that data-setup work is in progress. */
static long g_disp_budget = -1;
/* indirect call/jmp by (seg,off): linear = seg*16+off (image space) */
int recomp_dispatch(CPU *cpu, unsigned seg, unsigned off) {
    unsigned addr = ((seg << 4) + off) & 0xFFFFF;
    if (g_disp_trace < 0) g_disp_trace = getenv("DINO_TRACE") ? 1 : 0;
    /* No dispatch budget by default. It was a bring-up guard -- stop the CPU
     * rather than spin forever on a bad function pointer -- from when a run was
     * a few seconds of boot. A game being played dispatches indefinitely and
     * would simply stop partway through. The depth cap below still catches
     * runaway recursion, and the watchdog catches a genuine hang.
     * DINO_BUDGET sets one again when bringing up a new path. */
    if (g_disp_budget < 0) { const char *e = getenv("DINO_BUDGET"); g_disp_budget = e ? atol(e) : 0; }
    if (addr == 0) return 0;                            /* uninitialized fnptr */
    if (g_disp_budget && --g_disp_budget <= 0) { cpu->halted = 1; return 0; }
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

/* Same for an indirect FAR call: the site pushed CS and a return word, so a
 * miss has to drop four bytes rather than two. Without it every call through
 * an unresolved far pointer walks SP down by 4 and the caller's `pop bp`
 * comes back with somebody else's data. */
void dispatch_far(CPU *cpu, unsigned seg, unsigned off) {
    if (!recomp_dispatch(cpu, seg, off)) cpu->sp += 4;
}
""")
        if SPCHECK:
            f.write("""
/* SP audit. A lifted function is entered with its return frame already on the
 * guest stack and must leave having popped it, so the net delta is +2 (near
 * ret), +4 (retf), or those plus the bytes a `ret N` pops for its caller. A
 * NEGATIVE delta means the function consumed stack and never gave it back,
 * which is the thing that walks SP out of its 8 KB and trips Borland's own
 * stack check. Report the first few, worst first, with a running low-water. */
/* A live call stack, so a boot that stops making calls can still say where it
 * stopped. recomp_sp_check pops it on the way out, so whatever is left when
 * the watchdog fires is the chain that never returned. */
#define MAXSTK 512
unsigned g_stk[MAXSTK]; int g_stk_depth;
/* Which functions ran, and how many times.
 *
 * "The screen never changes" says nothing about why. A call histogram does:
 * it separates a handler that ran and decided nothing from one that was never
 * reached at all. Open-addressed on the function's own address; the table is
 * sized well past the ~900 functions a lift produces, so it never fills. */
#define CALLHIST 4096
static unsigned g_ch_addr[CALLHIST];
static unsigned long g_ch_n[CALLHIST];

static void call_note(unsigned addr) {
    unsigned i = (addr * 2654435761u) % CALLHIST;
    for (unsigned k = 0; k < CALLHIST; k++) {
        unsigned j = (i + k) % CALLHIST;
        if (g_ch_n[j] && g_ch_addr[j] != addr) continue;
        g_ch_addr[j] = addr; g_ch_n[j]++; return;
    }
}

void call_hist_dump(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (unsigned j = 0; j < CALLHIST; j++)
        if (g_ch_n[j]) fprintf(f, "%05X %lu\\n", g_ch_addr[j], g_ch_n[j]);
    fclose(f);
}

static void recomp_push(unsigned addr);
void recomp_sp_check(unsigned addr, unsigned sp0, unsigned sp1, unsigned bp0, unsigned bp1);

/* DINO_FNTRACE=<offset>[,<offset>...]: what a function was handed and gave
 * back. A validator that returns the wrong answer looks exactly like one that
 * was never called, until you can see the value. */
#define FNT_MAX 8
static unsigned g_fnt[FNT_MAX];
static int g_fnt_n = -1;

static int fnt_watched(unsigned addr) {
    if (g_fnt_n < 0) {
        g_fnt_n = 0;
        const char *e = getenv("DINO_FNTRACE");
        while (e && *e && g_fnt_n < FNT_MAX) {
            g_fnt[g_fnt_n++] = (unsigned)strtoul(e, NULL, 16);
            const char *c = strchr(e, ',');
            if (!c) break;
            e = c + 1;
        }
    }
    for (int i = 0; i < g_fnt_n; i++) if (g_fnt[i] == addr) return 1;
    return 0;
}

/* DINO_FNDUMP=<offset>:<len>: bytes at DS:<offset>, printed either side of a
 * watched function. Registers say what a routine was handed; a structure it
 * works through -- a stream, a block header -- is the part you cannot see
 * without this. */
static void fnt_dump(CPU *cpu, const char *tag) {
    /* <offset>:<len> reads through DS; <seg>:<offset>:<len> names the segment,
     * for a buffer that does not live in the data segment. */
    static int off = -1, len, seg = -1;
    if (off < 0) {
        off = 0;
        const char *e = getenv("DINO_FNDUMP");
        if (e) {
            const char *c1 = strchr(e, ':');
            const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
            if (c2) {
                seg = (int)strtoul(e, NULL, 16);
                off = (int)strtoul(c1 + 1, NULL, 16);
                len = (int)strtoul(c2 + 1, NULL, 0);
            } else {
                off = (int)strtoul(e, NULL, 16);
                len = c1 ? (int)strtoul(c1 + 1, NULL, 0) : 16;
            }
            if (len > 64) len = 64;
            if (len <= 0) len = 16;
        }
    }
    if (!off && seg < 0) return;
    uint16_t sg = seg < 0 ? cpu->ds : (uint16_t)seg;
    fprintf(stderr, "[fn]   %s %04X:%04X:", tag, sg, (unsigned)off);
    for (int i = 0; i < len; i++)
        fprintf(stderr, " %02X", mem_read8(cpu, sg, (uint16_t)(off + i)));
    fprintf(stderr, "\\n");
}

void recomp_enter(CPU *cpu, unsigned addr) {
    if (fnt_watched(addr)) {
        fprintf(stderr, "[fn] %05X in   ax=%04X bx=%04X cx=%04X dx=%04X "
                        "ds=%04X es=%04X ss=%04X sp=%04X bp=%04X",
                addr, cpu->ax, cpu->bx, cpu->cx, cpu->dx,
                cpu->ds, cpu->es, cpu->ss, cpu->sp, cpu->bp);
        /* And the top of the stack, which is where the arguments are: the
         * return address first, then the words the caller pushed. Guessing an
         * argument's address from SP and a disassembly works right up until the
         * call arrives at a different stack depth. */
        fprintf(stderr, "  stack:");
        for (int i = 0; i < 7; i++)
            fprintf(stderr, " %04X",
                    mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + i * 2)));
        fprintf(stderr, "\\n");
        fnt_dump(cpu, "in ");
    }
    recomp_push(addr);
}

void recomp_leave(CPU *cpu, unsigned addr, unsigned sp0, unsigned sp1,
                  unsigned bp0, unsigned bp1) {
    if (fnt_watched(addr))
        { fprintf(stderr, "[fn] %05X out  ax=%04X dx=%04X\\n", addr, cpu->ax, cpu->dx);
          fnt_dump(cpu, "out"); }
    recomp_sp_check(addr, sp0, sp1, bp0, bp1);
}

static void recomp_push(unsigned addr) {
    call_note(addr);
    if (g_stk_depth < MAXSTK) g_stk[g_stk_depth] = addr;
    g_stk_depth++;
}

/* Set while a guest interrupt handler runs. Its SP delta is not the caller's
 * business: the runtime pushes the interrupt frame, `iret` lifts to a bare
 * return that leaves SP alone, and the whole register file is restored
 * afterwards. DinoPark's INT 8 handler ends `pushf / call far [old vector]`
 * chaining to a previous handler that does not exist, which the dispatcher
 * rightly declines -- a reliable -2 every tick, enough to use up the audit's
 * report cap and hide anything real. */
int g_recomp_quiet;

void recomp_sp_check(unsigned addr, unsigned sp0, unsigned sp1,
                     unsigned bp0, unsigned bp1) {
    static int fired = 0;
    if (g_stk_depth > 0) g_stk_depth--;
    if (g_recomp_quiet) return;
    int delta = (int)(short)(unsigned short)(sp1 - sp0);
    int bp_bad = (bp0 != bp1);
    /* A function pops its own return frame: +2 for a near ret, +4 for retf,
     * plus whatever a `ret N` clears for its caller. Anything under +2 means
     * it consumed stack that was not its own. */
    if (!bp_bad && delta >= 2 && delta <= 256) return;
    if (fired++ >= 25) return;
    fprintf(stderr, "[sp] fn_%05X  SP %04X->%04X (%+d)  BP %04X->%04X%s\\n",
            addr, sp0, sp1, delta, bp0, bp1, bp_bad ? "  <-- BP CLOBBERED" : "");
}
""")

        # Stubs for call targets we could not resolve. They cannot do the work,
        # but they must still pop the return frame the call site pushed, or
        # every call through one loses 2 or 4 bytes of stack and the caller's
        # `pop bp` then reads the wrong slot.
        for s in stubs:
            # Pop what the call site pushed: a far call put CS and a return
            # word on the stack, a near one just the word. Deciding instead by
            # decoding the target and matching its ret/retf measured WORSE --
            # it over-pops the `push cs; call near` sites and Borland's stack
            # check starts firing again. Keep the simple rule.
            pop = 4 if s.startswith("far_") else 2
            f.write(f"void {s}(CPU *cpu) {{ cpu->sp += {pop}; }}   /* unresolved: keeps the stack balanced */\n")

    print(f"Lifted {lifted}/{len(funcs)} functions -> {nchunks} chunks in {OUT}")
    print(f"  code segments: {len(segs_used)}")
    print(f"  wrap-corrected near call/jmp targets: {n_wrapped}")
    print(f"  switch arms lifted inline: {n_arms}")
    if n_blits:
        print(f"  compiled blits turned back into loops: {n_blits}")
    print(f"  dispatch entries: {len(names)}  unresolved-call stubs: {len(stubs)}")
    print(f"  entry image offset: 0x{ENTRY_IMG:05X}"
          + ("  (a detected function)" if ENTRY_IMG in known else "  (NOT detected — startup stub needed)"))


if __name__ == "__main__":
    main()
