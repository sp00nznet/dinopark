/*
 * ems.c - expanded memory, because the game asks for it and then does without.
 *
 * DinoPark keeps its assets in its own heap and pages the cold ones out. Where
 * it pages them TO is expanded memory: there are five `int 67h` sites in the
 * binary and an "EMMXXXX0" string to find the driver with. On a 1993 machine
 * with EMM386 loaded, that is where the pressure went.
 *
 * We were not answering, so detection failed and everything had to fit in
 * conventional memory. It nearly does -- the heap peaks around 330K of the 393K
 * available -- and then a park left running long enough asks for one block too
 * many and the game stops with its own "memory err 3", which is the code for
 * "a block was paged out and could not be brought back".
 *
 * The game uses five functions and no more: 41 to find the page frame, 42 to
 * count pages, 43 to allocate, 44 to map, 45 to free. It maps one physical page
 * at a time and addresses records of 256 bytes, 64 to a 16K page.
 *
 * Mapping copies. A real board switches which memory the frame decodes to;
 * copying the page in and the previous one back out looks identical to the
 * program, which only ever sees the frame through normal memory reads, and it
 * keeps the rest of the runtime -- the write hooks, the plane split, the heap
 * walker -- from having to know that this window is special.
 */
#include "ems.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMS_PAGE      16384u              /* a page, and the mapping granularity */
#define EMS_PHYS      4                   /* physical pages in the frame */
#define EMS_HANDLES   16
#define EMS_DEFAULT_K 2048                /* how much to offer, in KB */

static int      g_on = -1;
static uint16_t g_frame_seg = 0xE000;     /* the 64K window, above the VGA area */
static uint8_t *g_store;                  /* the pages themselves, host-side */
static unsigned g_total, g_free;
static int      g_mapped[EMS_PHYS];       /* logical page in each physical one */
static unsigned g_handle_pages[EMS_HANDLES];
static int      g_handle_used[EMS_HANDLES];

int ems_enabled(void)
{
    if (g_on < 0) {
        const char *e = getenv("DINO_EMS");
        g_on = !(e && !atoi(e));
        if (g_on) {
            unsigned kb = EMS_DEFAULT_K;
            const char *k = getenv("DINO_EMS_KB");
            if (k && atoi(k) > 0) kb = (unsigned)atoi(k);
            g_total = kb * 1024u / EMS_PAGE;
            g_store = calloc(g_total, EMS_PAGE);
            if (!g_store) { g_on = 0; return 0; }
            g_free = g_total;
            for (int i = 0; i < EMS_PHYS; i++) g_mapped[i] = -1;
            fprintf(stderr, "[ems] %u KB at %04X:0000 (%u pages)\n",
                    kb, g_frame_seg, g_total);
        }
    }
    return g_on;
}

uint16_t ems_frame_segment(void) { return g_frame_seg; }

/* The frame, as the guest sees it. */
static uint8_t *frame(CPU *cpu, int phys)
{
    return &cpu->mem[((uint32_t)g_frame_seg << 4) + (uint32_t)phys * EMS_PAGE];
}

static void unmap(CPU *cpu, int phys)
{
    if (g_mapped[phys] < 0) return;
    memcpy(g_store + (size_t)g_mapped[phys] * EMS_PAGE, frame(cpu, phys), EMS_PAGE);
    g_mapped[phys] = -1;
}

void ems_int67(CPU *cpu)
{
    if (!ems_enabled()) { cpu->ah = 0x84; return; }   /* no such function */

    switch (cpu->ah) {
        case 0x40:                                    /* status */
            cpu->ah = 0;
            break;

        case 0x41:                                    /* where is the page frame */
            cpu->bx = g_frame_seg;
            cpu->ah = 0;
            break;

        case 0x42:                                    /* how many pages */
            cpu->bx = (uint16_t)g_free;
            cpu->dx = (uint16_t)g_total;
            cpu->ah = 0;
            break;

        case 0x43: {                                  /* allocate */
            unsigned want = cpu->bx;
            if (!want)      { cpu->ah = 0x89; break; }  /* zero pages */
            if (want > g_free) { cpu->ah = 0x88; break; } /* not enough */
            int h = -1;
            for (int i = 1; i < EMS_HANDLES; i++) if (!g_handle_used[i]) { h = i; break; }
            if (h < 0)      { cpu->ah = 0x85; break; }  /* out of handles */
            g_handle_used[h] = 1;
            g_handle_pages[h] = want;
            g_free -= want;
            cpu->dx = (uint16_t)h;
            cpu->ah = 0;
            break;
        }

        case 0x44: {                                  /* map a page into the frame */
            int phys = cpu->al;
            int log  = (int16_t)cpu->bx;
            int h    = cpu->dx;
            if (phys < 0 || phys >= EMS_PHYS) { cpu->ah = 0x8B; break; }
            if (h <= 0 || h >= EMS_HANDLES || !g_handle_used[h]) { cpu->ah = 0x83; break; }
            if (log == -1) { unmap(cpu, phys); cpu->ah = 0; break; }  /* unmap */
            if (log < 0 || (unsigned)log >= g_handle_pages[h]) { cpu->ah = 0x8A; break; }

            /* Logical pages are per handle; give each handle its own run so two
             * handles cannot land on the same backing page. */
            unsigned base = 0;
            for (int i = 1; i < h; i++) if (g_handle_used[i]) base += g_handle_pages[i];
            unsigned abs = base + (unsigned)log;
            if (abs >= g_total) { cpu->ah = 0x8A; break; }

            if (g_mapped[phys] == (int)abs) { cpu->ah = 0; break; }   /* already there */
            unmap(cpu, phys);
            memcpy(frame(cpu, phys), g_store + (size_t)abs * EMS_PAGE, EMS_PAGE);
            g_mapped[phys] = (int)abs;
            cpu->ah = 0;
            break;
        }

        case 0x45: {                                  /* free */
            int h = cpu->dx;
            if (h <= 0 || h >= EMS_HANDLES || !g_handle_used[h]) { cpu->ah = 0x83; break; }
            for (int p = 0; p < EMS_PHYS; p++) unmap(cpu, p);
            g_free += g_handle_pages[h];
            g_handle_used[h] = 0;
            g_handle_pages[h] = 0;
            cpu->ah = 0;
            break;
        }

        case 0x46:                                    /* version 4.0 */
            cpu->al = 0x40;
            cpu->ah = 0;
            break;

        default:
            cpu->ah = 0x84;                           /* unsupported function */
            break;
    }
}
