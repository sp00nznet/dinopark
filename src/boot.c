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
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
/* Watchdog: the deep init still hits a busy-wait (data-setup WIP), so guarantee
 * the boot terminates. Override with DINO_WATCHDOG seconds. */
static DWORD WINAPI watchdog(LPVOID arg) {
    int secs = arg ? *(int *)arg : 8;
    Sleep((DWORD)secs * 1000);
    fprintf(stderr, "[watchdog] %ds elapsed — boot still running, exiting\n", secs);
    fflush(stderr);
    recomp_dump_misses("work/dino_misses.txt");
    _exit(3);
}
#endif

int main(int argc, char **argv) {
    const char *exe = argc > 1 ? argv[1] : "original/DINOPARK.EXE";

    CPU cpu;
    cpu_init(&cpu);
    if (cpu_alloc_mem(&cpu) != 0) return 1;
    if (dino_load_image(&cpu, exe) != 0) return 1;

    printf("booting recompiled DinoPark from entry %04X:%04X...\n", cpu.cs, cpu.ip);
    fflush(stdout);
#ifdef _WIN32
    static int wd_secs; { const char *e = getenv("DINO_WATCHDOG"); wd_secs = e ? atoi(e) : 8; }
    CreateThread(NULL, 0, watchdog, &wd_secs, 0, NULL);
#endif

    /* enter the lifted Borland startup at image offset 0 */
    extern void fn_00000(CPU *cpu);
    fn_00000(&cpu);

    printf("startup returned (halted=%d, AX=%04X)\n", cpu.halted, cpu.ax);
    recomp_dump_misses("work/dino_misses.txt");
    return 0;
}
