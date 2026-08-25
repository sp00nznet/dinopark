/*
 * runtime16.c - minimal DOS/VGA runtime for the DinoPark bring-up.
 *
 * First-boot scaffolding: enough INT 21h (DOS), INT 10h (video), keyboard,
 * mouse and port handling to get the Borland startup + game init running. Many
 * services are logged-and-stubbed; we fill them in as the boot demands them.
 */
#include "runtime16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define LOADSEG  0x0000   /* image loaded so seg:off matches lift (image space) */
#define HDR_SIZE 0x4800

/* ---- DOS file handles -> host FILE* ------------------------------------ */
#define MAXFH 32
static FILE *g_fh[MAXFH];
static int   g_trace = -1;

static void trace(const char *fmt, ...) {
    if (g_trace < 0) g_trace = getenv("DINO_TRACE") ? 1 : 0;
    if (!g_trace) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

/* read an ASCIIZ filename from cpu memory (DS:DX) */
static void read_fname(CPU *cpu, uint16_t seg, uint16_t off, char *out, int n) {
    uint32_t a = seg_off(seg, off);
    int i = 0;
    for (; i < n - 1 && cpu->mem[a + i]; i++) out[i] = (char)cpu->mem[a + i];
    out[i] = 0;
}

/* ---- VGA register state (modelled minimally) --------------------------- */
static uint8_t g_seq_index, g_gc_index, g_crtc_index;
static uint8_t g_map_mask = 0x0F;          /* sequencer reg 2 */
static uint8_t g_dac_wr, g_dac_comp;
static int     g_dac_chan;
static uint8_t g_palette[256][3];

uint8_t port_in8(CPU *cpu, uint16_t port) {
    (void)cpu;
    switch (port) {
        case 0x3DA: { static uint8_t t; t ^= 0x09; return t; }  /* CRT status: toggle retrace */
        case 0x3C5: return g_map_mask;
        case 0x3CF: return 0;
        case 0x3C9: return 0x3F;
        default: return 0xFF;
    }
}

void port_out8(CPU *cpu, uint16_t port, uint8_t val) {
    (void)cpu;
    switch (port) {
        case 0x3C4: g_seq_index = val; break;
        case 0x3C5: if (g_seq_index == 2) g_map_mask = val; break;
        case 0x3CE: g_gc_index = val; break;
        case 0x3CF: break;
        case 0x3D4: g_crtc_index = val; break;
        case 0x3D5: break;
        case 0x3C8: g_dac_wr = val; g_dac_chan = 0; break;
        case 0x3C9:
            g_palette[g_dac_wr][g_dac_chan] = val;
            if (++g_dac_chan == 3) { g_dac_chan = 0; g_dac_wr++; }
            break;
        default: break;
    }
}

/* ---- software interrupts ---------------------------------------------- */
static void int21(CPU *cpu) {
    uint8_t ah = cpu->ah;
    if (getenv("DINO_DBG")) fprintf(stderr, "[INT21] AH=%02X BX=%04X CX=%04X\n", ah, cpu->bx, cpu->cx);
    switch (ah) {
        case 0x25: case 0x35: break;                 /* get/set int vector - ignore */
        case 0x30: cpu->ax = 0x0005; break;          /* DOS version 5.0 */
        case 0x2C: cpu->cx = 0; cpu->dx = 0; break;  /* get time */
        case 0x2A: cpu->cx = 1993; cpu->dh = 1; cpu->dl = 1; break;  /* get date */
        case 0x19: cpu->al = 2; break;               /* current drive = C: */
        case 0x47: cpu->mem[seg_off(cpu->ds, cpu->si)] = 0; cpu->flags &= ~FLAG_CF; break; /* cwd="" */
        case 0x48: cpu->ax = 0x2000; cpu->flags &= ~FLAG_CF; break;  /* alloc -> give a seg */
        case 0x49: cpu->flags &= ~FLAG_CF; break;    /* free */
        case 0x4A: cpu->flags &= ~FLAG_CF; break;    /* resize */
        case 0x3D: {                                 /* open file */
            char name[128]; read_fname(cpu, cpu->ds, cpu->dx, name, sizeof name);
            int fh = -1; for (int i = 5; i < MAXFH; i++) if (!g_fh[i]) { fh = i; break; }
            FILE *f = fh >= 0 ? fopen(name, "rb") : NULL;
            trace("[INT21] open '%s' -> %s fh=%d\n", name, f ? "ok" : "FAIL", fh);
            if (f) { g_fh[fh] = f; cpu->ax = fh; cpu->flags &= ~FLAG_CF; }
            else   { cpu->ax = 2; cpu->flags |= FLAG_CF; }
            break;
        }
        case 0x3E: {                                 /* close */
            int fh = cpu->bx; if (fh >= 0 && fh < MAXFH && g_fh[fh]) { fclose(g_fh[fh]); g_fh[fh] = NULL; }
            cpu->flags &= ~FLAG_CF; break;
        }
        case 0x3F: {                                 /* read */
            int fh = cpu->bx; uint16_t n = cpu->cx; uint32_t buf = seg_off(cpu->ds, cpu->dx);
            size_t got = (fh >= 0 && fh < MAXFH && g_fh[fh]) ? fread(&cpu->mem[buf], 1, n, g_fh[fh]) : 0;
            cpu->ax = (uint16_t)got; cpu->flags &= ~FLAG_CF; break;
        }
        case 0x42: {                                 /* lseek */
            int fh = cpu->bx; long off = (cpu->cx << 16) | cpu->dx; int whence = cpu->al;
            if (fh >= 0 && fh < MAXFH && g_fh[fh]) {
                fseek(g_fh[fh], off, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET);
                long p = ftell(g_fh[fh]); cpu->ax = p & 0xFFFF; cpu->dx = (p >> 16) & 0xFFFF;
            }
            cpu->flags &= ~FLAG_CF; break;
        }
        case 0x44:                                   /* IOCTL: get device info */
            /* Borland's _setupio asks whether each standard handle is a
             * character device, to decide which ones to line-buffer. Report
             * 0..2 as console devices and anything else as a disk file. */
            cpu->dx = (cpu->bx <= 2) ? 0x80D3 : 0x0002;
            cpu->ax = cpu->dx;
            cpu->flags &= ~FLAG_CF; break;
        case 0x4C: trace("[INT21] exit code %d\n", cpu->al); cpu->halted = 1; break;
        default: trace("[INT21] AH=%02X (unhandled)\n", ah); break;
    }
}

void dos_int21(CPU *cpu)  { int_handler(cpu, 0x21); }
void bios_int10(CPU *cpu) { int_handler(cpu, 0x10); }
void bios_int16(CPU *cpu) { int_handler(cpu, 0x16); }
void mouse_int33(CPU *cpu){ int_handler(cpu, 0x33); }

void int_handler(CPU *cpu, int vec) {
    switch (vec) {
        case 0x21: int21(cpu); break;
        case 0x10: /* video BIOS */
            if (cpu->ah == 0x00) trace("[INT10] set mode %02X\n", cpu->al);
            break;
        case 0x16: cpu->ax = 0; cpu->flags |= FLAG_ZF; break;  /* keyboard: no key */
        case 0x33: cpu->ax = 0; break;                          /* mouse: absent */
        case 0x66: break;                                       /* Miles AIL: stub */
        case 0x1A: cpu->cx = 0; cpu->dx = 0; break;             /* timer ticks */
        case 0x03: cpu->halted = 1; break;                      /* breakpoint -> stop */
        default: trace("[INT %02X] AH=%02X (unhandled)\n", vec, cpu->ah); break;
    }
}

/* ---- image loader (MZ + relocations into image space) ------------------ */
int dino_load_image(CPU *cpu, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(sz); fread(file, 1, sz, f); fclose(f);

    uint16_t hdr_para = file[8] | (file[9] << 8);
    uint16_t nrelocs  = file[6] | (file[7] << 8);
    uint16_t reloff   = file[0x18] | (file[0x19] << 8);
    uint32_t img_off  = (uint32_t)hdr_para * 16;        /* = 0x4800 */

    /* image bytes load at linear 0 (image space): mem[k] = file[img_off + k] */
    long img_sz = sz - img_off;
    memcpy(cpu->mem, file + img_off, img_sz);

    /* apply relocations: each fixup word += LOADSEG (0 here, so identity but
     * we keep the machinery for correctness/relocatable bases) */
    for (int i = 0; i < nrelocs; i++) {
        uint16_t ro = file[reloff + 4*i] | (file[reloff + 4*i+1] << 8);
        uint16_t rs = file[reloff + 4*i+2] | (file[reloff + 4*i+3] << 8);
        uint32_t a = ((uint32_t)rs << 4) + ro;          /* image-space linear */
        uint16_t w = cpu->mem[a] | (cpu->mem[a+1] << 8);
        w += LOADSEG;
        cpu->mem[a] = w & 0xFF; cpu->mem[a+1] = w >> 8;
    }
    free(file);

    /* Build a minimal PSP + environment above the image (image lives at
     * linear 0). DOS hands a real program DS=ES=PSP and the env segment in
     * PSP:[0x2C]; the Borland c0 startup scans the env and aborts without it. */
    const uint16_t PSP_SEG = 0x9000, ENV_SEG = 0x9100, TOP_SEG = 0xA000;
    uint32_t psp = seg_off(PSP_SEG, 0), env = seg_off(ENV_SEG, 0);
    cpu->mem[psp + 0x00] = 0xCD; cpu->mem[psp + 0x01] = 0x20;          /* INT 20h */
    cpu->mem[psp + 0x02] = TOP_SEG & 0xFF; cpu->mem[psp + 0x03] = TOP_SEG >> 8; /* top of mem */
    cpu->mem[psp + 0x2C] = ENV_SEG & 0xFF; cpu->mem[psp + 0x2D] = ENV_SEG >> 8; /* env seg */
    cpu->mem[psp + 0x80] = 0;                                          /* cmdline len 0 */
    cpu->mem[psp + 0x81] = 0x0D;
    /* environment: empty (double null) + count(1) + program path */
    cpu->mem[env + 0] = 0; cpu->mem[env + 1] = 0;
    cpu->mem[env + 2] = 1; cpu->mem[env + 3] = 0;
    memcpy(&cpu->mem[env + 4], "C:\\DINOPARK\\DINOPARK.EXE", 24);

    /* MZ initial register state (image space); DOS enters with DS=ES=PSP */
    cpu->cs = LOADSEG;  cpu->ip = 0;                      /* CS:IP = 0000:0000 */
    cpu->ss = 0x3D04;   cpu->sp = 0x0080;                /* from header */
    cpu->ds = PSP_SEG;  cpu->es = PSP_SEG;
    printf("loaded %ld image bytes, %u relocs; entry %04X:%04X SS:SP %04X:%04X DS=%04X(PSP)\n",
           img_sz, nrelocs, cpu->cs, cpu->ip, cpu->ss, cpu->sp, cpu->ds);
    return 0;
}

/* Divide-by-zero from lifted code. The 8086 would raise INT 0; the guest never
 * installs a handler, so log the first few and leave the registers alone.
 * ponytail: named catz_div0 because pcrecomp's lift16 hardcodes that name --
 * a leak from the project it was written in. Rename upstream and here together. */
void catz_div0(const char *kind)
{
    static int fired = 0;
    if (fired++ >= 6) return;
    fprintf(stderr, "[DIV0] guest %s by zero; registers left unchanged\n", kind);
}
