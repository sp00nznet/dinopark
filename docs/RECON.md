# Phase 0 — Reconnaissance

> What are we looking at, before we touch anything.

## The target

**DinoPark Tycoon** — developed by **Manley & Associates**, published by **MECC**,
1993. MS-DOS edutainment business sim: you run a dinosaur theme park. Buy dinosaurs
at auction, feed them, fence them in, set ticket prices, take bank loans, and try to
turn a profit. Teaches economics, arithmetic, and paleontology by stealth. (Also
released for Macintosh, 3DO (1994), and Windows (1997); this teardown targets the
**DOS** release.)

This is the same 16-bit DOS lineage as our `civ` and `bolo` recomps.

## Executable header (DINOPARK.EXE)

```
MZ  bytes_last_page=0x00C0  pages=0x020D (525)  -> image = 268,480 B = whole file
hdr_paragraphs=0x0480 (1152)  -> header = 18,432 B
relocs=4139  reloc_table_off=0x003E  overlay#=0
CS:IP=0000:0000   SS:SP=3D04:0080
min_alloc=0  max_alloc=0xFFFF
```

### Key findings

- ✅ **Not packed.** No PKLITE / LZEXE / EXEPACK / DIET / aPACK / WWPACK signature.
  The MZ image is the full 262 KB and the entry/header look like a normal Borland
  link output. **No unpacking step** — we go straight to decode. (Contrast Bolo,
  which was PKLITE-`large+extra` and needed a custom static decompressor.)
- 🛠️ **Borland 16-bit C toolchain.** `"Borland"` runtime fingerprint at file
  offset `0x34A04`. Large memory model (4,139 relocations, segmented). Likely
  Turbo C / Borland C++ era.
- 🎵 **Miles Sound System (AIL).** `"MIDPAK"` at `0x34F0E`. The numerous `.COM`
  files are Miles digital + MIDI sound-card drivers; music ships as `.XMI` (XMIDI).
  See the audio inventory below.

## File inventory (192 files)

| Group | Ext | Count | What it is |
|-------|-----|-------|------------|
| **Code** | `DINOPARK.EXE` | 1 | 262 KB — all game logic. Unpacked Borland-C 16-bit DOS MZ. |
| Setup | `CONFIG.EXE`, `SETUP.BAT` | 2 | Sound-card / install configuration. |
| **Graphics** | `.PIC` | 25 | Full-screen images (title, auction, bank, diner, credits, blueprints…). |
| **Actors/anim** | `.ACT` | 57 | Per-dinosaur and per-screen animations/actors (`ALBERT`, `ALLOS`, `ANKY`, `APATO`, `DEINON`, `COEL`, `BAPA`, `BTNS`, `BUS`, `DINOMART`, …). |
| Sprites/SFX | `.ABT` | 44 | Small sprite / sound bits (`BIRDS`, `BELL`, `CLAPPER`, `COMET`, `CRACKLE`, …). |
| **Audio drivers** | `.COM` | 27 | Miles AIL drivers: `ADLIB`, `SBLASTER`, `SBPRO`, `PAS16`, `WSS`, `MIDPAK`, `SNDSYS`, `STFX`, `SOUNDRV`, etc. |
| Audio config | `.ADV`, `.AD` | 10 | Miles driver descriptors (`ADLIB.ADV`, `ADLIBG.ADV`, …). |
| **Music** | `.XMI` | 7 | Miles XMIDI music (`AUCTION`, `DINOCITY`, `DINO5A`, `DINOFKEY`, …). |
| Data | `.DAT`, `.CFG`, numbered `.000`–`.007` | ~10 | Game data / config / packed asset blobs. |
| Misc | `.PF`, `.ICO`, `.DMO`, `.ba1` | 4 | Font(?), icon, demo, archive metadata. |

## Tooling on hand

- **Ghidra** and **IDA**, both driven headlessly, for cross-seeding function
  bounds and names.
- **m2c**, for a second opinion on decompiled output.
- The **[pcrecomp](https://github.com/sp00nznet/pcrecomp) toolkit** — the 16-bit
  disassembler and lifter, the NE/MZ parsers and the runtime. `tools/lift_full.py`
  finds it through `PCRECOMP_HOME`, or as a sibling checkout.

## Next steps (Phase 1)

1. **Decode** `DINOPARK.EXE` with the 16-bit decoder (`pcrecomp/tools/disasm`),
   applying the 4,139-entry relocation table to resolve segment fixups.
2. **Map** functions, segments, the call graph, and string tables; identify the
   Borland C runtime/startup vs. game code (classifier).
3. **Cross-seed** with Ghidra and IDA (both available) and reconcile function bounds.
4. **Reverse the asset formats** in parallel — `.PIC`/`.ACT`/`.ABT` graphics and
   `.XMI` (Miles XMIDI) audio → `docs/FORMATS.md`.
5. Begin **lifting** the highest-fan-in game-logic functions to C against the
   DOS/VGA/Miles runtime.

## Upstream-worthy tools to watch for

Anything reusable we build (a Miles/AIL `.XMI` decoder, a Borland-C startup
classifier, a `.PIC`/`.ACT` planar-VGA image extractor) gets pushed up into
[**pcrecomp**](https://github.com/sp00nznet/pcrecomp), not kept local — same rule
as every other project in the family.
