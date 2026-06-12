/*
 * DinoPark — static recompilation of DinoPark Tycoon (1993, Manley & Associates / MECC).
 *
 * Native entry point. This is the hand-written host shell that boots the
 * DOS/VGA/Miles runtime and hands control to the lifted game code once the
 * Phase 1 recompilation pipeline produces it. Placeholder for now.
 */
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    puts("DinoPark recomp - Phase 0 scaffold. Bring your own game files in original/.");
    puts("Recompiled game code lands here as Phase 1 progresses. The bus is coming.");
    return 0;
}
