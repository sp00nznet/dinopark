# Asset Formats (WIP)

Reverse-engineered from `DINOPARK.EXE` loader strings (Phase 1) and confirmed
against the asset bytes in Phase 2. Bring your own game files into `original/`.

## `.ACT` — actors / animations

The workhorse format (57 files; one per dinosaur and screen). Loaded by
`ReadAct`. Structure inferred from loader error strings:

```
header   : 4-char magic, validated ("ReadAct:Unrecognized header '%4.4s'")
sprites  : sprite count, then sprites (some LZ-packed -> "ReadActLZSP, sprite=%d")
brushes  : "brush start" offset/table
scripts  : script count, script size  -> bytecode for the actor VM
```

- **Sprites** are stored **LZ-compressed** (`LZSP`); needs a decompressor.
- **Scripts** drive a small **bytecode VM** (`Unknown opcode error. opcode: …`).
  Reversing the opcode set unlocks all in-game animation. Top Phase 2 target.

## `.PIC` — full-screen images

25 files (title, auction, bank, diner, blueprints, `font.pic`, `pens.pic`…).
Loaded by `ReadPic`, same 4-char magic discipline
(`ReadPic:Unrecognized header '%4.4s'`). Likely planar/packed VGA (320×200 or
640×480). Decoder TBD.

## `.ABT` — sprites / sound bits

44 small files (`honk.abt`, `birds.abt`, `dino2a.abt`, `clapper`, `comet`…).
Mix of small sprites and SFX. Format TBD.

## `.XMI` — music (Miles XMIDI)

7 files (`DINOPARK`, `DINOCITY`, `AUCTION`, `EXTINCT`, `DINOFKEY`, `DINO5A`).
Standard **Miles Sound System** XMIDI. Loaded via the music driver
(`Unable to load xmi file '%s'`). A Miles `.XMI → .MID` converter is a strong
**upstream** candidate for [pcrecomp](https://github.com/sp00nznet/pcrecomp).

## `.COM` / `.ADV` — Miles AIL sound drivers

27 `.COM` + the `.ADV`/`.AD` descriptors are Miles digital + MIDI drivers
(AdLib, Sound Blaster / Pro, Pro Audio Spectrum, Windows Sound System, …),
selected by `CONFIG.EXE`. The recomp answers these through the host audio
backend rather than loading the DOS drivers.

## `dino.cfg` / `product.pf`

`dino.cfg` (20 B) — runtime config written by `CONFIG.EXE` (sound card sel.).
`product.pf` — product/registration data. Small; trivial to model.
