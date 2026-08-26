/*
 * test_sprintf.c - call the game's own sprintf directly.
 *
 * DinoPark dies reporting "%s: file not Found" with an empty %s, and the
 * formatting runs through fn_05BE8 -> fn_01DBD, the printf core whose format
 * dispatch is the 24-entry `jmp word cs:[bx+0x2250]` switch. Driving it from a
 * harness turns a whole boot into a one-second test.
 *
 *   fn_05BE8(dest_far, fmt_far, ...)   -- args pushed right to left, far
 */
#include <stdio.h>
#include <string.h>
#include "recomp/cpu.h"
#include "recomp/gen/recomp_all.h"

#define SEG_STK  0x8000
#define SEG_BUF  0x7000

int dino_load_image(CPU *cpu, const char *path);

static void put(CPU *cpu, uint16_t seg, uint16_t off, const char *s)
{
    memcpy(&cpu->mem[seg_off(seg, off)], s, strlen(s) + 1);
}

int main(int argc, char **argv)
{
    CPU cpu;
    cpu_init(&cpu);
    if (cpu_alloc_mem(&cpu) != 0) return 1;
    if (dino_load_image(&cpu, argc > 1 ? argv[1] : "original/DINOPARK.EXE") != 0)
        return 1;

    const char *fmt  = argc > 2 ? argv[2] : "%s: file not Found";
    const char *arg  = argc > 3 ? argv[3] : "DINO.CFG";

    put(&cpu, SEG_BUF, 0x100, fmt);
    put(&cpu, SEG_BUF, 0x200, arg);
    memset(&cpu.mem[seg_off(SEG_BUF, 0)], 0, 0x100);      /* destination */

    cpu.ds = 0x3020;                    /* DGROUP: the format tables live here */
    cpu.es = 0x3020;
    cpu.ss = SEG_STK; cpu.sp = 0xFF00;

    push16(&cpu, SEG_BUF); push16(&cpu, 0x200);   /* the %s argument, far   */
    push16(&cpu, SEG_BUF); push16(&cpu, 0x100);   /* the format string, far */
    push16(&cpu, SEG_BUF); push16(&cpu, 0x000);   /* the destination, far   */
    push16(&cpu, 0); push16(&cpu, 0xFFFF);        /* far-return frame       */
    fn_05BE8(&cpu);

    const char *out = (const char *)&cpu.mem[seg_off(SEG_BUF, 0)];
    printf("fmt  = %s\n", fmt);
    printf("arg  = %s\n", arg);
    printf("out  = \"%.100s\"\n", out);
    printf("%s\n", strstr(out, arg) ? "PASS: the argument came through"
                                    : "FAIL: %s produced nothing");
    return 0;
}
