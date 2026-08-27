<div align="center">

# 🦕 DinoPark — a static recomp of *DinoPark Tycoon*

```
   ____  _             ____             _      _____                            
  |  _ \(_)_ __   ___ |  _ \ __ _ _ __| | __ |_   _|   _  ___ ___   ___  _ __  
  | | | | | '_ \ / _ \| |_) / _` | '__| |/ /   | || | | |/ __/ _ \ / _ \| '_ \ 
  | |_| | | | | | (_) |  __/ (_| | |  |   <    | || |_| | (_| (_) | (_) | | | |
  |____/|_|_| |_|\___/|_|   \__,_|_|  |_|\_\   |_| \__, |\___\___/ \___/|_| |_|
                                                   |___/                       
        You own a dinosaur park. Don't go broke. Don't get eaten.
            Now running on hardware that didn't exist in 1993.
```

**Manley & Associates / MECC, 1993 → native code, today.**
No DOSBox. No emulator. The original 16-bit DOS binary, taken apart and rebuilt.

</div>

---

## Part I — A Love Letter to a Forgotten Gem 💛

Two years *before* a CGI T. rex stomped through a Jurassic theme park on the big
screen, a small Seattle studio called **Manley & Associates** quietly shipped the
real thing onto a floppy disk: **DinoPark Tycoon**, published in 1993 by
**MECC** — the legendary Minnesota Educational Computing Corporation, the same
people who gave a generation dysentery on the **Oregon Trail**.

It's the greatest pitch in edutainment history, and it's aimed squarely at a
ten-year-old: *here is an empty plot of land and a pile of money. Go buy some
dinosaurs and build a theme park. Try not to go bankrupt. Also, try not to let
the carnivores eat the customers.*

And it is **secretly a brutal little economics engine wearing a dinosaur costume**.
Under the cartoon Apatosaurus, the game is teaching you supply and demand,
profit margins, loan interest, and the cold arithmetic of running a business:

- 🦖 **Buy your dinosaurs at auction** — bid against rivals for an
  *Albertosaurus*, an *Apatosaurus*, a *Triceratops*, a *Deinonychus*, a
  *Stegosaurus*... each with a price, an appetite, and a temperament.
- 🥩 **Feed them** — herbivores want plants, carnivores want meat, and meat is
  *expensive*. Underfeed a dino and it gets unhappy. Underfeed a *hungry
  carnivore* and you have a different, faster problem.
- 🧱 **Fence them in** — buy land, build enclosures, and for the love of god put
  a strong enough fence around the things with teeth.
- 🚌 **Sell tickets** — the bus pulls up, the visitors pour in, and you set the
  prices. Too high and nobody comes; too low and you can't make payroll.
- 🏦 **Visit the bank** — take a loan, watch the interest, balance the books at
  the **DinoMart** and the park **Diner**.

You learned to read a balance sheet because you wanted a bigger dinosaur. That's
the whole trick, and it's a *good* trick. DinoPark Tycoon never got the fame it
deserved. This project is us giving it the afterlife it earned: not trapped in an
emulator, but **recompiled** — its actual 1993 logic, turned into C, running as a
real native program.

> *"We're not reverse engineers. We're software archaeologists. And the dig site
> is every hard drive from the 90s."*

---

## Part II — The Technical Guts 🔧

### What "static recompilation" means here

We are **not** emulating a CPU at runtime. We take the original 16-bit DOS
machine code, reverse-engineer it instruction by instruction, and **lift it back
into C** that a modern compiler turns into a native x86 program. The DOS, VGA,
keyboard, and Miles-audio services the game expects get answered by a small
**DOS-on-SDL2 runtime**. The output is native code you can read, debug, fix, and
*extend* — widescreen, save states, faster auctions, things the original could
never do.

> Same pipeline that resurrected *Civilization* (1991) and *Bolo Adventures III*
> (1993) from 16-bit DOS. Built on the [pcrecomp toolkit](https://github.com/sp00nznet/pcrecomp).

### Phase 0 recon — what we're dealing with

We've cracked open the box and put the binary on the table. The good news: this
one is **friendlier than it looks**.

| Property | Finding |
|----------|---------|
| **Main binary** | `DINOPARK.EXE` — 262 KB, 16-bit DOS MZ executable. *All* game logic. |
| **Packed?** | ❌ **No.** Clean, unpacked MZ — no PKLITE/LZEXE/EXEPACK. No unpacking step. We can decode it directly. |
| **Relocations** | 4,139 — a big, segmented, large-memory-model program. |
| **Compiler** | 🛠️ **Borland** 16-bit C toolchain (Turbo C / Borland C++ era) — runtime fingerprint in the binary. |
| **Audio** | 🎵 **Miles Sound System** (AIL / `MIDPAK`). The pile of `.COM` files are Miles digital + MIDI drivers (AdLib, Sound Blaster, Pro Audio Spectrum, Windows Sound System…); music is `.XMI` (XMIDI). |
| **Setup** | `CONFIG.EXE` + `SETUP.BAT` — sound-card configuration front-end. |
| **Graphics/data** | `.PIC` (full-screen art), `.ACT` (animations/actors — one per dinosaur and screen), `.ABT` (sprites/audio bits). Formats reversed in parallel. |

Unlike Bolo (PKLITE-packed QuickBASIC), DinoPark is an **unpacked Borland C**
program — which means it walks the cleanest possible path: **decode → analyze →
lift**, no decompression detour. See [`docs/RECON.md`](docs/RECON.md) for the
full teardown.

### The pipeline

```
  DINOPARK.EXE (unpacked 16-bit DOS MZ, Borland C, large model)
        │  ① decode      8086 machine code → instructions
        ▼
  disassembly + relocations
        │  ② analyze     functions, call graph, strings, segments
        ▼
  symbol table
        │  ③ lift        8086 → C, against a DOS/VGA/Miles runtime
        ▼
  C source  ──④ shim──▶  VGA→SDL2, DOS INTs + Miles→runtime  ──⑤ build──▶  native DINOPARK
```

The graphics, animation (`.ACT`), and audio (`.XMI`) formats are reverse-engineered
in parallel — see [`docs/FORMATS.md`](docs/FORMATS.md) (WIP).

---

## 📸 What it looks like

Native code, no emulator. Every one of these is the recompiled game running.

| | |
|---|---|
| ![Title screen](docs/img/title.png) | ![The credits roll](docs/img/credits.png) |
| **The title screen** — the park, the logo, and the menu | **The credits**, over an animating park |
| ![The bank](docs/img/bank.png) | ![Main street](docs/img/mainstreet.png) |
| **The bank** approves your $5,000 loan | **Main street** — Dino City, the diner, the general store |
| ![The Real Estate office](docs/img/realestate.png) | ![Choosing a plot](docs/img/plots.png) |
| **The Real Estate office** — desert $500/acre, plains $1000, marsh $750 | **Choosing a plot** to buy |
| ![Saved parks](docs/img/loadgame.png) | |
| **Saved parks**, read straight off the original disk | |

---

## Status

🎮 **Phase 6 — it plays.**

From a dusty 1993 floppy to a native executable you can sit down in front of.
It runs its real Borland startup, plays its whole credit sequence, opens the
park, and answers the mouse: click, and the bank approves your $5,000 loan.
Main street, the Real Estate office, the notice board, the General Ledger, your
saved parks — the game, running as native code, with its music playing. No
emulator, no copyrighted bytes in this repo.

- ✅ Rights situation researched (see below) — abandonware; **no game files shipped here**.
- ✅ Binary triaged: unpacked Borland-C 16-bit DOS exe, **large model**, Miles audio.
- ✅ Project scaffolded on the [pcrecomp toolkit](https://github.com/sp00nznet/pcrecomp).
- ✅ **Phase 1 — decoded & mapped:** 693 functions, 75,175 instructions, 883 strings.
  Large-model call graph completed — **3,609/3,611 far calls resolved (99.9%)**.
  See [`docs/FUNCTIONS.md`](docs/FUNCTIONS.md). Upstreamed to
  [`pcrecomp/disasm/largemodel16.py`](https://github.com/sp00nznet/pcrecomp).
- ✅ **Phase 2 — naming + assets:** string-xref naming pinned the dinosaur-species
  table, employee-hiring and finance screens; **cracked the `UNC` asset container
  family — `UNC2` actor format 100% solved (55/55 files)**, `UNCP` pictures mapped.
  See [`docs/PHASE2.md`](docs/PHASE2.md) and [`docs/FORMATS.md`](docs/FORMATS.md).
- 🔬 **Phase 3 — codec fully located (Ghidra):** imported + decompiled all 839
  functions; **found the real sprite codec** — an **in-place DPCM decompressor**
  (`FUN_1000_0e50` + register-arg helpers) and a **self-modifying, jump-table
  planar-VGA blitter** (`FUN_191d_0bf7`) that even Ghidra can't fully decompile.
  Per-sprite header is `width,height` (ALBERT 62×43). See [`docs/CODEC.md`](docs/CODEC.md).
- ⚙️ **Phase 4 — the lift is live, and it renders:** stood up the **recomp16**
  pipeline and lifted the decode functions to C (`tools/lift_dinopark.py`). The
  lifted **`fn_1907`** RLE blitter (from `FUN_1907_00be`), executed natively on the
  recomp16 CPU model, **decodes `AUCTION.PIC` into the full 320×200 auction-hall
  screen** — the first DinoPark image rendered *by recompiled code*, no emulator.
  (Tracing the loader also revealed the `.ACT` dino actors use a separate
  self-modifying VGA blitter; the `.PIC`/`.ABT` art uses the RLE path we lifted.)
  See [`docs/PHASE4.md`](docs/PHASE4.md).
- 🎨 **Palette solved — full color!** Read the real `.PIC` loader (`FUN_1d88_0f75`)
  and found the palette is **embedded in each full-screen `.PIC`**: `[UNCP][off]
  [size][W][H][768-byte 6-bit palette][RLE image]`. Decode the image from offset
  **784** and use **`[16:784]`** as the palette → the auction hall renders in its
  real DinoPark colors (tan barn walls, the green "CURRENT CAPITAL" banner, purple
  chairs), entirely from the lifted `fn_1907` + the `.PIC`'s own palette. See
  [`docs/PALETTE.md`](docs/PALETTE.md).
- 🦖 **`.ACT` dinosaurs render!** Lifted the planar actor blitter `FUN_191d_08fb`
  to C and read its control walk to recover the exact `.ACT` encoding —
  `bit7`=new scanline, `bit6`=draw vs skip `(byte&0x3f)` pixels. `tools/decode_act.py`
  decodes **ALBERT.ACT → Albert the Albertosaurus**, 62×43, **585/585 pixels
  exactly**, in colour. (Also corrected Phase 3: the blitter isn't self-modifying —
  it's CS-scratch + planar VGA + a compiled-blit computed jump.) See [`docs/SPRITES.md`](docs/SPRITES.md).
- 🚀 **Phase 5 — the whole game boots as native code:** `tools/lift_full.py` lifts
  **all ~700 functions (~90,000 lines of C)**, including the Borland `c0` startup;
  it **compiles, links, and boots** (`scripts/build_boot.ps1` → `work/dino_boot.exe`).
  The harness loads the 250 KB image, **applies all 4,139 relocations**, builds a
  minimal PSP + environment, and runs the recompiled startup: DOS-version check →
  install interrupt vectors → resize memory → **calls `main`** → runs init via
  `recomp_dispatch` (function-pointer tables). Minimal DOS/VGA runtime in
  `src/recomp/runtime16.c`. See [`docs/BOOT.md`](docs/BOOT.md).
- 🔎 **Boot diagnosis (DS/BSS trace):** ruled out the usual suspects — **`DS` is
  correct** (`0x3020` DGROUP at every dispatch) and **initialised data loads
  correctly**. The real blocker before the title screen is **jump-table / `switch`
  dispatch**: indirect jumps land on mid-function case-blocks, which don't
  recompile cleanly as standalone functions (garbage = code bytes read as
  pointers). Convergence harness + instruction-boundary validation in place.
- 🎬 **Phase 6 — the game runs.** Five things stood between booting and running,
  and each was found by instrumenting rather than reasoning:
  - **The memory manager was a stub.** The analyzer's last function ends at
    `2F831` and the allocator's assembly helpers all live above it, with no
    Borland prologue to find them by — including the block-mover compaction
    slides blocks with, which had been lifted to `cpu->sp += 2`. Compaction
    could not work, the heap fragmented, and the allocator spun.
  - **The heap was 64K short.** The PSP sat at `0x9000`, costing the game memory
    DOS would have left free. It said so itself: `memory err 2 ... maxblk=1424`.
  - **Word `OUT` dropped AH.** `out dx, ax` lowered to a byte write, so the Mode X
    Map Mask — set with `mov ax,(mask<<8)|02; out dx,ax` — never updated. The
    four planes drifted apart and the title screen came out in vertical stripes.
  - **`int86x` builds its interrupt call on the stack.** Borland writes
    `push bp / int nn / pop bp / retf` into a stack buffer and far-calls it;
    there is no lifted function at a stack address, so everything reached that
    way silently did nothing — including the mouse probe.
  - **The timer interrupt never ran.** DinoPark hooks INT 8 and drives itself
    from it. The main loop ran, polled the mouse twenty million times, drew its
    cursor, and nothing else ever happened, because for the game no time passed.
- 🎮 **It plays.** What looked like an attract loop was the game itself: that
  park screen is the play view, the grey panels are its controls, and a click
  starts a park. Two instruments found it, and both stayed:
  - **`DINO_STATEWORD`** reports a DGROUP word whenever it changes. `fn_0CDDB`
    is the top-level state machine — `mov bx,[3C5C] / cmp bx,0x14 /
    jmp cs:[bx+8AB]`, twenty-one states through a jump table — and watching that
    word showed the game parked at `0xFFFF`, which means "this screen is still
    running", waiting for something to change it.
  - **A call histogram** over the SPCHECK instrumentation: which functions ran
    and how often. "Nothing changes" says nothing about *why*; this separates a
    handler that ran and decided nothing from one never reached at all. Diffing
    an idle run against one clicking a grid named the forty-six functions on the
    click path — three of them state-writers.
  Two more game screens turned out to be dispatching to empty stubs, rejected as
  function entries because the containing function embeds data the linear decode
  desynchronises on. Their bytes are `39 26 E4 3D 77` — Borland's stack probe,
  which appears nowhere but an entry. Unresolved stubs: 7 → 2.
- 🔎 **Stress-tested, and the biggest bug yet.** A deterministic monkey
  (`DINO_CLICK=monkey`) clicks at random for as long as a run lasts; a stall
  report says when the screen stops changing and what was on the stack. Between
  them they found:
  - **DOS find-first was unimplemented.** Unhandled INT 21h calls report
    success, so `findnext` answered "here is another file" forever. That is what
    the game uses to look for saved parks.
  - **Every buffered text-mode read returned the first bytes of DGROUP.** The
    game listed no saved parks although eight sit beside the executable. The
    validator reads six bytes and compares them with `"DINOSG"`, and the byte it
    saw was zero — although the DOS read had delivered `44 49 4E 4F 53 47`
    correctly. The low-level read strips carriage returns in place with
    `es lodsb` (`26 AC`), and lift16 dropped the segment prefix: `lodsb` read
    `DS:SI` instead, DS is DGROUP, SI was zero, and `DGROUP:0000` holds
    `\0\0\0\0Borland C++ - Copyri`. The buffer came out byte-for-byte identical
    to the start of the data segment. String instructions take their source
    through DS:SI *by default* and the prefix overrides it like any other memory
    reference — only the ES:DI destination is fixed. Big reads go straight to
    the caller's buffer and bypass all of this, which is why the art always
    loaded. The saved games list now.
- 🎵 **Sound.** The AIL calls were answered and nothing was played. Tracing
  what function 0x704 — register sequence — actually *receives* settled where to
  start: its first far pointer is the loaded file, `FORM....XDIR` sitting in
  guest memory. The game had already done the loading; all that was missing was
  somewhere for the notes to go. `src/recomp/music.c` parses XMI and plays it
  through the host's MIDI output. XMI is MIDI with two differences, and both
  matter: delays are a run of bytes below `0x80` whose values add up (chaining
  while each is `0x7F`), not MIDI's variable-length quantity; and note-on
  carries a **duration** where a MIDI file would carry a matching note-off later
  in the stream, so the player owes every note its own. `src/test_xmi.c` checks
  the invariants — every note-on answered by exactly one note-off, ticks never
  going backwards, everything in range — and all seven of the game's sequences
  pass with the counts balancing exactly. It writes a `.mid` per sequence too,
  because the one thing a check cannot tell you is whether the tune is right.
- 🦕 **It plays properly.** Driven the way a person would rather than at
  random: new game → the bank approves $5,000 → main street → the Real Estate
  office → pick a land type → BUY → the plot map → pick a plot → BUY. The plot
  comes back marked **SOLD!** and the money reads **$4,500** — a $500 desert
  plot, and the variable agrees. BUY stays greyed out until a plot is selected,
  which is the game being right rather than the recompile being lucky.
- 🖥️ **The VGA latches — every blitted sprite was smeared four pixels wide.**
  Watching it run turned up what still frames had not: some things crisp, others
  "completely blown", and the interface *pulsing*. Two bugs.
  - **Pulsing was the CRTC.** Writes to the data register at `3D5` were dropped
    entirely, and `0C`/`0D` are the address the display starts scanning from —
    Mode X page-flips by pointing them elsewhere, not by copying. So we always
    showed page 0 while the game drew page 1, alternating as it flipped.
  - **Smearing was the latches.** The blitter sets VGA **write mode 1**, where a
    read loads one byte from *every* plane into a latch and a write stores those
    latches into the enabled planes, ignoring the byte the CPU wrote — that is
    how a Mode X blit moves four planes at once. We modelled none of it: a read
    returned a single plane, the write put that one byte in all four, and every
    source pixel came out spread across the four pixels of its group.
  I had explained the second one away, deciding the smeared text on the Real
  Estate corkboard was *greeked filler* because a span filler drew it. It was
  real text. The office now reads `DESERT — Peaceful and remote. Dry, hot &
  flat. $500 per acre.`, the plot map says `SELECT A PLOT OF LAND TO PURCHASE`,
  and the start menu says `NEW GAME / OLD GAME / EXIT TO DOS`. The lesson is
  about the harness: a still frame every ten seconds cannot see flicker, and
  "which routine drew these pixels" is not evidence about what they meant.
- ⏭️ **Next:** the game runs out of memory a few minutes into a park —
  `memory err 3 ... bytes=43008 maxblk=43008`, asking for exactly the largest
  free block and failing.

---

## 🏗️ Building & running it yourself

> **Bring your own game.** This repo contains **no** original DinoPark Tycoon
> code or assets — only our tools, runtime, and recompiled-from-scratch sources.
> You supply a copy of the 1993 game you legally own; drop the files into
> `original/` and the toolchain reads from there. (See *Legal* below.)

```bash
# 1. Put your DinoPark Tycoon files in original/   (DINOPARK.EXE, *.PIC, *.ACT, ...)
# 2. Recon / generate (placeholder — pipeline lands as Phase 1 progresses)
# 3. Build:
cmake -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

(16-bit source ⇒ the native target is **32-bit/Win32** so pointer math lines up.)

---

## ⚖️ Legal & the rights situation

DinoPark Tycoon (1993) was developed by **Manley & Associates** and published by
**MECC**. The intellectual property followed MECC through a long corporate chain
of custody:

```
MECC  ──1995──▶  SoftKey  ──▶  The Learning Company  ──1999──▶  Mattel
      ──2000──▶  Gores Technology Group  ──▶  Riverdeep / HMH
      ──2014──▶  Houghton Mifflin Harcourt   (most likely current rights holder)
```

The game has been **out of print and unsupported for ~30 years** and is widely
preserved as **abandonware** (Internet Archive, My Abandonware, etc.). "Abandonware"
is a *practical* description, **not** a legal status — copyright very likely still
subsists, most plausibly with **Houghton Mifflin Harcourt**.

So we play it safe, exactly like our other projects:

- 🚫 **No game code, executables, art or audio files in this repository.**
  `original/`, `extracted/` and `work/` are git-ignored, and so is the lifted
  C — that is a machine translation of the game's own binary, so we generate it
  and never ship it. This repo is our tools, our runtime and our documentation.
  Nothing here will run without your own copy of the game.
- 🖼️ **Except the screenshots.** `docs/img` holds a handful of captures of the
  game running, reproduced to show what the project does. They are the one piece
  of the original work in here, and they are the publisher's, not ours.
- 🧑‍⚖️ You must **own a copy** of DinoPark Tycoon to use this. Bring your own files.
- 📨 If you're the current rights holder and want to talk, open an issue. This is
  a non-commercial software-preservation and interoperability effort.

This project is unaffiliated with Manley & Associates, MECC, or Houghton Mifflin
Harcourt. All trademarks belong to their respective owners.

### 📄 Licence

Everything **we** wrote — the lifter, the runtime, the tools, the scripts, the
docs — is **[MIT](LICENSE)**. Take it, learn from it, point it at your own
1993 floppy.

That licence stops at the edge of our own work. DinoPark Tycoon is not ours to
license, and none of it is here to be licensed.

---

## 🙏 Credits

- **Manley & Associates** — for building it.
- **MECC** — for believing a kid would learn economics if you bribed them with a Stegosaurus. You were right.
- Built with the [**pcrecomp**](https://github.com/sp00nznet/pcrecomp) toolkit — *everything old is new again.*

<div align="center">

*Now go feed your dinosaurs. The bus is coming.* 🚌🦕
</div>
