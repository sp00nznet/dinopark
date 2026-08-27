# Phase 2 — Naming & Asset Formats

Two fronts: (1) attach meaning to the 693 functions, and (2) crack the asset
containers so the game's data is readable. Re-run with
`python tools/strings_xref.py` and `python tools/unc_parse.py`.

## 1. Function naming via string xref (`tools/strings_xref.py`)

In large model a string is reached by pushing its DGROUP-relative offset as a
16-bit immediate, so we recover the DGROUP file base by voting (base = string_off
− code_immediate, paragraph-aligned, maximizing distinct strings explained) and
attribute each xref to its containing function.

**Result:** DGROUP base ≈ `0x32FE0`; ~41 functions named from the *near*-data
string pool. This only covers DGROUP strings — DinoPark is **large model with
heavy FAR data**, so most strings live in their own far segments with per-segment
offsets that immediate-matching can't follow. Completing this needs segment-aware
analysis (Ghidra/IDA far-pointer tracking) — that's the cross-seed step.

Even partial, it pinned real anchors:

| Function | Evidence | Likely role |
|----------|----------|-------------|
| `0x2D5F1` / `0x2D7F0` | `Albertosaurus`, `Ankylosaurus`, `Coelophysis`, `Tyrannosaurus`, `Triceratops`, `pens.pic` | **dinosaur species table / info screen** |
| `0x07638` | `Hire the employee`, `gloved person in back` | **employee hiring** |
| `0x2726D` / `0x32493` | `Gift concession revenue`, `Staff summary` | **finance / staff report** |
| `0x33C98` (24 callers) | `The date is`, `%d:%02d` | **date/clock formatting** |
| `0x0B7B3` (14 callers) | `Creating swap file` | Borland overlay/swap runtime |

## 2. Asset containers (`tools/unc_parse.py`)

Cracked the `UNC` container family across all 82 graphics files — see
[`FORMATS.md`](FORMATS.md).

- ✅ **`UNC2` actor format fully solved** — 55/55 validate (sprite_count + trailing
  u32 offset table; sprite *i* = `[tbl[i], tbl[i+1])`).
- ✅ **`UNCP` pictures** — family + two sub-variants identified (atlas offset-table
  vs fullscreen); fullscreen validated, atlas table located.
- ◻️ **`UNCS`** (2 files) — variant with inline dimension table, layout TBD.

## Phase 3 targets

1. **LZSP codec** — disassemble `ReadActLZSP` and decode a real sprite to a PNG
   (the verification milestone). A generic LZ/RLE bitmap decoder may be upstreamable.
2. **Actor script VM** — reverse the bytecode opcode set.
3. **Ghidra/IDA cross-seed** — far-pointer-aware naming to finish the function
   map, then begin lifting the named game-logic functions.
