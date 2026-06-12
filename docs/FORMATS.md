# Asset Formats

Reverse-engineered from `DINOPARK.EXE` loader strings (Phase 1) and **confirmed
against the real asset bytes** (Phase 2, `tools/unc_parse.py`). Bring your own
game files into `original/`.

## The `UNC` container family

Every graphics file starts with a 4-char `UNC?` magic, validated by the engine's
`ReadAct` / `ReadPic` (`Unrecognized header '%4.4s'`). Three tags seen across 82
files:

| Magic | Ext | Files | What |
|-------|-----|------:|------|
| `UNC2` | `.ACT` | 55 | Actor sprite set (the workhorse — every dino, screen, UI) |
| `UNCS` | `.ACT` | 2 | Actor variant (`BUS`, `PEOPLE`) — inline dimension table |
| `UNCP` | `.PIC` | 25 | Picture (full-screen background or multi-image atlas) |

## `UNC2` — actor sprite set ✅ **solved (55/55 validated)**

```
 +0   char[4]  "UNC2"
 +4   u16      sprite_count
 +6   u32      table_off          ; -> trailing per-sprite offset table
 +10  u16,u16  flags (usually 0)
 +14  …        sprite data (LZSP-compressed), sprite 0 first
 …
 [table_off]   u32[sprite_count]  ; file offset of each sprite; sprite i spans
                                  ; [tbl[i], tbl[i+1]) (last runs to table_off)
```

Validated on all 55 files: the trailing table holds **exactly** `sprite_count`
u32 entries (file tail = `sprite_count × 4`), monotonic, first entry = 14.
Examples: `ALBERT` 11 sprites, `BTNS` 77, `DINOMART` 91, `AUCTION` 109,
`PARK` 138.

- **Sprites** are **LZ-compressed** (`ReadActLZSP, sprite = %d`). The LZSP codec
  + per-sprite bitmap header (likely `width,height` then RLE/LZ pixels) is the
  next layer to reverse — see Phase 3.
- Some actors also carry **scripts** (`script count`/`script size`) driving the
  actor **bytecode VM** (`Unknown opcode error. opcode: …`); located via the
  `+10` flags region. Opcode set TBD.

## `UNCP` — pictures (two sub-variants)

```
 +0   char[4]  "UNCP"
 +4   u16      count / type
 +6   u16      pad (0)
 +8   u32[…]   offset table
```

- **atlas** subtype — `+8` holds a monotonic, in-bounds `u32` offset table of
  sub-images (e.g. `PENS` 272 poses, `OFFICE` 48, `ABOUT` 88, `CREDITS` 36).
- **fullscreen** subtype — a single near-EOF pointer at `+8` then a full-screen
  compressed image; `+4` is constant `8` (all background screens: `AUCTION`,
  `MALL`, `MECC`, `BLUEPRNT`, `WINSHOP`, …).

The `+4` field doubles as count (atlas) vs type flag (fullscreen); disambiguating
it and decoding the pixel data is Phase 3. `FONT.PIC` carries `width=128,
height=64` at `+14` — a fixed-grid glyph sheet, its own special case.

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
