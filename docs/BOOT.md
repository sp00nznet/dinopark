# Playable bring-up (Phase 5) — the whole game runs as native code

DinoPark Tycoon is now lifted **in full** and executes as a native program.

```
scripts\build_boot.ps1      # lift all 697 funcs -> cl -> work\dino_boot.exe
work\dino_boot.exe          # loads the image, runs the recompiled startup
```

## What works ✅

- **Full-program lift.** `tools/lift_full.py` lifts **697 functions / ~90,000
  lines of C** against the recomp16 CPU model — every detected function plus the
  Borland `c0` startup (discovered by following the entry's near calls; the
  analyzer misses it because it has no standard prologue).
- **Dispatch table.** `recomp_dispatch.c` maps image address → function pointer
  so indirect calls/jumps resolve at runtime (`call/jmp` through registers/tables).
- **It compiles and links** into `work/dino_boot.exe` (MSVC, 64-bit host — the
  flat `seg:off → mem[]` model is pointer-size-agnostic).
- **It boots and runs the real startup → `main`.** `boot.c` + `runtime16.c` load
  the 250 KB image, **apply all 4,139 relocations**, build a **minimal PSP +
  environment** above the image, and enter at `0000:0000` with `DS=ES=PSP`. The
  recompiled Borland `c0` startup then executes for real:
  ```
  INT 21h AH=30 (DOS version) → AH=35×4/25 (install int vectors)
          → AH=4A (resize memory block) → call main (fn_01604 → FUN_1000_15ad)
  ```
  `main` runs its init sequence, and the game's **indirect calls now dispatch**
  (`lifter.dispatch=True` → `recomp_dispatch` reads the function pointer from
  memory and calls the target by address). It boots cleanly — no crash.

## Convergence harness

Indirect-call misses are fed back round by round: `recomp_dispatch` records
in-range misses, `recomp_dump_misses` writes `work/dino_misses.txt`, and
`lift_full.py` reads them back as forced function starts (filtered to real
instruction boundaries inside their containing function). `scripts/converge.ps1`
runs lift -> build -> boot -> collect in a loop.

Delete `work/dino_misses.txt` whenever anything upstream of it changes. A miss
recorded under a bug is usually a bad pointer, and forcing bad pointers as
function starts corrupts the next lift.

## Five bugs between "boots cleanly" and "runs the game"

The boot used to reach `main`, dispatch into garbage, and return. It looked like
a data-setup problem. It was five separate things, and the first one hid the
other four.

**1. The build had been failing silently since June.** `converge.ps1` decided a
round succeeded by testing whether `work/dino_boot.exe` existed -- and it always
existed, left over from the last build that worked. Every measurement for weeks
was of the same stale binary. It now deletes the exe before building.

**2. CS was stale for every function.** `lift16` resolves every `cs:`-relative
read and every near indirect dispatch through a per-function code-segment
constant, `_CODE_SEG`; unset, it falls back to `cpu->cs`, which `runtime16.c`
writes once at load and never maintains across the C-call dispatch. So all 53
`jmp word cs:[bx+table]` switch dispatches read their table from the wrong
segment. `build_segmap()` recovers the real map -- see below.

**3. `dispatch_near` did not exist.** For a near indirect call the lifter emits
`push16(cpu,0xFFFF); dispatch_near(...)`: a dummy return word, then the call.
On a miss that word has to come back off, or the caller's frame sits one slot
low. Nothing defined the function, and its absence is what turned a single
missed `_INIT_` entry into the endless stream of code-bytes-as-pointers -- the
walker's own `bx`/`si` were being read off a desynchronised stack.

**4. The startup ran under the wrong CS.** With no seeded function below it, the
Borland startup fell through to a last-resort "its own paragraph" rule and got
`SEG_001E`, so its near calls dispatched into a segment nothing else shared. The
MZ header already says otherwise: entry is `0000:0000`, so image offset 0 is
seeded with CS=0 like any far-call target.

**5. Near call/jmp targets did not wrap.** A near target is `CS:(IP + rel)` and
that offset wraps at 16 bits; the lifter resolves it as `func_start + disp`,
which is the same thing only when it does not wrap. Every call backwards past
the segment base wraps, and **707 of DinoPark's 1136 near calls** (62%) resolved
somewhere arbitrary. `lift_full.py` now rewrites `disp` to the wrap-correct
target before lifting, the same trick bolo uses.

**6. Every far call compiled to an empty stub.** `lift16` resolved far targets
with formulas carried over from Civ (`seg*16 + off - 0x14`, and
`hdr_size + seg*16 + off`), and DinoPark keys `known_funcs` by plain image
offset, so neither ever matched. **3,574 far calls -- nearly every call a
large-model program makes -- became 360 no-op functions**, which is the real
reason the boot "ran cleanly" and did nothing. The lifter now takes an opt-in
`far_base` for projects whose key space maps directly (upstream, default off so
Civ is unchanged), and `lift_full.py` sets it to 0.

That leaves far calls landing on entry points the prologue heuristic missed --
Borland omits `push bp` for functions with no locals, so the analyzer merges them
into whatever precedes them. Anything a far call points at is a function by
definition, so those targets are promoted statically (90 of them) rather than
waiting for a runtime miss. Unresolved far calls: **3,574 -> 25**.

### Pixels

There is a window now. `src/recomp/video.c` presents a 320x200 8-bit
framebuffer through Win32 GDI -- an 8-bit DIB takes the VGA palette directly,
so it costs no dependency, and it also writes a BMP so a headless run leaves
something to look at.

Two things feed it:

- **`scripts\build_pic.ps1`** decodes a `.PIC` with the lifted `fn_1907` and
  shows it. `tools/gallery.py` does every file at once.
- **The boot runtime** opens the window when the game sets mode 13h and
  presents `mem[0xA0000]` on the retrace poll, which is where the game paces
  itself anyway. `work/vga_exit.bmp` is written on the way out either way.

### What the game needed before it would draw

It was refusing on its own terms, and we were not listening. Implementing INT
21h AH=09 (print string) and AH=40 (write) put its complaints on stdout:

- *"This program can only run on an MCGA or VGA system"* -- INT 10h AH=1A, the
  display-combination-code call, was a stub returning nothing, which reads as a
  CGA. It now answers VGA, along with AH=0F and AH=12/BL=10.
- **`open 'product.pf'` failed.** The game asks for its files by bare name, the
  way it would on the floppy it shipped on; they live in `original/`.
  `open_game_file()` tries the working directory and falls back there, stripping
  DOS drive letters and backslashes.

With both fixed the game opens `PRODUCT.PF` and **sets mode 13h**. The window
opens. It has not drawn anything yet.

### The stack: found by measuring, not by guessing

Three theories about `Stack overflow!` were wrong before one measurement
settled it. `DINO_SPCHECK=1 python tools/lift_full.py` wraps every lifted
function:

```c
void fn_196CF(CPU *cpu) { uint16_t _s = cpu->sp, _b = cpu->bp;
                          fn_196CF__body(cpu);
                          recomp_sp_check(0x196CF, _s, cpu->sp, _b, cpu->bp); }
```

A function is entered with its return frame already on the guest stack and must
leave having popped it, so the net delta is +2 (near `ret`), +4 (`retf`), or
those plus a `ret N`. Anything less means it ate stack that was not its own, and
BP must come back unchanged. One run named every offender in order. Four
distinct bugs came out of it:

**Switch arms were being dispatched instead of jumped to.** `fn_01DBD` lost 158
bytes per call. Its indirect jmp compiled to `recomp_dispatch(...); return;`,
which returns from the C function -- so `mov sp, bp / pop bp / retf` never ran,
and the caller's `pop bp` then read a call-frame sentinel. This is upstream's
`3412d02` (32-bit) ported to the 16-bit path: follow the table at decode time,
make the arms block leaders of the switching function, and emit a branch on the
target address -- `goto` inside this function, dispatch only for the rest.

DinoPark uses both Borland shapes, and both are recovered (439 arms):

```
dense    cmp bx, N / jbe / jmp default / shl bx, 1 / jmp cs:[bx+table]
sparse   mov cx, N / mov bx, values / mov ax, cs:[bx] / cmp / je hit
         add bx, 2 / loop / jmp default
    hit: jmp cs:[bx + N*2]        <- offsets sit right after the values
```

**Entries hidden behind embedded data.** `fn_05988` is an obvious function --
`push bp; mov bp, sp; sub sp, 0x58` -- but promotion rejected it, because the
containing function embeds a sparse switch's value table and the linear decode
walking through that data comes out misaligned. `valid_boundary` now also
accepts a Borland prologue on its own evidence. Unresolved stubs: 34 -> 15.

**Stubs that swallowed the frame.** An unresolved call target became
`void res_005988(CPU *cpu) { (void)cpu; }`, and the call site had pushed 2 or 4
bytes for it to pop. Every call through one walked SP down. They pop now.

Popping by the *call site* (4 for `far_`, 2 for `res_`) is what works. Deciding
instead by decoding the target and matching its `ret`/`retf` measured **worse**
-- it over-pops the `push cs; call near` sites and the stack check starts firing
again -- so the simple rule stands.

**`dispatch_far`.** `dispatch_near` cleaned up its 2-byte frame on a miss; the
far path pushed 4 and cleaned up nothing. Now mirrored.

### Two clocks and a heap

Past the stack work the boot reached mode 13h and then went completely silent
-- no DOS calls, no BIOS calls, no port reads. To find out where, the audit
wrappers keep a live call stack: `recomp_push` on the way in, popped by
`recomp_sp_check` on the way out, so whatever is still on it when the watchdog
fires is the chain that never returned. Build with `-DDINO_SPCHECK` to print it.

```
[watchdog] live call stack, outermost first:
    fn_00000  fn_0C6A6  fn_0F0C5  fn_06F48  fn_07C6F
```

`fn_07C6F` is a **CPU speed calibration loop**:

```
07C81  mov si, 0x46C      ; the BIOS tick count, at 0040:006C
07C88  mov bx, [si]
07C8A  cmp bx, [si]
07C8C  je  07C8A          ; wait for it to change
07C90  mov cx, 0x14       ; then count inner loops until the next tick
```

It reads the counter straight out of the BIOS data area rather than through
INT 1Ah, so nothing in the interrupt layer can satisfy it -- the memory itself
has to advance. A thread in `boot.c` now writes `mem[0x46C]` at 18.2 Hz.

That got it to the next wait, which was the same mistake one layer down:

```
026BA  pushf / cli
026BE  out 0x43, al       ; latch 8253 counter 0
026C3  in  al, 0x40       ; low byte
026CA  in  al, 0x40       ; high byte
026CE  not bx
```

The 8253 timer, used as a high-resolution clock by taking the difference
between two reads. Port 0x40 returned a constant, so the difference was always
zero -- **591 million reads** in one watchdog window. Ports 0x40/0x43 are
modelled now, counting down at 1.193182 MHz off the host's performance counter.

And `AH=48` (allocate) always returned segment 0x2000 -- which is *inside the
loaded image*, so the game was handed its own code to write over, and the
`BX=0xFFFF` "how much is there" probe was answered as a success rather than
with the largest free block. There is a real bump allocator now, over the gap
between the stack top and the PSP:

```
alloc FFFF para -> 0008 (avail 50F0)     probe fails, reports the largest block
alloc 50F0 para -> 3F10 (avail 50F0)     the game takes all 324 KB
```

### DOS terminate has to terminate

Every `Stack overflow!` and every one of the hundreds of nonsense recursive
exits turned out to be **post-mortem noise**. The game was reaching
`mov ah, 4Ch / int 21h` -- a deliberate, clean `exit(0)`, preceded by Borland's
`_exit` restoring interrupt vectors 00/04/05/06 -- and the runtime answered it
by setting `cpu->halted` and returning. Nothing checks that flag, so the lifted
code ran on into the bytes after the `int 21h` and everything after that was
garbage being executed.

`AH=4Ch` now snapshots the screen and calls `exit()`. The effect is out of all
proportion to the fix: the boot ends cleanly, and the SP audit reports **zero
violations** across the entire run. All the frame drift that survived the last
round was happening after the program had already asked to die.

Three diagnostics went in alongside it, and all three are worth keeping:

- **The exit call chain.** `-DDINO_SPCHECK` prints the live call stack from
  inside `AH=4Ch`, so the question "who decided to quit?" is answered directly.
- **`text_snapshot`** dumps the 80x25 text screen at B800:0000, because DOS
  programs write messages there rather than through DOS.
- **`stack_message_scan`** sweeps the stack segment for printable strings. The
  game formats its errors into a stack buffer and hands them to a printer we do
  not fully model, so they otherwise never reach anything we can read.

`AH=43h` now answers file-attribute probes from the filesystem instead of
always saying yes, `AH=3Ch` (create) is implemented, and the startup region's
call targets are carved into their own functions -- a target below `first_det`
has no containing detected function, so `valid_boundary` could never accept one
and four of them were becoming stubs that swallowed `_exit`'s frame.

### Borrowing civ's answer: override the C runtime

civ hit this same wall and its commit log says what worked -- *"Hand-implement
fopen/fclose ... bypassing broken CRT internal allocation"*, and later *"Fix FILE
struct corruption by moving state off DS"*. Some C-runtime routines are more
trouble lifted than rewritten.

`lift_full.py` has an `OVERRIDES` table now: listed functions are skipped by the
lifter and implemented natively in `src/recomp/dino_impl.c`, keeping their
`fn_XXXXX` names so callers and the dispatch table are unchanged. Three so far:

| function | why |
|---|---|
| `fn_05BE8` | `sprintf`. The Borland printf core reads its format from the wrong segment however it is driven. |
| `fn_02FA6` | `_setargv`. Returns through a computed `jmp` and leaves DS on the PSP; the game takes no arguments. |
| `fn_00419` | Miles driver load. The `.COM` driver is a separate binary we do not lift, and a zero handle makes the game quit. |

`src/test_sprintf.c` (`scriptsuild_test.ps1 sprintf`) drives one lifted
function directly, which turned a whole boot into a one-second test and is how
the sprintf replacement was validated.

### The `_INIT_` table, read statically

The startup walks a linker-built list of `{flag, priority, far ptr}` records and
calls each. Nothing else references those routines, so recursive descent never
reaches them -- they had only ever been found by the convergence loop's runtime
misses, which meant deleting `work/dino_misses.txt` silently un-registered them.

That mattered far more than it sounds. The first record is the routine that
walks Borland's stream table marking every slot free (`FILE.fd = 0xFF` at
`DGROUP:0x7630 + i*0x14 + 4`). Without it `_getstream` finds no free slot, every
`fopen` returns NULL, and the game reports `dino.cfg: file not Found` and quits.

`init_list_targets()` now reads the table out of the image, taking its bounds
from the startup code itself rather than hardcoding them:

```
mov dx, DGROUP        (image offset 0)
mov si, _INIT_        mov di, _INITEND
```

All six entries are recovered: `1C0F 25BB 26D4 2FA6 30CF 6467`.

Also fixed: the environment block. `memcpy(&mem[env+4], "C:\DINOPARK\DINOPARK.EXE", 24)`
copied exactly 24 characters -- the string's full length -- so the **terminating
NUL was never written**, and the program path a DOS program parses to find its
own directory ran on into whatever followed. That is where the stray `=` in
`=dino.cfg` came from. The block is built properly now, with real variables, the
double NUL, the count word, and a terminated path.

### Where it stops now

The boot reads its configuration and reaches graphics in the real flow:

```
open 'product.pf' -> ok      open 'dino.cfg' -> ok
INT 10h set mode 13
open 'SBPFM.ADV' -> ok       open 'MIDPAK.AD' -> ok      open 'MIDPAK.COM' -> ok
```

It no longer exits -- the watchdog is what stops it -- and it is bringing up the
full Miles audio stack. It has not drawn yet: `work/vga_exit.bmp` is still black.

Open:

- **The loaded drivers cannot be called.** `SBPRO.COM`, `MIDPAK.COM` and friends
  are separate 16-bit binaries the game loads and jumps into; the dispatch goes
  to addresses like `603E:0200` and `650E:0200` and misses. `fn_00419` reports
  success so the game continues, but everything downstream of a real driver is
  absent. Stubbing Miles at the AIL entry points is the honest fix.
- **`fn_0CCD7`** returns with SP 31352 bytes high under `-DDINO_SPCHECK`. It is
  another of the switch functions, and it is the next thing the audit names.
- The audit build and the plain build diverge here -- the plain one runs on, the
  audit one trips the stack check -- so trust the plain build for behaviour and
  the audit only for naming the function.

## The segment map

Large-model Borland splits DinoPark across **29 code segments**, and a function's
CS decides what its switch tables and near indirect dispatches read.
`work/functions.json` records only flat image offsets, so `build_segmap()` in
`tools/lift_full.py` recovers CS:

1. A far `call`/`jmp` `9A/EA seg:off` states its target's CS outright -- **258
   detected functions** seeded directly, with **zero conflicts**. The MZ header's
   entry `CS:IP` seeds image offset 0 the same way.
2. A near call cannot leave its segment, so both ends share one CS -- flood-fill
   the seeds across near-call edges (**380/693**).
3. Anything still unseeded takes the nearest seeded function below it (the linker
   lays segments out in order).

Every assignment is guarded by `0 <= fn - CS*16 < 0x10000`: an offset that does
not fit in 16 bits cannot belong to that segment, whatever the edge says.

Independently checked: brute-forcing CS at each `jmp word cs:[bx+disp]` site
under the constraint that *every* table entry must land on a valid instruction
boundary in the enclosing function solves **50 of the 53 tables**, and every
answer agrees with the map above (`fn_15764` -> `SEG_13C5`, and so on). The 3
that do not solve are the sparse switch form -- a `cmp/je/add bx,2/loop` scan
over `{value, offset}` pairs -- which needs its own table reader.

## The `_INIT_` table (ruled out)

Worth recording, since it looked like the culprit. Borland's startup walks a
linker-built table at `0x1ED..0x230`: 6-byte records `{flag, priority, far ptr}`
from `ES:SI` to `ES:DI`, lowest priority first, flag 0 meaning a near call.
Dumping it out of the relocated image shows it is **entirely correct** -- six
records at `DGROUP:7B62`, every pointer landing in code:

```
[0] flag=00 prio=02 -> 0000:1C0F      [3] flag=00 prio=10 -> 0000:2FA6
[1] flag=00 prio=10 -> 0000:25BB      [4] flag=00 prio=10 -> 0000:30CF
[2] flag=01 prio=10 -> 0000:26D4      [5] flag=01 prio=1E -> 0000:6467
```

Five of the six are not detected function starts -- they are mid-function to the
analyzer, for the no-prologue reason above -- but all six are valid instruction
boundaries and obvious entry points.

## The runtime so far (`runtime16.c`)

A minimal DOS/VGA layer, filled in as the boot demands it:
- **INT 21h** — file open/read/seek/close (→ host files), alloc/free, get
  date/time/version, exit.
- **INT 10h/16h/33h/1Ah** — video mode, keyboard (no-key), mouse (absent), timer.
- **Port I/O** — VGA Sequencer (Map Mask), Graphics Controller, CRTC, and the DAC
  (palette captured), plus the `0x3DA` retrace toggle.

## The road to interactive (next)

The startup currently returns early — it reaches game code but the init sequence
needs more runtime under it. In rough order:

1. **The custom heap.** `FUN_3ee1_*` is DinoPark's handle-based memory manager
   (alloc/lock/unlock). The asset loaders and most of `main` depend on it.
2. **Startup → `main` → game loop.** Make the C-runtime init complete so control
   reaches the real `main` and its top-level loop, not an early return.
3. **Video + display.** Wire the planar VGA writes (`0xA000` + Map Mask, already
   modelled in part) and the DAC palette to an SDL2 framebuffer — reusing the
   `decode_pic`/`decode_act` work for the actual draw path.
4. **Input + timing.** Real keyboard/mouse via SDL, a frame-paced main loop (the
   civ harness pattern in `src/main.c`).
5. **Audio.** Stub Miles (INT 66h) cleanly, then optionally a real backend.

This is the same multi-session arc civ went through to reach "interactive
boot/menu". The foundation — a fully lifted, compiling, booting native DinoPark —
is in place.

> Note: `src/recomp/gen/` is regenerated per build (gitignored). `build_boot.ps1`
> runs `lift_full.py` (whole game); `build_pic.ps1` / `decode_act.py` use
> `lift_dinopark.py` (the focused decoders). Run one workflow at a time.
