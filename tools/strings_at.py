#!/usr/bin/env python3
"""
strings_at.py - turn string indices from a trace into the text the game showed.

fn_195ED is the string fetcher: it takes an index, seeks to index*4 in the
offset table at the head of strings.dat, and reads the string that offset names.
So DINO_FNTRACE=195ED records every message the game decided to display, and
this turns that log back into English.

    scripts\run.ps1 -Play -Audit         (with DINO_FNTRACE=195ED set)
    python tools/strings_at.py work/btrun.txt

Handy when the question is not "did it crash" but "what did it think it was
telling the player" -- a screen that refuses to do anything is usually saying
why, in a tooltip nobody captured.
"""
import re, struct, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(path):
    d = open(path, "rb").read()
    n = struct.unpack_from("<I", d, 0)[0] // 4
    out = []
    for i in range(n):
        s = struct.unpack_from("<I", d, i * 4)[0]
        e = d.find(b"\n", s)
        if e < 0:
            e = d.find(b"\0", s)
        out.append(d[s:e].decode("latin1", "replace"))
    return out


def main():
    log = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "work", "btrun.txt")
    txt = load(os.path.join(ROOT, "original", "strings.dat"))
    seen, order = {}, []
    # `[fn] 195ED in ... stack: <ret ip> <ret cs> <index> ...`
    pat = re.compile(r"\[fn\] 195ED in .*stack:\s+\S+\s+\S+\s+([0-9A-Fa-f]{4})")
    for line in open(log, encoding="latin1"):
        m = pat.search(line)
        if not m:
            continue
        i = int(m.group(1), 16)
        if i not in seen:
            seen[i] = 0
            order.append(i)
        seen[i] += 1
    if not order:
        print("no string fetches in %s -- was DINO_FNTRACE=195ED set?" % log)
        return
    print("%d distinct strings shown, in the order first seen:" % len(order))
    for i in order:
        t = txt[i] if i < len(txt) else "<out of range>"
        print("  [%3d] x%-4d %s" % (i, seen[i], t))


if __name__ == "__main__":
    main()
