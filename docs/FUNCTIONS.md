# Phase 1 — Decode & Function Map

> Output of `python tools/analyze_dinopark.py` (drives the pcrecomp 16-bit
> decoder + the new `disasm/largemodel16` call-graph completion). Re-runnable;
> raw artifacts land in `work/` (gitignored).

## Image layout (DINOPARK.EXE)

```
  0x00000 ┌───────────────────────────┐
          │ MZ header + reloc table   │  18,432 B  (1152 paras, 4139 relocs)
  0x04800 ├───────────────────────────┤  ← load image / entry (CS:IP 0000:0000)
          │ CODE                      │  190 KB
          │  Borland C runtime + game │  693 functions, 75,175 instructions
  0x34031 ├───────────────────────────┤  ← code/data boundary (auto-detected)
          │ DGROUP (initialized data) │  54 KB
          │  strings, tables, "Borland"│  883 strings
  0x41840 ├───────────────────────────┤  ← SS:SP 3D04:0080 (stack at top)
  0x418C0 └───────────────────────────┘
```

## Function map

| Metric | Value |
|--------|-------|
| Functions (real) | **693** |
| Instructions | 75,175 |
| Strings | 883 |
| Roots (uncalled — startup / indirect-only) | 258 |
| Leaves (call nothing local — fuzzable units) | 138 |
| **Far calls resolved** | **3,609 / 3,611 (99.9%)** |

Memory model is **large** — every function uses `RETF` and the program is wired
together almost entirely by FAR calls. The base analyzer only tracks near calls,
so completing the graph meant resolving far targets
(`file_off = hdr + seg*16 + off`). That logic was generalized and pushed upstream
to [`pcrecomp/tools/disasm/largemodel16.py`](https://github.com/sp00nznet/pcrecomp).

### Hottest functions (compiler/runtime helpers float to the top)

| Addr | Callers | Callees | Size | Likely role |
|------|--------:|--------:|-----:|-------------|
| `0x0792B` | **411** | 1 | 230 B | Borland runtime helper (far-ptr / long math / stack check) |
| `0x0633C` | 58 | 1 | 55 B | runtime leaf |
| `0x102CC` | 52 | 1 | 93 B | runtime leaf |
| `0x0A3E8` | 50 | 1 | 34 B | runtime leaf |
| `0x1DDED` | 44 | 5 | 226 B | shared game helper |
| `0x13833` | 32 | 18 | 1205 B | high-fan game logic |
| `0x12085` | 28 | 14 | 876 B | high-fan game logic |

A function with hundreds of callers and ~zero callees is the signature of a
compiler-inserted helper; those get identified and stubbed first so the game
logic above them reads cleanly.

## What the binary tells us about itself (string-driven recon)

- 🎬 **Actor script VM.** `.ACT` files carry a sprite count, a *brush* table, and
  a **script count / script size** — and the code has an `Unknown opcode error.
  opcode: …` handler. DinoPark animates via a tiny **bytecode interpreter**, not
  hardcoded frames. Reversing that opcode set is a Phase 2 centerpiece.
- 🗜️ **LZ-compressed sprites.** `ReadActLZSP, sprite = %d` → sprites are stored
  LZ-packed inside `.ACT`; `ReadAct` / `ReadPic` validate a 4-char magic
  (`Unrecognized header '%4.4s'`). See [`FORMATS.md`](FORMATS.md).
- 🎵 **Miles audio confirmed.** `Unable to load xmi file '%s'`, `register music
  data`, `Error initializing music driver` + the `MIDPAK`/`.ADV` drivers.
- 🦕 **Game-logic anchors** (menu/action strings to seed function naming):
  `Buy Dinos` · `Buy Dino Chow` · `Attend Auction` · `Go to the park` ·
  `Show dinosaur information` · `Buy item`.
- 🛠️ **Borland runtime markers:** `Divide error`, `Abnormal program termination`,
  `floating point formats not linked`, `Error creating swap file` (overlay/swap).

## Next (Phase 2)

1. **Name the runtime layer** — classify the ~hot leaves as Borland C library
   (memcpy/strcpy/far-ptr/long-math/`printf`) so they can be replaced by host
   intrinsics instead of lifted.
2. **String→function xref** — attribute DGROUP string offsets to the functions
   that load them (DS-relative imm16 scan) to auto-name game logic from the
   anchors above.
3. **Reverse the `.ACT` script VM** opcode set + the LZSP sprite codec.
4. Begin **lifting** the high-fan game-logic functions against the DOS/VGA/Miles
   runtime.
