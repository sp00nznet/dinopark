# Screens, commands and the shop gate

What the UI is made of, recovered while chasing a general store that would not
open. Written down because none of it is obvious from the outside and all of it
is reusable the next time a screen refuses to do something.

## The state machine

`fn_0CDDB` is the top of it:

```
mov bx, word ds:[0x3C5C]      ; the state
cmp bx, 0x14                  ; twenty-one of them
jbe +
jmp end
shl bx, 1
jmp word cs:[bx+0x8AB]        ; a jump table in its own code segment
```

`DGROUP:3C5C` holds the state. `0xFFFF` means "the screen currently running has
not finished", and a screen handler loops until something else changes it —
which is why watching that one word says more about where the game thinks it is
than any screenshot.

Known states:

| State | Screen |
|-------|--------|
| 1  | main street |
| 2  | the real estate office and its plot map |
| 0x0F | the bank |
| 0x12 | the park, which is also the title screen |
| 7  | the general store |
| 3, 4, 6, 8, 0x0A | the other shops, see below |

## Commands

`fn_1C6F8` is the command dispatcher. Every hotspot the player clicks arrives
here as a number in its first argument, and **a number it does not recognise is
silently ignored** — no message, no state change, nothing. A screen that does
nothing when clicked is usually this.

| Command | Effect |
|---------|--------|
| 0 | idle; issued continuously |
| 1, 3, 11 | handled inline |
| 100 | state 4 |
| 101 | state 8 |
| 102 | state 3 |
| 103 | the real estate office: counts your pens, and if you own all ten says so (string 199) rather than opening |
| 104 | state 6 |
| 105 | **the general store**, state 7 |
| 106 | state 0x0A |

Commands 103–106 go through a second jump table at `cs:[bx*2+0x14BB]`, with
`CS = 0x1B5D`.

## The shop open/closed gate

Commands 100–105 are the six shops on main street, and each has a record ten
bytes long. The first word is its open flag:

```
cmp [bp+6], 0x64 / jl ...      ; is this a shop?
cmp [bp+6], 0x69 / jg ...
imul 10                        ; cmd * 10
cmp word ds:[bx+0x547E], 0     ; the flag
jne open
mov ax, 0xDE                   ; else string 222, "This store is closed."
```

So the flags live at `DGROUP:0x5866` upward, ten bytes apart. Two are set at the
start — the bank and the real estate office, the only two you can use before you
own anything — and buying your first plot of land sets the rest. Watching all six
across a purchase shows exactly that: zero while main street is up, then
`realest.act` and `blueprnt.pic` load as the land is bought, then one.

Nothing writes that array through the index, and nothing writes it by absolute
address either, so it is reached through a pointer; the way to see it move is to
watch the addresses rather than to search for the code.

## Reading what the game is saying

`fn_195ED` takes a string index, seeks to `index*4` in the table at the head of
`strings.dat`, and reads the string that offset names. So

```
DINO_FNTRACE=195ED           every message the game decided to display
DINO_FNTRACE=1C6F8           every command a click issued
python tools/strings_at.py work/btrun.txt
```

turns a session into a list of what the player was told, in order. A screen that
refuses to act is usually saying why, in a tooltip nobody captured — clicking the
closed store produces `cmd 105` and string 222, which is the whole gate proving
itself in two lines of log.

## What this ruled out

The general store's catalog does not open. It is not the shop gate: the hotspot
is recognised, the command is issued, the flag is checked, and after buying land
the flag is set. Whatever is wrong is inside state 7, and the next thing to
capture is the command the catalog click issues — nothing at all means the
hotspot is not matching, and a command the dispatcher does not list means it
falls into that silent default.
