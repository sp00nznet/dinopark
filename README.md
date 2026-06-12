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

## Status

🚧 **Phase 2 done — named & asset formats cracked.**

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
- ⚙️ **Phase 4 — the lift is live:** stood up the **recomp16** pipeline and
  **lifted the DPCM decoder group to C** (`tools/lift_dinopark.py` →
  `fn_0E50`+3 helpers). It **compiles and *executes natively*** on a real
  `ALBERT.ACT` sprite — the first DinoPark machine code running as recompiled C,
  no emulator. Correct full-sprite framing needs the loader (`FUN_1862_0797`)
  lifted too — the remaining step before the dino renders. See [`docs/PHASE4.md`](docs/PHASE4.md).
- ⏭️ **Next:** lift the sprite loader so the decoder gets loader-correct input →
  render a dinosaur to PNG *from the recompiled code*; then the actor script VM.

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

- 🚫 **No original game code, executables, art, or audio in this repository.**
  `original/`, `extracted/`, and `work/` are git-ignored. This repo is tools +
  our own recompiled source only.
- 🧑‍⚖️ You must **own a copy** of DinoPark Tycoon to use this. Bring your own files.
- 📨 If you're the current rights holder and want to talk, open an issue. This is
  a non-commercial software-preservation and interoperability effort.

This project is unaffiliated with Manley & Associates, MECC, or Houghton Mifflin
Harcourt. All trademarks belong to their respective owners.

---

## 🙏 Credits

- **Manley & Associates** — for building it.
- **MECC** — for believing a kid would learn economics if you bribed them with a Stegosaurus. You were right.
- Built with the [**pcrecomp**](https://github.com/sp00nznet/pcrecomp) toolkit — *everything old is new again.*

<div align="center">

*Now go feed your dinosaurs. The bus is coming.* 🚌🦕
</div>
