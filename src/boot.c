/*
 * boot.c - DinoPark recompilation boot harness (playable bring-up, phase 1).
 *
 * Loads the image into the recomp16 flat memory, seeds the CPU at the Borland
 * c0 entry (CS:IP = 0000:0000), and runs the recompiled startup. The startup
 * sets up the C runtime and calls the game's main. Hardware is serviced by
 * runtime16.c (DOS/VGA/keyboard stubs). This is bring-up scaffolding — it boots
 * the lifted code and reports how far it gets.
 */
#include "recomp/cpu.h"
#include "recomp/runtime16.h"
#include "recomp/gen/recomp_all.h"
#include "recomp/video.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
/* Watchdog: the deep init still hits a busy-wait (data-setup WIP), so guarantee
 * the boot terminates. Override with DINO_WATCHDOG seconds. */
static CPU *g_cpu;      /* for the watchdog's parting screenshot */
/* The BIOS tick count at 0040:006C, kept moving.
 *
 * DinoPark calibrates itself against it -- `mov si,0x46C / mov bx,[si] /
 * cmp bx,[si] / je $-2` waits for the tick to change, then counts how many
 * inner loops fit in one tick. It reads the counter straight out of the BIOS
 * data area rather than through INT 1Ah, so nothing in the interrupt layer
 * can satisfy it: the memory itself has to advance. A torn read costs at
 * worst one bad calibration sample. */
static DWORD WINAPI bios_clock(LPVOID unused)
{
    (void)unused;
    for (;;) {
        if (g_cpu && g_cpu->mem) {
            unsigned long t = vga_ticks();
            uint8_t *p = &g_cpu->mem[0x46C];
            p[0] = (uint8_t)t;         p[1] = (uint8_t)(t >> 8);
            p[2] = (uint8_t)(t >> 16); p[3] = (uint8_t)(t >> 24);
        }
            vga_sample(g_cpu);
        Sleep(55);                      /* 18.2 Hz */
    }
}

static DWORD WINAPI watchdog(LPVOID arg) {
    int secs = arg ? *(int *)arg : 8;
    Sleep((DWORD)secs * 1000);
    fprintf(stderr, "[watchdog] %ds elapsed — boot still running, exiting\n", secs);
    { extern unsigned long g_port_reads[8];
      fprintf(stderr, "[watchdog] retrace polls %lu, other port reads %lu\n",
              g_port_reads[0], g_port_reads[1]); }
#ifdef DINO_SPCHECK
    { extern unsigned g_stk[]; extern int g_stk_depth;
      fprintf(stderr, "[watchdog] live call stack, outermost first (%d deep):\n", g_stk_depth);
      for (int i = 0; i < g_stk_depth && i < 40; i++)
          fprintf(stderr, "    fn_%05X\n", g_stk[i]); }
#endif
    fflush(stderr);
    if (g_cpu) vga_snapshot(g_cpu, "work/vga_exit.bmp");
    vga_best_dump("work/best_frame.bmp");
    vga_plane_report();
    write_histogram();
    if (g_cpu) fb_scan(g_cpu, "work/fb_scan.bmp");
    if (g_cpu) heap_dump(g_cpu);
    if (g_cpu) find_signature(g_cpu, "UNC2");
    if (g_cpu) { find_block_size(g_cpu, 0x22DF); find_block_size(g_cpu, 0x22E0); }
    heap_trace_dump();
    hdr_write_dump();
    chain_break_dump();

    recomp_dump_misses("work/dino_misses.txt");
    _exit(3);
}
#endif

int main(int argc, char **argv) {
    const char *exe = argc > 1 ? argv[1] : "original/DINOPARK.EXE";

    CPU cpu;
    cpu_init(&cpu);
    g_cpu = &cpu;
    if (cpu_alloc_mem(&cpu) != 0) return 1;
    if (dino_load_image(&cpu, exe) != 0) return 1;

    printf("booting recompiled DinoPark from entry %04X:%04X...\n", cpu.cs, cpu.ip);
    fflush(stdout);
#ifdef _WIN32
    static int wd_secs; { const char *e = getenv("DINO_WATCHDOG"); wd_secs = e ? atoi(e) : 8; }
    CreateThread(NULL, 0, watchdog, &wd_secs, 0, NULL);
    CreateThread(NULL, 0, bios_clock, NULL, 0, NULL);
#endif

    /* enter the lifted Borland startup at image offset 0 */
    extern void fn_00000(CPU *cpu);
    fn_00000(&cpu);

    printf("startup returned (halted=%d, AX=%04X)\n", cpu.halted, cpu.ax);
    /* Whatever the game drew, keep it: a headless run has no window to look at
     * and the framebuffer is the only evidence it drew at all. */
    vga_snapshot(&cpu, "work/vga_exit.bmp");
    vga_best_dump("work/best_frame.bmp");
    vga_plane_report();
    write_histogram();
    fb_scan(&cpu, "work/fb_scan.bmp");
    heap_dump(&cpu);
    find_signature(&cpu, "UNC2");
    find_block_size(&cpu, 0x22DF); find_block_size(&cpu, 0x22E0);
    heap_trace_dump();
    hdr_write_dump();
    chain_break_dump();

    if (vga_window_pump()) { puts("window open - Esc or close it to exit"); vga_window_wait(); }
    recomp_dump_misses("work/dino_misses.txt");
    return 0;
}
