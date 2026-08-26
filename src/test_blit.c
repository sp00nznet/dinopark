/*
 * test_blit.c - drive the sprite plotter directly and check where its pixels land.
 *
 * fn_09DC7 (FUN_091d_0bf7) is the row plotter: it takes an x in CX, a width in
 * BL and a pixel stream at DS:SI, works out DI from x/4 plus the row base kept
 * in its own code segment at cs:[0x0A], and makes four passes -- one per VGA
 * plane -- with the Map Mask rotating 11/22/44/88. Each pass runs a compiled
 * blit: a computed jump into an unrolled copy block, which the lifter rewrites
 * back into a loop.
 *
 * Feeding it 0,1,2,3,... makes the answer obvious: pixel n must land at column
 * x+n. Anything else shows exactly how the plane spread is going wrong, which a
 * whole boot cannot.
 *
 *   scripts\build_test.ps1 blit [width] [x]
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "recomp/cpu.h"
#include "recomp/runtime16.h"
#include "recomp/gen/recomp_all.h"

#define BLIT_SEG  0x091D          /* the plotter's own code segment      */
#define SRC_SEG   0x6000          /* somewhere harmless for the pixels   */
#define STK_SEG   0x8800

int dino_load_image(CPU *cpu, const char *path);

int main(int argc, char **argv)
{
    int width = argc > 1 ? atoi(argv[1]) : 16;
    int x     = argc > 2 ? atoi(argv[2]) : 0;

    CPU cpu;
    cpu_init(&cpu);
    if (cpu_alloc_mem(&cpu) != 0) return 1;
    if (dino_load_image(&cpu, "original/DINOPARK.EXE") != 0) return 1;

    /* unchain the VGA the way the game does, so writes go through the planes */
    port_out8(&cpu, 0x3C4, 4);
    port_out8(&cpu, 0x3C5, 0x00);

    /* the plotter's scratch: row base 0 (top row), and the mode word clear so
     * it duplicates the plane bit into the high nibble */
    mem_write16(&cpu, BLIT_SEG, 0x0A, 0);
    mem_write16(&cpu, BLIT_SEG, 0x14, 0);

    for (int i = 0; i < width; i++)
        mem_write8(&cpu, SRC_SEG, (uint16_t)i, (uint8_t)(i + 1));   /* 1..width */

    /* The plotter selects planes with `mov al,2 / dec dx / out dx,al / inc dx`,
     * so it expects DX already pointing at the Sequencer data port -- the outer
     * blitter leaves it there. Without it every write goes to whatever the mask
     * happened to be, which is all four planes. */
    cpu.dx = 0x3C5;

    cpu.ds = SRC_SEG;  cpu.si = 0;
    cpu.es = 0xA000;
    cpu.cx = (uint16_t)x;
    cpu.bx = (uint16_t)width;                  /* BL = width */
    cpu.ss = STK_SEG;  cpu.sp = 0xFF00;
    push16(&cpu, 0xFFFF);                      /* near-call return slot */

    fn_09DC7(&cpu);

    vga_plane_report();
    const uint8_t *f = vga_compose_frame(&cpu);
    printf("width=%d x=%d\n", width, x);
    printf("expected: column %d..%d = 1..%d, everything else 0\n",
           x, x + width - 1, width);

    printf("got     : ");
    for (int i = 0; i < 40; i++) printf("%3d", f[i]);
    printf("\n");

    int bad = 0;
    for (int i = 0; i < 320; i++) {
        int want = (i >= x && i < x + width) ? (i - x + 1) : 0;
        if (f[i] != want) {
            if (bad < 6)
                printf("  column %3d: got %3d, expected %3d\n", i, f[i], want);
            bad++;
        }
    }
    printf(bad ? "FAIL: %d columns wrong\n" : "PASS: every pixel landed where it should\n", bad);
    return bad != 0;
}
