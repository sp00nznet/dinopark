/*
 * runtime16.h - host runtime surface the lifted DinoPark code calls into.
 *
 * The lifter emits three escape hatches: software interrupts (int_handler),
 * port I/O (port_in8/out8), and address-indirect calls (recomp_dispatch). The
 * implementations model just enough DOS/VGA for the game to boot.
 */
#ifndef DINO_RUNTIME16_H
#define DINO_RUNTIME16_H

#include "cpu.h"

/* software interrupt: INT 10h video, 16h keyboard, 21h DOS, 33h mouse, 66h Miles */
void int_handler(CPU *cpu, int vec);
/* named vectors the lifter emits directly */
void dos_int21(CPU *cpu);
void bios_int10(CPU *cpu);
void bios_int16(CPU *cpu);
void mouse_int33(CPU *cpu);

/* x86 port I/O — VGA sequencer/GC/DAC/CRTC live here */
uint8_t port_in8(CPU *cpu, uint16_t port);
void    port_out8(CPU *cpu, uint16_t port, uint8_t val);

/* address-indirect call/jmp dispatch (generated in recomp_dispatch.c) */
int  recomp_dispatch(CPU *cpu, unsigned seg, unsigned off);   /* 1 = dispatched */
/* near indirect call: the lifted code pushes a dummy return word first, so a
 * miss has to pop it back off or the callee's frame is one slot low. */
void dispatch_near(CPU *cpu, unsigned seg, unsigned off);
void recomp_dump_misses(const char *path);   /* in-range dispatch misses -> file */

/* boot: load the image + relocations into cpu->mem and seed segment regs */
int  dino_load_image(CPU *cpu, const char *path);

void catz_div0(const char *kind);   /* divide-by-zero from lifted code */

/* present mem[0xA0000] in the window / to a BMP */
void vga_flush(CPU *cpu);
void vga_snapshot(CPU *cpu, const char *path);

#endif
