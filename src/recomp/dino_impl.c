/*
 * dino_impl.c - native stand-ins for lifted C-runtime functions.
 *
 * Following what civ did: some CRT routines are more trouble lifted than
 * rewritten. DinoPark's Borland printf core (fn_01DBD, reached through
 * fn_05BE8) reads its format string from the wrong segment however it is
 * driven -- see src/test_sprintf.c -- so every message the game formats comes
 * out empty, including the filename in "%s: file not Found" that makes it quit.
 *
 * These are listed in lift_full.py's OVERRIDES, which skips lifting them and
 * points the dispatch table here instead.
 *
 * Calling convention, verified against the callers: a far call, so on entry
 * cpu->sp addresses the pushed return IP, arguments start at sp+4, and the
 * function returns by dropping the 4-byte frame. The caller clears the
 * arguments itself.
 */
#include "cpu.h"
#include "runtime16.h"
#include <stdio.h>
#include <string.h>

#define ARG(n) mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4 + (n) * 2))

/* Copy an ASCIIZ string out of guest memory. */
static void guest_str(CPU *cpu, uint16_t seg, uint16_t off, char *out, int max)
{
    uint32_t a = seg_off(seg, off);
    int i = 0;
    for (; i < max - 1 && cpu->mem[a + i]; i++) out[i] = (char)cpu->mem[a + i];
    out[i] = 0;
}

/*
 * sprintf(char far *dest, const char far *fmt, ...)
 *
 * Borland's own set: %d %i %u %o %x %X %c %s %%, an optional `l` for the
 * 32-bit forms, and flags/width/precision in between. Each directive is handed
 * to the host's printf with the argument pulled off the guest stack, which is
 * where the widths and zero-padding come from for free.
 */
void fn_05BE8(CPU *cpu)
{
    uint16_t doff = ARG(0), dseg = ARG(1);
    uint16_t foff = ARG(2), fseg = ARG(3);
    int argi = 4;                              /* first vararg word */

    char fmt[512];
    guest_str(cpu, fseg, foff, fmt, sizeof fmt);

    char out[1024];
    int n = 0;

    for (const char *p = fmt; *p && n < (int)sizeof out - 1; ) {
        if (*p != '%') { out[n++] = *p++; continue; }

        const char *start = p++;               /* at the '%' */
        if (*p == '%') { out[n++] = '%'; p++; continue; }

        const char *flags = p;
        while (*p && strchr("-+ #0", *p)) p++;           /* flags */
        while (*p >= '0' && *p <= '9') p++;              /* width */
        if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
        const char *fw_end = p;
        int is_long = 0;
        while (*p && strchr("lhL", *p)) { if (*p == 'l' || *p == 'L') is_long = 1; p++; }
        char conv = *p ? *p++ : 0;

        /* Rebuild the directive for the host: % + flags/width/precision, then
         * our own length modifier, so the host reads the size we actually pass
         * rather than the one the 16-bit source asked for. */
        char spec[64];
        int fw = (int)(fw_end - flags);
        if (fw > 40) fw = 40;
        int k = 0;
        spec[k++] = '%';
        memcpy(spec + k, flags, (size_t)fw); k += fw;

        char piece[512];
        piece[0] = 0;

        switch (conv) {
        case 'd': case 'i': {
            long v = is_long ? (long)(int32_t)((uint32_t)ARG(argi) |
                                               ((uint32_t)ARG(argi + 1) << 16))
                             : (long)(int16_t)ARG(argi);
            argi += is_long ? 2 : 1;
            spec[k++] = 'l'; spec[k++] = conv; spec[k] = 0;
            snprintf(piece, sizeof piece, spec, v);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned long v = is_long ? ((uint32_t)ARG(argi) |
                                         ((uint32_t)ARG(argi + 1) << 16))
                                      : (unsigned long)ARG(argi);
            argi += is_long ? 2 : 1;
            spec[k++] = 'l'; spec[k++] = conv; spec[k] = 0;
            snprintf(piece, sizeof piece, spec, v);
            break;
        }
        case 'c':
            spec[k++] = 'c'; spec[k] = 0;
            snprintf(piece, sizeof piece, spec, (char)(ARG(argi) & 0xFF));
            argi += 1;
            break;
        case 's': {
            /* large model: a char* argument is far, offset then segment */
            uint16_t soff = ARG(argi), sseg = ARG(argi + 1);
            argi += 2;
            char s[512];
            guest_str(cpu, sseg, soff, s, sizeof s);
            spec[k++] = 's'; spec[k] = 0;
            snprintf(piece, sizeof piece, spec, s);
            break;
        }
        default:                                        /* unknown: pass through */
            snprintf(piece, sizeof piece, "%.*s", (int)(p - start), start);
            break;
        }

        for (const char *q = piece; *q && n < (int)sizeof out - 1; q++) out[n++] = *q;
    }
    out[n] = 0;

    uint32_t d = seg_off(dseg, doff);
    memcpy(&cpu->mem[d], out, (size_t)n + 1);

    cpu->ax = (uint16_t)n;                     /* sprintf returns the length */
    cpu->sp = (uint16_t)(cpu->sp + 4);         /* retf: drop the far frame */
}

/*
 * _setargv -- Borland's command-line splitter, run from the _INIT_ table.
 *
 * The original builds argv in space it carves off the stack and returns
 * through a computed `jmp word ds:[0x7918]` rather than a ret, after stashing
 * its return address and DS in DGROUP and a CS-resident slot. Lifted, it comes
 * back with DS still pointing at the PSP, so that jump reads a garbage target
 * and the boot wanders off.
 *
 * DinoPark takes no arguments, so the whole routine is skippable. It is called
 * near, so returning means dropping the 2-byte frame the call site pushed and
 * leaving argc/argv alone -- the C startup treats zero arguments as normal.
 */
void fn_02FA6(CPU *cpu)
{
    cpu->sp = (uint16_t)(cpu->sp + 2);         /* ret */
}

/*
 * Miles AIL driver load -- report success without loading anything.
 *
 * The real routine reads a .COM driver (SBPRO.COM and friends, named by
 * DINO.CFG) into memory and calls its entry point. That is a separate 16-bit
 * binary which is not part of the recompiled image, so there is nothing to
 * call: the dispatch goes to an address like 603E:0200 and misses.
 *
 * The caller only tests the returned handle against zero, and on zero it
 * prints "Unable to load sound driver" and quits -- taking the whole game with
 * it. Returning a non-zero handle lets the game carry on silently; later calls
 * through Miles land on the same harmless dispatch miss.
 *
 * Far call, one far-pointer argument, handle returned in AX.
 */
void fn_00419(CPU *cpu)
{
    cpu->ax = 0x0001;                          /* a handle, and not zero */
    cpu->dx = 0x0000;
    cpu->sp = (uint16_t)(cpu->sp + 4);         /* retf */
}
