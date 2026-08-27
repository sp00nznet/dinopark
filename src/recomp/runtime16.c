/*
 * runtime16.c - minimal DOS/VGA runtime for the DinoPark bring-up.
 *
 * First-boot scaffolding: enough INT 21h (DOS), INT 10h (video), keyboard,
 * mouse and port handling to get the Borland startup + game init running. Many
 * services are logged-and-stubbed; we fill them in as the boot demands them.
 */
#include "runtime16.h"
#include "video.h"
#include "music.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

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

/* The game asks for its data files by bare name, the way it would on the
 * floppy it shipped on. They live in original/ here, so try the working
 * directory first and fall back to there. DOS paths are backslashed and may
 * carry a drive letter; strip both. */
static FILE *open_game_file(const char *name, const char *mode)
{
    FILE *f = fopen(name, mode);
    if (f) return f;

    const char *base = name;
    if (base[0] && base[1] == ':') base += 2;          /* drop C: */
    for (const char *p = base; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;   /* keep the last component */

    char path[256];
    snprintf(path, sizeof path, "original/%s", base);
    return fopen(path, mode);
}

/* ---- VGA register state (modelled minimally) --------------------------- */
static uint8_t g_seq_index, g_gc_index, g_crtc_index;
static uint8_t g_map_mask = 0x0F;          /* sequencer reg 2 */
static uint8_t g_dac_wr, g_dac_comp;
static int     g_dac_chan;
static uint8_t g_palette[256][3];

/* Mode 13h is 320x200 linear at A000:0000, which in this flat memory model is
 * just mem[0xA0000] -- the lifted code writes pixels straight there and no
 * plane emulation is involved. (An unchained/Mode-X screen would need the map
 * mask honoured on write; nothing has asked for one yet.) */
#define VGA_BASE 0xA0000u
#define VGA_W 320
#define VGA_H 200
/* DOS heap. Everything below is taken: the image runs to 0x3D0C0 and the stack
 * sits just above it, so allocations come from the gap between the stack top
 * and the PSP. A bump allocator is enough -- the game frees at exit, if ever. */
#define HEAP_LO 0x3F10u
/* Up to the PSP, which now sits just under the video window. It used to be at
 * 0x9000 with the environment above it, which quietly cost the game the top
 * 64K of conventional memory -- DOS would have left everything below 0xA000
 * free. The intro ran out at the Manley screen and reported its own
 * "memory err 2" because of it. */
#define HEAP_HI 0x9F00u
static uint16_t g_heap_next = HEAP_LO;
static uint16_t g_dgroup;                  /* from `mov dx, DGROUP` at image 0 */
static uint16_t g_ail_seq;                 /* handles handed out by AIL 0x704 */

/* Vectors the guest installs. Recorded so a spin waiting on an ISR-updated
 * flag can be told apart from one that is simply looping. */
static uint16_t g_vec_seg[256], g_vec_off[256];
unsigned long g_port_reads[8];   /* 3DA, 3C5, 3CF, 3C9, other */

/* Planar VGA.
 *
 * DinoPark does not use plain mode 13h. fn_09054 sets 13h and then unchains
 * it: chain-4 off at Sequencer 4, Graphics 5 and 6 cleared, Map Mask 0x0F,
 * CRTC 0x14/0x17 fixed up. That is Mode X, where one byte address covers four
 * pixels -- one in each plane -- and the Map Mask decides which planes a write
 * lands in. Storing it flat collapses all four onto the same byte, which is
 * why an unchained screen renders as nothing recognisable.
 *
 * Chained (13h) writes stay in cpu->mem so the flat path keeps working; only
 * unchained ones are split out into planes.
 */
#define VGA_LOW  0xA0000u
#define VGA_HIGH 0xB0000u
static uint8_t g_plane[4][0x10000];
static int g_chain4 = 1;                   /* mode 13h until told otherwise */
static uint8_t g_read_plane;               /* Graphics reg 4: read map select */
static unsigned long g_plane_writes[4];    /* writes, not occupancy: an even
                                            * blit touches each plane equally,
                                            * and overwrites hide in occupancy */
static unsigned long g_mask_writes[16];    /* by Map Mask low nibble */
static uint8_t g_blit_cs14_lo, g_blit_cs14_hi;

/* Watch the real walker instead of reimplementing it.
 *
 * fn_2F0CD reads the size word at ES:[2] of each block it visits, so every
 * read of a linear address ending in 2 inside the heap names a block it
 * stepped to -- in the order it actually stepped, coalescing branch included.
 * A ring of the last 64 is enough to show the cycle it is stuck in, and costs
 * nothing when DINO_HEAPTRACE is unset. */
#define HSEQ 64
static uint16_t g_hseq[HSEQ];
static unsigned g_hseq_n;
static unsigned long g_hseq_hits;
static int g_heaptrace = -1;

static void heap_note(uint32_t addr)
{
    if (g_heaptrace < 0) g_heaptrace = getenv("DINO_HEAPTRACE") ? 1 : 0;
    if (!g_heaptrace || (addr & 0xF) != 2) return;
    if (addr < (uint32_t)HEAP_LO * 16 || addr >= (uint32_t)HEAP_HI * 16) return;
    g_hseq[g_hseq_n++ % HSEQ] = (uint16_t)((addr - 2) / 16);
    g_hseq_hits++;
}

/* The block header at 6C47 ends up holding F5F5, and memory starts zeroed, so
 * something wrote it. Record the span of 0xF5 writes landing in the heap: if a
 * drawing routine is running off the end of its buffer, the run shows up as a
 * contiguous range ending on top of a header. */
static uint32_t g_f5_lo = 0xFFFFFFFFu, g_f5_hi;
static unsigned long g_f5_n;

/* Attribution. Knowing that something wrote a byte is not the same as knowing
 * what did; DINO_WATCH=<linear hex> reports the first write to one address
 * together with the live call stack, which under -DDINO_SPCHECK names the
 * routine responsible. */
static uint32_t g_watch = 0xFFFFFFFFu;
static int g_watch_fired;

static unsigned long host_ms(void);
/* Milliseconds since the run started. host_ms() is the host's uptime, which is
 * fine for pacing a delta but useless as a threshold: every "after N ms" test
 * written against it is true on the first call. */
static unsigned long host_elapsed_ms(void);

static void watch_note(uint32_t addr, uint8_t val)
{
    static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("DINO_WATCH");
        if (e) g_watch = (uint32_t)strtoul(e, NULL, 16);
    }
    if (addr != g_watch || g_watch_fired >= 8) return;
    /* DINO_WATCH_AFTER=<ms>: ignore writes before then. A stack address is
     * written constantly, so the first eight are whatever ran during startup
     * and the interesting ones -- the read that should have landed there --
     * never get reported. */
    { static long after = -1;
      if (after < 0) { const char *e = getenv("DINO_WATCH_AFTER"); after = e ? atol(e) : 0; }
      if (after > 0 && (long)host_elapsed_ms() < after) return; }
    g_watch_fired++;
    fprintf(stderr, "[watch] %05X write #%d = %02X\n", addr, g_watch_fired, val);
#ifdef DINO_SPCHECK
    /* Every fire, not just the first: the first write to an address is often
     * innocent and the one that matters comes later. */
    {
        extern unsigned g_stk[]; extern int g_stk_depth;
        for (int i = 0; i < g_stk_depth && i < 40; i++)
            fprintf(stderr, "[watch]     fn_%05X\n", g_stk[i]);
    }
#endif
}

/* DINO_STATEWORD=<off>[,<off>...]: report game variables whenever they change.
 *
 * fn_0CDDB is the top-level state machine -- `mov bx,[3C5C] / cmp bx,0x14 /
 * jmp cs:[bx+8AB]`, twenty-one states through a jump table -- so watching that
 * one word says which screen the game thinks it is on. A list matters when the
 * question is which of several candidates a variable is: scan a DGROUP dump for
 * a value you know the game holds (the bank hands over $5,000), then watch
 * everything that matched and see which one behaves like money.
 *
 * Reported on change rather than on write, and reporting the value the write
 * PRODUCES rather than the one in memory -- the hook runs before the byte
 * lands, so reading it back shows every 16-bit store a byte behind and each
 * transition arrives looking like a half-value. */
#define SW_MAX 8
static uint16_t g_sw_off[SW_MAX], g_sw_last[SW_MAX];
static int g_sw_count = -1, g_sw_n;

static void stateword_note(CPU *cpu, uint32_t addr, uint8_t val)
{
    if (g_sw_count < 0) {
        g_sw_count = 0;
        const char *e = getenv("DINO_STATEWORD");
        while (e && *e && g_sw_count < SW_MAX) {
            g_sw_last[g_sw_count] = 0xFFFF;
            g_sw_off[g_sw_count++] = (uint16_t)strtoul(e, NULL, 16);
            const char *c = strchr(e, ',');
            if (!c) break;
            e = c + 1;
        }
    }
    if (!g_sw_count || !g_dgroup || g_sw_n > 200000) return;

    for (int i = 0; i < g_sw_count; i++) {
        uint32_t want = seg_off(g_dgroup, g_sw_off[i]);
        if (addr != want && addr != want + 1) continue;
        uint16_t v = mem_read16(cpu, g_dgroup, g_sw_off[i]);
        v = (addr == want) ? (uint16_t)((v & 0xFF00) | val)
                           : (uint16_t)((v & 0x00FF) | (val << 8));
        if (v == g_sw_last[i]) return;
        g_sw_last[i] = v;
        g_sw_n++;
        fprintf(stderr, "[state] %04X = %04X (%u)", g_sw_off[i], v, v);
#ifdef DINO_SPCHECK
        { extern unsigned g_stk[]; extern int g_stk_depth;
          for (int k = g_stk_depth - 3; k < g_stk_depth; k++)
              if (k >= 0) fprintf(stderr, "  <- fn_%05X", g_stk[k]); }
#endif
        fprintf(stderr, "\n");
        return;
    }
}

/* Every write to a block's size field, in order. Reads tell us the walk the
 * game performs; writes tell us the chain it believes it built, and the two
 * diverging is the whole question. Only the low byte is recorded, which is
 * enough to see which paragraphs were ever treated as block headers. */
#define HWR 48
static struct { uint16_t seg; uint8_t val; } g_hwr[HWR];
static unsigned g_hwr_n;

/* The chain invariant: walking from ds:[742A] by size must land exactly on
 * ds:[742E]. Check it after every write to a size field and report the first
 * write that breaks it -- with the call stack, which names the routine. Cheap,
 * the chain being a couple of dozen blocks, and it answers the question
 * directly rather than inferring it from a snapshot taken long afterwards. */
/* The chain is legitimately inconsistent *while* the manager edits it -- a
 * 16-bit store arrives as two bytes, and headers get cleared before being
 * filled. So the first break means nothing; the one that matters is the last
 * write after which it never became consistent again. Record that, with the
 * call stack as it stood, and report it at exit. */
static uint32_t g_break_addr; static uint8_t g_break_val;
static uint16_t g_break_stop, g_break_first, g_break_last;
static unsigned g_break_stk[40]; static int g_break_depth; static int g_break_seen;

static void chain_check(CPU *cpu, uint32_t addr, uint8_t val)
{
    if (g_heaptrace <= 0 || !g_dgroup) return;
    /* Only after a write to a block's size field, and never while the heap's
     * own control words are being stored: a 16-bit store arrives here as two
     * byte writes, and checking between them reads a half-updated pointer. */
    if ((addr & 0xF) != 2 && (addr & 0xF) != 3) return;
    if (addr < (uint32_t)HEAP_LO * 16 || addr >= (uint32_t)HEAP_HI * 16) return;
    uint16_t first = mem_read16(cpu, g_dgroup, 0x742A);
    uint16_t last  = mem_read16(cpu, g_dgroup, 0x742E);
    if (!first || !last || first == last) return;

    uint16_t seg = first;
    for (int n = 0; n < 4096; n++) {
        if (seg == last) return;                       /* still consistent */
        if (seg < HEAP_LO || seg >= HEAP_HI) break;
        uint16_t size = mem_read16(cpu, seg, 2);
        if (!size) break;
        uint16_t next = (uint16_t)(seg + size);
        if (next <= seg) break;                        /* wrapped */
        seg = next;
    }

    g_break_seen = 1;
    g_break_addr = addr; g_break_val = val; g_break_stop = seg;
    g_break_first = first; g_break_last = last;
#ifdef DINO_SPCHECK
    { extern unsigned g_stk[]; extern int g_stk_depth;
      g_break_depth = g_stk_depth < 40 ? g_stk_depth : 40;
      for (int i = 0; i < g_break_depth; i++) g_break_stk[i] = g_stk[i]; }
#endif
}

void chain_break_dump(void)
{
    if (!g_break_seen) {
        fprintf(stderr, "[chain] the block chain was consistent throughout\n");
        return;
    }
    fprintf(stderr, "[chain] last write leaving it broken: %05X = %02X; walk stops at %04X (first=%04X last=%04X)\n",
            g_break_addr, g_break_val, g_break_stop, g_break_first, g_break_last);
    for (int i = 0; i < g_break_depth; i++)
        fprintf(stderr, "[chain]     fn_%05X\n", g_break_stk[i]);
}

static void hdr_write_note(uint32_t addr, uint8_t val)
{
    if (g_heaptrace <= 0 || (addr & 0xF) != 2) return;
    if (addr < (uint32_t)HEAP_LO * 16 || addr >= (uint32_t)HEAP_HI * 16) return;
    g_hwr[g_hwr_n++ % HWR].seg = (uint16_t)((addr - 2) / 16);
    g_hwr[(g_hwr_n - 1) % HWR].val = val;
}

void hdr_write_dump(void)
{
    if (g_heaptrace <= 0 || !g_hwr_n) return;
    unsigned n = g_hwr_n < HWR ? g_hwr_n : HWR;
    unsigned start = g_hwr_n < HWR ? 0 : g_hwr_n % HWR;
    fprintf(stderr, "[hdrw] %u writes to +2 fields; last %u:\n", g_hwr_n, n);
    fprintf(stderr, "[hdrw]  ");
    for (unsigned i = 0; i < n; i++)
        fprintf(stderr, "%04X=%02X ", g_hwr[(start + i) % HWR].seg, g_hwr[(start + i) % HWR].val);
    fprintf(stderr, "\n");
}

static void f5_note(uint32_t addr, uint8_t val)
{
    if (g_heaptrace < 0) g_heaptrace = getenv("DINO_HEAPTRACE") ? 1 : 0;
    if (!g_heaptrace || val != 0xF5) return;
    if (addr < (uint32_t)HEAP_LO * 16 || addr >= (uint32_t)HEAP_HI * 16) return;
    if (addr < g_f5_lo) g_f5_lo = addr;
    if (addr > g_f5_hi) g_f5_hi = addr;
    g_f5_n++;
}

/* Where does the drawing actually go?
 *
 * The game writes only a few hundred pixels to the VGA window but loads whole
 * screens, so it must be composing somewhere else. A histogram of writes by 4 KB
 * page finds that buffer without guessing what a framebuffer looks like -- the
 * hottest page outside the card is where the picture is being built. */
static unsigned long g_page_writes[256];

void write_histogram(void)
{
    int top[8];
    for (int n = 0; n < 8; n++) {
        int best = -1;
        for (int i = 0; i < 256; i++) {
            int dup = 0;
            for (int k = 0; k < n; k++) if (top[k] == i) dup = 1;
            if (dup) continue;
            if (best < 0 || g_page_writes[i] > g_page_writes[best]) best = i;
        }
        top[n] = best;
        if (g_page_writes[best] == 0) break;
        fprintf(stderr, "[hot] %05X..%05X : %lu writes%s\n",
                (unsigned)best * 0x1000, (unsigned)best * 0x1000 + 0xFFF,
                g_page_writes[best],
                (best >= 0xA0 && best < 0xB0) ? "   (the VGA window)" : "");
    }
}

/* Is anything ever freed?
 *
 * The heap fills and the allocator spins, but a full heap after five intro
 * screens is only a bug if the game meant to hand those screens back. Walking
 * the block chain and totalling used against free answers it; printing that
 * once per file the game opens turns it into a series, and a leak shows up as
 * free paragraphs falling monotonically screen after screen.
 *
 * Counting writes to the flag byte instead does not work: a block header's +8
 * is one byte in sixteen, so a blitted image writes thousands of them. */
void heap_summary(CPU *cpu, const char *why)
{
    if (!g_dgroup) return;
    uint16_t first = mem_read16(cpu, g_dgroup, 0x742A);
    uint16_t last  = mem_read16(cpu, g_dgroup, 0x742E);
    if (!first || !last) return;
    unsigned used = 0, freep = 0, nblk = 0, nfree = 0, biggest = 0;
    uint16_t seg = first;
    for (; nblk < 8192 && seg != last; nblk++) {
        if (seg < HEAP_LO || seg >= HEAP_HI) break;
        uint16_t size = mem_read16(cpu, seg, 2);
        if (!size) break;
        if (cpu->mem[seg_off(seg, 0) + 8] & 0x80) { used += size; }
        else { freep += size; nfree++; if (size > biggest) biggest = size; }
        seg = (uint16_t)(seg + size);
    }
    fprintf(stderr, "[heapsum] %-14s %u blocks, used %uK, free %uK in %u blocks "
                    "(largest %uK)\n", why, nblk, used / 64, freep / 64, nfree,
            biggest / 64);
}

int recomp_mem_write8(CPU *cpu, uint32_t addr, uint8_t val)
{
    /* remember the blitter's mode word as it is written */
    if (addr == 0x091D0u + 0x14) g_blit_cs14_lo = val;
    if (addr == 0x091D0u + 0x15) g_blit_cs14_hi = val;
    g_page_writes[(addr >> 12) & 0xFF]++;
    watch_note(addr, val);
    hdr_write_note(addr, val);
    chain_check(cpu, addr, val);
    f5_note(addr, val);
    stateword_note(cpu, addr, val);
    if (addr < VGA_LOW || addr >= VGA_HIGH || g_chain4) return 0;
    uint32_t off = addr - VGA_LOW;
    /* Sample on drawing activity, not on a wall clock. An 18.2 Hz timer is far
     * too coarse for a sequence that composes a screen and moves on; tying it
     * to VGA traffic means the richest moment gets seen whenever it happens. */
    { static unsigned long n; extern void vga_sample(CPU *);
      if ((++n & 0x7FF) == 0) vga_sample(cpu); }
    /* DINO_MASKRESET=<ms>: zero the plane tallies once, at that point in the
     * run, so the report at exit covers only what was drawn after it. Totals
     * over a whole session say nothing about one screen. */
    { static int done; static long at = -1;
      if (at < 0) { const char *e = getenv("DINO_MASKRESET"); at = e ? atol(e) : 0; }
      if (at > 0 && !done && (long)host_elapsed_ms() >= at) {
          done = 1;
          memset(g_mask_writes, 0, sizeof g_mask_writes);
          memset(g_plane_writes, 0, sizeof g_plane_writes);
          fprintf(stderr, "[plane] tallies reset\n");
      } }
    g_mask_writes[g_map_mask & 0xF]++;
    for (int p = 0; p < 4; p++)
        if (g_map_mask & (1u << p)) { g_plane[p][off] = val; g_plane_writes[p]++; }
    return 1;
}

/* Where did an asset actually land? The container signature is the anchor:
 * finding it says where the loader's buffer really starts, which is the one
 * thing the block chain cannot tell us once it has walked into the data. */
/* Look for a block header carrying a particular size, anywhere in the heap.
 * If a split shrank a block but its remainder header never appeared where the
 * chain expects it, this says whether the header was written somewhere else
 * (a wrong ES) or never written at all. */
void find_block_size(CPU *cpu, uint16_t want)
{
    unsigned hits = 0;
    for (uint32_t seg = HEAP_LO; seg < HEAP_HI && hits < 8; seg++) {
        uint32_t a = (uint32_t)seg * 16;
        uint16_t size = (uint16_t)(cpu->mem[a + 2] | (cpu->mem[a + 3] << 8));
        if (size != want) continue;
        hits++;
        fprintf(stderr, "[blk] size %04X at segment %04X (prev=%04X flags=%02X)\n",
                want, (unsigned)seg,
                (unsigned)(cpu->mem[a] | (cpu->mem[a + 1] << 8)), cpu->mem[a + 8]);
    }
    if (!hits)
        fprintf(stderr, "[blk] no block anywhere in the heap has size %04X\n", want);
}

void find_signature(CPU *cpu, const char *sig)
{
    size_t n = strlen(sig), hits = 0;
    for (uint32_t a = 0x400; a + n < 0xA0000u && hits < 8; a++) {
        if (memcmp(&cpu->mem[a], sig, n)) continue;
        hits++;
        fprintf(stderr, "[sig] \"%s\" at %05X (segment %04X:%04X)\n",
                sig, a, (unsigned)(a / 16), (unsigned)(a % 16));
    }
    if (!hits)
        fprintf(stderr, "[sig] \"%s\" not found in guest memory\n", sig);
}

void heap_trace_dump(void)
{
    if (g_heaptrace <= 0 || !g_hseq_hits) return;
    fprintf(stderr, "[walk] %lu block-size reads; last %u visited, oldest first:\n",
            g_hseq_hits, g_hseq_n < HSEQ ? g_hseq_n : HSEQ);
    unsigned n = g_hseq_n < HSEQ ? g_hseq_n : HSEQ;
    unsigned start = g_hseq_n < HSEQ ? 0 : g_hseq_n % HSEQ;
    fprintf(stderr, "[walk]  ");
    for (unsigned i = 0; i < n; i++)
        fprintf(stderr, "%04X ", g_hseq[(start + i) % HSEQ]);
    fprintf(stderr, "\n");
    if (g_f5_n)
        fprintf(stderr, "[walk] %lu 0xF5 writes into the heap, %05X..%05X (segments %04X..%04X)\n",
                g_f5_n, g_f5_lo, g_f5_hi, (unsigned)(g_f5_lo / 16), (unsigned)(g_f5_hi / 16));
}

int recomp_mem_read8(CPU *cpu, uint32_t addr, uint8_t *out)
{
    heap_note(addr);
    if (addr < VGA_LOW || addr >= VGA_HIGH || g_chain4) return 0;
    *out = g_plane[g_read_plane & 3][addr - VGA_LOW];
    return 1;
}

static int g_vga_live;
static uint8_t g_video_mode = 0x03;

static unsigned long host_elapsed_ms(void)
{
    static unsigned long t0;
    unsigned long now = host_ms();
    if (!t0) t0 = now;
    return now - t0;
}

/* Wall clock in milliseconds, for pacing things the guest should not set the
 * rate of. */
static unsigned long host_ms(void)
{
#ifdef _WIN32
    return (unsigned long)GetTickCount64();
#else
    return (unsigned long)(clock() * 1000ULL / CLOCKS_PER_SEC);
#endif
}

unsigned long vga_ticks(void)
{
#ifdef _WIN32
    return (unsigned long)((GetTickCount64() * 1193ULL) / 65536ULL);
#else
    return (unsigned long)(clock() / (CLOCKS_PER_SEC / 18));
#endif
}

/* Compose what is actually on screen into a linear 320x200 buffer.
 *
 * Chained, that is just the VGA window. Unchained, pixel (x,y) lives in plane
 * x&3 at offset y*80 + x/4 -- four planes of 80 bytes per row rather than one
 * run of 320. */
static uint8_t g_compose[VGA_W * VGA_H];

static const uint8_t *vga_frame(CPU *cpu)
{
    if (g_chain4) return &cpu->mem[VGA_BASE];
    for (int y = 0; y < VGA_H; y++)
        for (int x = 0; x < VGA_W; x++)
            g_compose[y * VGA_W + x] = g_plane[x & 3][y * (VGA_W / 4) + (x >> 2)];
    return g_compose;
}

/* Keep the most picture-like frame the game ever shows.
 *
 * Sampling only at exit catches whatever the last clear left behind -- a solid
 * fill, in practice. A frame is worth keeping if it uses a lot of distinct
 * indices, which a cleared screen does not, so track the best seen and write
 * that at the end alongside the palette in force. */
static uint8_t g_best_frame[VGA_W * VGA_H];
static uint8_t g_best_pal[768];
static int g_best_distinct;

void vga_sample(CPU *cpu)
{
    if (!cpu || !cpu->mem) return;
    const uint8_t *f = vga_frame(cpu);
    int seen[256]; memset(seen, 0, sizeof seen);
    int distinct = 0;
    for (int i = 0; i < VGA_W * VGA_H; i += 3)
        if (!seen[f[i]]) { seen[f[i]] = 1; distinct++; }
    if (distinct <= g_best_distinct) return;
    g_best_distinct = distinct;
    memcpy(g_best_frame, f, sizeof g_best_frame);
    memcpy(g_best_pal, g_palette, sizeof g_best_pal);
}

/* Per-plane occupancy. In an unchained mode a 320-wide row is four planes of
 * 80 bytes, and the blitter makes one pass per plane with the Map Mask set. If
 * the passes are not landing where they should, the giveaway is the planes
 * holding wildly different amounts -- or only one holding anything at all. */
void vga_plane_report(void)
{
    for (int p = 0; p < 4; p++) {
        long nz = 0, distinct = 0;
        int seen[256]; memset(seen, 0, sizeof seen);
        for (uint32_t i = 0; i < (uint32_t)(VGA_W / 4) * VGA_H; i++) {
            uint8_t v = g_plane[p][i];
            if (v && v != 0xFF) nz++;
            if (!seen[v]) { seen[v] = 1; distinct++; }
        }
        fprintf(stderr, "[plane] %d: %ld set, %ld distinct, %lu writes\n",
                p, nz, distinct, g_plane_writes[p]);
    }
    /* cs:[0x14] in the blitter's segment decides whether it duplicates the
     * plane mask into the high nibble; without that the mask walks through
     * four values that select nothing. */
    fprintf(stderr, "[plane] blitter cs:[0x14] = %04X\n",
            (unsigned)(g_blit_cs14_lo | (g_blit_cs14_hi << 8)));
    fprintf(stderr, "[plane] map mask now %02X, read plane %d, %s\n",
            g_map_mask, g_read_plane, g_chain4 ? "chained" : "unchained");
    for (int m = 0; m < 16; m++)
        if (g_mask_writes[m])
            fprintf(stderr, "[plane] mask %X: %lu writes%s\n", m, g_mask_writes[m],
                    m ? "" : "   <-- selects no plane; these are dropped");
}

void vga_best_dump(const char *path)
{
    if (g_best_distinct < 2) {
        fprintf(stderr, "[best] no frame with any variety was ever shown\n");
        return;
    }
    fprintf(stderr, "[best] richest frame seen used %d distinct indices -> %s\n",
            g_best_distinct, path);
    vga_write_bmp(path, g_best_frame, VGA_W, VGA_H, g_best_pal);
    FILE *r = fopen("work/best_frame.raw", "wb");
    if (r) { fwrite(g_best_frame, 1, sizeof g_best_frame, r); fclose(r); }
    r = fopen("work/best_frame.pal", "wb");
    if (r) { fwrite(g_best_pal, 1, sizeof g_best_pal, r); fclose(r); }
}

/* The composed screen, for harnesses that want to check where pixels landed. */
const uint8_t *vga_compose_frame(CPU *cpu) { return vga_frame(cpu); }

/* DINO_FILM=<ms>: write work/film_NNN.bmp every so often, so a run that nobody
 * is watching still leaves behind the sequence of screens it went through.
 * The single best frame says what the game can draw; a strip says where it
 * got to. */
static void film_note(CPU *cpu)
{
    static int period = -1;
    static unsigned long last;
    static int n;
    if (period < 0) {
        const char *e = getenv("DINO_FILM");
        period = e ? atoi(e) : 0;
    }
    if (period <= 0 || n >= 60) return;
    unsigned long now = host_ms();
    if (n && now - last < (unsigned long)period) return;
    last = now;
    char path[64];
    snprintf(path, sizeof path, "work/film_%03d.bmp", n++);
    vga_write_bmp(path, vga_frame(cpu), VGA_W, VGA_H, (const uint8_t *)g_palette);
}

void vga_flush(CPU *cpu)
{
    if (!g_vga_live) return;
    film_note(cpu);
    vga_window_present(vga_frame(cpu), (const uint8_t *)g_palette);
}

/* At most one frame per 16ms, from anywhere.
 *
 * The retrace poll used to be the only place this happened, on the reasoning
 * that a game pacing itself off the beam is telling you when it wants a frame.
 * It is -- during the intro. The main loop never touches 3DA at all, so once
 * the intro was over the window stopped updating entirely and the park was
 * only ever seen in the exit snapshot. Every software interrupt goes through
 * int_handler and the main loop polls the mouse through it constantly, so
 * calling it from both places covers the whole run. */
/* A screen that stops changing, reported once with whoever is on the stack.
 *
 * A long run that goes quiet is the hardest thing to diagnose after the fact:
 * the film shows the last frame and nothing about why it is the last one. This
 * says when the picture stopped moving and what was running at the time. Thirty
 * seconds, because a player reading a dialog is not a stall. */
#define STALL_MS 30000

static void stall_note(CPU *cpu, const uint8_t *frame)
{
    static unsigned long hash, since;
    static int said;
    unsigned long h = 5381;
    for (unsigned i = 0; i < VGA_W * VGA_H; i += 37) h = h * 33 + frame[i];
    unsigned long now = host_ms();
    if (h != hash) { hash = h; since = now; said = 0; return; }
    if (said || !since || now - since < STALL_MS) return;
    said = 1;
    fprintf(stderr, "[stall] the screen has not changed in %lus\n",
            (now - since) / 1000);
#ifdef DINO_SPCHECK
    { extern unsigned g_stk[]; extern int g_stk_depth;
      for (int i = 0; i < g_stk_depth && i < 12; i++)
          fprintf(stderr, "[stall]     fn_%05X\n", g_stk[i]); }
#endif
}

void vga_flush_paced(CPU *cpu)
{
    static unsigned long last;
    if (!g_vga_live) return;
    unsigned long now = host_ms();
    if (now - last < 16) return;
    last = now;
    stall_note(cpu, vga_frame(cpu));
    vga_flush(cpu);
}

/* The 80x25 text screen at B800:0000. DOS programs put their messages there
 * directly rather than through DOS, so an error the game is showing the user
 * is otherwise invisible to us. Printed only if something is actually on it. */
/* The game formats its errors into a stack buffer and hands them to a printer
 * we do not fully model, so they never reach a screen we can read. The bytes
 * are still there at exit: sweep the stack segment for one and show it. */
void stack_message_scan(CPU *cpu)
{
    uint32_t base = seg_off(cpu->ss, 0);
    for (uint32_t i = 0; i < 0x2000; i++) {
        const char *s = (const char *)&cpu->mem[base + i];
        int n = 0;
        while (n < 80 && s[n] >= 32 && s[n] < 127) n++;
        if (n >= 12 && s[n] == 0) {
            fprintf(stderr, "[stack] %s\n", s);
            i += n;
        }
    }
}

void text_snapshot(CPU *cpu)
{
    uint32_t base = 0xB8000;
    int any = 0;
    for (int i = 0; i < 80 * 25 * 2; i += 2) {
        uint8_t c = cpu->mem[base + i];
        if (c && c != ' ') { any = 1; break; }
    }
    if (!any) return;
    fprintf(stderr, "---- text screen ----\n");
    for (int y = 0; y < 25; y++) {
        char line[81];
        int last = -1;
        for (int x = 0; x < 80; x++) {
            uint8_t c = cpu->mem[base + (y * 80 + x) * 2];
            line[x] = (c >= 32 && c < 127) ? (char)c : ' ';
            if (line[x] != ' ') last = x;
        }
        line[last + 1] = 0;
        if (last >= 0) fprintf(stderr, "| %s\n", line);
    }
    fprintf(stderr, "---------------------\n");
}

/* Hunt for an offscreen framebuffer.
 *
 * A DOS game of this era usually draws into a RAM buffer and blits it to the
 * card at frame end, so an empty A000 does not mean nothing was drawn -- civ
 * had exactly this, and finding its offscreen buffer is what finally made its
 * title screen visible. Score every 320x200-sized window by how much it looks
 * like a picture: many distinct byte values, and neighbouring pixels usually
 * but not always equal. Flat fills and code both score badly.
 */
/* DinoPark's own heap, roughly as its walker sees it.
 *
 * fn_2F0CD chains blocks with `next = current + [es:2]`, bounded by the words at
 * DGROUP:742A and DGROUP:742E, and that is what this reproduces. Be careful
 * reading the tail: the real walker only advances that simply when the flag byte
 * at [es:8] has bit 7 set. With it clear it branches into a coalescing path, so
 * this dump stops being faithful at the first flags=00 block and anything it
 * prints after that is its own confusion, not evidence of corruption.
 *
 * Useful for what it does show: the bounds, and that the first ~20 blocks chain
 * cleanly with sensible sizes. */
void heap_dump(CPU *cpu)
{
    if (!g_dgroup) return;
    uint16_t first = mem_read16(cpu, g_dgroup, 0x742A);
    uint16_t last  = mem_read16(cpu, g_dgroup, 0x742E);
    fprintf(stderr, "[heap] first=%04X last=%04X (we handed out %04X..%04X)\n",
            first, last, (unsigned)HEAP_LO, (unsigned)HEAP_HI);
    uint16_t seg = first;
    int n = 0;
    const char *verdict = "ran past the block cap";
    for (; n < 8192; n++) {
        if (seg == last) { verdict = "reached the end marker"; break; }
        uint32_t a = seg_off(seg, 0);
        if (a + 16 >= 0x100000u) { verdict = "walked off the top of memory"; break; }
        uint16_t size = mem_read16(cpu, seg, 0x2);
        if (!size) { verdict = "hit a zero-size block"; break; }
        if (n < 40)
            fprintf(stderr, "[heap]   blk %2d @%04X size=%04X flags=%02X\n",
                    n, seg, size, cpu->mem[a + 8]);
        seg = (uint16_t)(seg + size);
    }
    fprintf(stderr, "[heap] %d blocks, stopped at %04X: %s\n", n, seg, verdict);
}

void fb_scan(CPU *cpu, const char *path)
{
    const uint32_t SIZE = 320u * 200u;
    uint32_t best_at = 0;
    long best_score = 0;

    for (uint32_t base = 0x1000; base + SIZE < 0xA0000u; base += 0x400) {
        int seen[256]; memset(seen, 0, sizeof seen);
        long same = 0, distinct = 0;
        for (uint32_t i = 0; i < SIZE; i += 4) {          /* sample, for speed */
            uint8_t v = cpu->mem[base + i];
            if (!seen[v]) { seen[v] = 1; distinct++; }
            if (i && v == cpu->mem[base + i - 4]) same++;
        }
        long runs = same * 100 / (SIZE / 4);              /* percent flat */
        if (distinct < 16 || runs > 96 || runs < 10) continue;
        long score = distinct * runs;
        if (score > best_score) { best_score = score; best_at = base; }
    }

    if (!best_score) {
        fprintf(stderr, "[fb] no picture-shaped buffer found\n");
        return;
    }
    fprintf(stderr, "[fb] best candidate at %05X (score %ld) -> %s\n",
            best_at, best_score, path);
    /* Also dump the raw indices and the DAC. If the game has not programmed a
     * palette yet, every index maps to black and the BMP looks empty even when
     * there is a picture sitting right there. */
    { FILE *r = fopen("work/fb_scan.raw", "wb");
      if (r) { fwrite(&cpu->mem[best_at], 1, 320 * 200, r); fclose(r); } }
    { FILE *r = fopen("work/fb_scan.pal", "wb");
      if (r) { fwrite(g_palette, 1, sizeof g_palette, r); fclose(r); } }
    { int pal_set = 0;
      for (size_t i = 0; i < sizeof g_palette; i++) if (((uint8_t *)g_palette)[i]) { pal_set = 1; break; }
      fprintf(stderr, "[fb] DAC palette %s\n", pal_set ? "programmed" : "still all zero"); }
    vga_write_bmp(path, &cpu->mem[best_at], 320, 200, (const uint8_t *)g_palette);
}

void vga_snapshot(CPU *cpu, const char *path)
{
    /* Raw indices too: with an unprogrammed DAC every index maps to black, so
     * the BMP alone cannot tell `nothing drawn` from `drawn, no palette yet`. */
    { const uint8_t *f = vga_frame(cpu);
      long nonzero = 0;
      for (int i = 0; i < VGA_W * VGA_H; i++) if (f[i]) nonzero++;
      fprintf(stderr, "[vga] %ld of %d pixels non-zero (%s)\n",
              nonzero, VGA_W * VGA_H, g_chain4 ? "chained" : "Mode X");
      FILE *r = fopen("work/vga_exit.raw", "wb");
      if (r) { fwrite(f, 1, VGA_W * VGA_H, r); fclose(r); } }
    vga_write_bmp(path, vga_frame(cpu), VGA_W, VGA_H,
                  (const uint8_t *)g_palette);
}

/* 8253 timer, channel 0. The game latches it with `out 0x43, 0` and reads the
 * counter low byte then high byte from port 0x40, and it uses the difference
 * between two reads as a high-resolution clock -- so a constant reply is an
 * infinite wait, which is where the boot sat spinning 591 million times. The
 * real counter runs down at 1.193182 MHz, and that is a fast enough clock
 * that the host's performance counter is the honest source for it. */
static uint16_t g_pit_latch;
static int g_pit_hi;

static uint16_t pit_counter(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    unsigned long long t = (unsigned long long)
        ((double)c.QuadPart * 1193182.0 / (double)f.QuadPart);
#else
    unsigned long long t = (unsigned long long)clock() * 1193182ULL / CLOCKS_PER_SEC;
#endif
    return (uint16_t)(0xFFFFu - (uint16_t)(t & 0xFFFFu));   /* counts down */
}

uint8_t port_in8(CPU *cpu, uint16_t port) {
    switch (port) {
        case 0x3DA: {                    /* CRT status: toggle retrace */
            /* The game polls this to pace itself, which makes it the natural
             * place to put a frame on screen. Throttled: the poll spins far
             * faster than anything needs redrawing. */
            static uint8_t t; static unsigned long last_ms;
            g_port_reads[0]++;
            /* Pace on a clock, not a poll count. One flush per 1024 polls came
             * out at about a frame a second, which is unplayable; how often the
             * game asks says nothing about how often the screen should change. */
            vga_flush_paced(cpu);
            (void)last_ms;
            t ^= 0x09; return t;
        }
        case 0x40:                       /* PIT counter 0, low byte then high */
            g_pit_hi = !g_pit_hi;
            return g_pit_hi ? (uint8_t)g_pit_latch : (uint8_t)(g_pit_latch >> 8);
        case 0x3C5: return g_map_mask;
        case 0x3CF: return 0;
        case 0x3C9: return 0x3F;
        default: g_port_reads[1]++; return 0xFF;
    }
}

void port_out8(CPU *cpu, uint16_t port, uint8_t val) {
    (void)cpu;
    /* DINO_PORTTRACE: the Sequencer traffic, in order. Deducing what the
     * blitter writes to the Map Mask from its instruction stream has been
     * unreliable; this just records it. */
    { static int t = -1;
      if (t < 0) t = getenv("DINO_PORTTRACE") ? 1 : 0;
      if (t && (port == 0x3C4 || port == 0x3C5))
          fprintf(stderr, "[port] %03X <- %02X%s\n", port, val,
                  port == 0x3C5 && g_seq_index == 2 ? "   (map mask)" : ""); }
    switch (port) {
        case 0x43:                       /* PIT control: latch counter 0 */
            if ((val & 0xC0) == 0x00) { g_pit_latch = pit_counter(); g_pit_hi = 0; }
            break;
        case 0x3C4: g_seq_index = val; break;
        case 0x3C5:
            if (g_seq_index == 2) g_map_mask = val;
            /* Sequencer 4 bit 3 is chain-4: set = mode 13h's linear view,
             * clear = unchained planes. */
            if (g_seq_index == 4) {
                int c4 = (val & 0x08) != 0;
                if (c4 != g_chain4) trace("[VGA] %s\n", c4 ? "chained (13h)" : "UNCHAINED (Mode X)");
                g_chain4 = c4;
            }
            break;
        case 0x3CE: g_gc_index = val; break;
        case 0x3CF: if (g_gc_index == 4) g_read_plane = val & 3; break;
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

/* A 16-bit port access is two consecutive 8-bit ones: AL to the port, AH to
 * port+1. Mode X sets the Map Mask with `mov ax,(mask<<8)|02; out dx,ax` on
 * port 3C4, so treating that as a byte write set the index and threw the mask
 * away -- the four planes drifted out of sync and the title screen came out
 * shredded into vertical stripes. */
uint16_t port_in16(CPU *cpu, uint16_t port)
{
    uint16_t lo = port_in8(cpu, port);
    return (uint16_t)(lo | (port_in8(cpu, (uint16_t)(port + 1)) << 8));
}

void port_out16(CPU *cpu, uint16_t port, uint16_t val)
{
    port_out8(cpu, port, (uint8_t)val);
    port_out8(cpu, (uint16_t)(port + 1), (uint8_t)(val >> 8));
}

/* Keyboard.
 *
 * The intro sits in a palette fade polling INT 16h AH=1 for a key to skip it,
 * so answering `no key` forever means the game never leaves its own title
 * sequence. Answering `yes` on every poll is just as wrong: the poll runs far
 * faster than a person types, and the game would consume hundreds of thousands
 * of keypresses -- civ hit precisely that and paced its input off the BIOS
 * tick, which is what this does.
 *
 * DINO_KEYS is a comma-separated scancode script, e.g. `39,39,1` for two
 * spaces then escape; it defaults to space, which is what the intro wants. The
 * ASCII byte is filled in for the few keys the game is likely to read that
 * way. */
static uint16_t g_key_script[64];
static int g_key_n, g_key_i = -1;
static uint16_t g_key_pending;
static unsigned long g_key_last_tick;

#define KEY_PERIOD 9                       /* ticks: about half a second */

static uint8_t scancode_ascii(uint8_t sc)
{
    switch (sc) {
        case 0x39: return ' ';
        case 0x1C: return 0x0D;            /* Enter */
        case 0x01: return 0x1B;            /* Escape */
        default:   return 0;
    }
}

static void key_init(void)
{
    if (g_key_i >= 0) return;
    g_key_i = 0;
    const char *s = getenv("DINO_KEYS");
    if (!s || !*s) {
        g_key_script[g_key_n++] = 0x39;    /* space, to advance the intro */
        return;
    }
    while (*s && g_key_n < 64) {
        g_key_script[g_key_n++] = (uint16_t)strtoul(s, (char **)&s, 16);
        while (*s == ',' || *s == ' ') s++;
    }
}

/* The game does not read keys from the BIOS. It installs its own INT 9 handler
 * (07AC:003E), which takes the scancode from port 0x60 and files it in a ring
 * of its own: sixteen bytes at DGROUP:0x148, tail at 0x144, head at 0x146, plus
 * a held-flag per scancode at 0x158 so a key queues once until it is released.
 * Its INT 16h reads exist only to drain the BIOS buffer and are discarded.
 *
 * So put the keys where it actually looks, doing the same bookkeeping the ISR
 * does -- which also means a press has to be followed by a release, or the
 * held flag blocks every repeat.
 */
/* The BIOS-buffer word for a scancode: scancode in AH, ASCII in AL. Only the
 * handful the game is likely to read that way; anything else comes through as
 * a scancode with no ASCII, which is what a function key looks like anyway. */
static uint16_t bios_key_word(uint8_t code)
{
    static const struct { uint8_t sc, ch; } map[] = {
        { 0x39, ' ' }, { 0x1C, '\r' }, { 0x01, 27 }, { 0x0E, 8 }, { 0x0F, '\t' },
        { 0x02, '1' }, { 0x03, '2' }, { 0x04, '3' }, { 0x05, '4' }, { 0x06, '5' },
        { 0x07, '6' }, { 0x08, '7' }, { 0x09, '8' }, { 0x0A, '9' }, { 0x0B, '0' },
        { 0x10, 'q' }, { 0x11, 'w' }, { 0x12, 'e' }, { 0x13, 'r' }, { 0x14, 't' },
        { 0x15, 'y' }, { 0x16, 'u' }, { 0x17, 'i' }, { 0x18, 'o' }, { 0x19, 'p' },
        { 0x1E, 'a' }, { 0x1F, 's' }, { 0x20, 'd' }, { 0x21, 'f' }, { 0x22, 'g' },
        { 0x23, 'h' }, { 0x24, 'j' }, { 0x25, 'k' }, { 0x26, 'l' },
        { 0x2C, 'z' }, { 0x2D, 'x' }, { 0x2E, 'c' }, { 0x2F, 'v' }, { 0x30, 'b' },
        { 0x31, 'n' }, { 0x32, 'm' },
    };
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
        if (map[i].sc == code) return (uint16_t)((code << 8) | map[i].ch);
    return (uint16_t)(code << 8);
}

static void key_inject(CPU *cpu, uint8_t sc)
{
    uint8_t code = sc & 0x7F;
    /* Both destinations. The game reads its own ring for gameplay, but it also
     * calls INT 16h -- nineteen thousand times in forty-five seconds during the
     * attract sequence -- and that had nothing to give it, because nothing ever
     * filled the pending word. */
    if (!(sc & 0x80)) g_key_pending = bios_key_word(code);
    if (sc & 0x80) {                            /* release */
        mem_write8(cpu, g_dgroup, (uint16_t)(0x158 + code), 0);
        return;
    }
    if (mem_read8(cpu, g_dgroup, (uint16_t)(0x158 + code))) return;   /* held */
    mem_write8(cpu, g_dgroup, (uint16_t)(0x158 + code), 1);

    uint16_t tail = mem_read16(cpu, g_dgroup, 0x144);
    uint16_t next = (uint16_t)((tail + 1) & 0xF);
    if (next == mem_read16(cpu, g_dgroup, 0x146)) return;             /* full */
    mem_write8(cpu, g_dgroup, (uint16_t)(0x148 + (tail & 0xF)), code);
    mem_write16(cpu, g_dgroup, 0x144, next);
}

/* Real keys as they arrive, then the script.
 *
 * The window hands over PC scancodes with 0x80 set on release, which is exactly
 * what key_inject wants, so a person at the keyboard is playing the game
 * directly. The script stays for runs with nobody there -- headless, or when
 * DINO_KEYS names one -- and is what walks the intro along under the harness. */
static void key_tick(CPU *cpu)
{
    key_init();
    if (!g_dgroup) return;

    for (int k; (k = vga_window_key()) >= 0; )
        key_inject(cpu, (uint8_t)k);

    static int use_script = -1;
    if (use_script < 0) use_script = getenv("DINO_KEYS") != NULL || !g_vga_live;
    if (!use_script || !g_key_n) return;

    unsigned long t = vga_ticks();
    if (t - g_key_last_tick < KEY_PERIOD) return;
    g_key_last_tick = t;

    static int releasing;
    uint8_t sc = (uint8_t)g_key_script[g_key_i];
    if (releasing) {
        key_inject(cpu, (uint8_t)(sc | 0x80));
        if (g_key_i + 1 < g_key_n) g_key_i++;   /* hold on the last one */
    } else {
        key_inject(cpu, sc);
        trace("[kbd] scancode %02X into the game\'s own queue\n", sc);
    }
    releasing = !releasing;
}

/* Handlers the guest installs, for the lifter.
 *
 * An address handed to DOS as an interrupt vector is an entry point, the same
 * way a call target is -- but it is computed at run time, so no static scan
 * finds it, and the analyzer, which goes by Borland prologues, does not either.
 * The game's INT 8 handler was one of these: dispatching to it missed, so the
 * timer never ran and for the game no time ever passed.
 *
 * Recorded separately from the dispatch misses. Those are any address the guest
 * jumped to and are not all entry points; forcing the whole accumulated set
 * lifted enough wrong ones that the game tripped its own stack check. */
#define MAXVEC 32
static unsigned g_noted_vec[MAXVEC];
static int g_noted_n;

static void note_vector(uint16_t seg, uint16_t off)
{
    unsigned a = ((unsigned)seg << 4) + off;
    if (!a) return;
    for (int i = 0; i < g_noted_n; i++) if (g_noted_vec[i] == a) return;
    if (g_noted_n < MAXVEC) g_noted_vec[g_noted_n++] = a;
}

void dump_vectors(const char *path)
{
    if (!g_noted_n) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < g_noted_n; i++) fprintf(f, "%05X\n", g_noted_vec[i]);
    fclose(f);
}


/* ---- DOS find-first / find-next ---------------------------------------- */
/*
 * The game enumerates files -- looking for saved parks, most likely -- with the
 * usual dance: AH=2F to remember the current DTA, AH=1A to point it at its own
 * buffer, AH=4E/4F to search, AH=1A again to put the old one back.
 *
 * None of it was implemented, and unhandled INT 21h calls report success. So
 * findnext answered "here is another file" every time and the caller looped
 * forever: five minutes of clicking at random froze the park screen exactly
 * there, in fn_0418B, which is findnext itself.
 *
 * DOS returns results through the DTA, 43 bytes the caller owns:
 *   +00..14  reserved for DOS -- the search state lives here, which is why
 *            findnext takes no arguments and why the DTA must not move
 *   +15      attribute   +16 time   +18 date   +1A size (32-bit)
 *   +1E      the name, ASCIIZ, 8.3
 *
 * Windows' own FindFirstFile takes DOS wildcards, so the matching is left to
 * it rather than written again here.
 */
static uint16_t g_dta_seg, g_dta_off;

#define MAXFIND 8
#ifdef _WIN32
static HANDLE g_find[MAXFIND];
#endif

static void dta_store(CPU *cpu, int slot, const char *name,
                      uint8_t attr, uint32_t size)
{
    uint32_t a = seg_off(g_dta_seg, g_dta_off);
    cpu->mem[a + 0x00] = (uint8_t)slot;           /* our search state */
    cpu->mem[a + 0x15] = attr;
    cpu->mem[a + 0x16] = 0; cpu->mem[a + 0x17] = 0;
    cpu->mem[a + 0x18] = 0x21; cpu->mem[a + 0x19] = 0x1A;   /* 1993-01-01 */
    for (int i = 0; i < 4; i++) cpu->mem[a + 0x1A + i] = (uint8_t)(size >> (8 * i));
    int i = 0;
    for (; i < 12 && name[i]; i++) {
        char c = name[i];
        cpu->mem[a + 0x1E + i] = (uint8_t)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    cpu->mem[a + 0x1E + i] = 0;
}

static void dos_fail(CPU *cpu, uint16_t err)
{
    cpu->ax = err;
    cpu->flags |= FLAG_CF;
}

static void find_first(CPU *cpu)
{
    char pat[256];
    read_fname(cpu, cpu->ds, cpu->dx, pat, sizeof pat);
    trace("[INT21] find first '%s'", pat);
#ifdef _WIN32
    int slot = -1;
    for (int i = 0; i < MAXFIND; i++) if (!g_find[i]) { slot = i; break; }
    if (slot < 0) { trace(" -> no slots"); trace("\n"); dos_fail(cpu, 18); return; }

    /* Same search path as an open: where we are, then the game directory. */
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        const char *base = pat;
        if (base[0] && base[1] == ':') base += 2;
        for (const char *p = base; *p; p++)
            if (*p == '\\' || *p == '/') base = p + 1;
        char alt[300];
        snprintf(alt, sizeof alt, "original/%s", base);
        h = FindFirstFileA(alt, &fd);
    }
    if (h == INVALID_HANDLE_VALUE) { trace(" -> none"); trace("\n"); dos_fail(cpu, 18); return; }

    g_find[slot] = h;
    const char *nm = fd.cAlternateFileName[0] ? fd.cAlternateFileName : fd.cFileName;
    dta_store(cpu, slot, nm, (uint8_t)(fd.dwFileAttributes & 0x27), fd.nFileSizeLow);
    trace(" -> '%s'", nm); trace("\n");
    cpu->ax = 0;
    cpu->flags &= ~FLAG_CF;
#else
    trace(" -> unsupported"); trace("\n");
    dos_fail(cpu, 18);
#endif
}

static void find_next(CPU *cpu)
{
#ifdef _WIN32
    int slot = cpu->mem[seg_off(g_dta_seg, g_dta_off)];
    if (slot < 0 || slot >= MAXFIND || !g_find[slot]) { dos_fail(cpu, 18); return; }
    WIN32_FIND_DATAA fd;
    if (!FindNextFileA(g_find[slot], &fd)) {
        FindClose(g_find[slot]);
        g_find[slot] = NULL;
        dos_fail(cpu, 18);                      /* no more files */
        return;
    }
    const char *nm = fd.cAlternateFileName[0] ? fd.cAlternateFileName : fd.cFileName;
    dta_store(cpu, slot, nm, (uint8_t)(fd.dwFileAttributes & 0x27), fd.nFileSizeLow);
    cpu->ax = 0;
    cpu->flags &= ~FLAG_CF;
#else
    dos_fail(cpu, 18);
#endif
}

/* Hand the registered sequence to the MIDI player.
 *
 * AIL function 0x704 registers a sequence, and its first far pointer (CX:BX) is
 * the loaded file: the trace showed `FORM....XDIR` there. So the game has
 * already done the loading, and all that is missing is somewhere for the notes
 * to go.
 *
 * Playback starts here, on registration, rather than on whichever call means
 * "start" -- the game registers and starts in one breath, and this way the
 * music does not depend on having identified the rest of a driver API that only
 * exists as twenty-five `mov ax,FN / int 66h` thunks.
 *
 * The game is never told anything is playing: AIL 0x689 keeps answering "no",
 * because the intro polls it and waits, and the run should not be paced by
 * whether the host has a synthesiser.
 */
static void ail_register(CPU *cpu)
{
    static uint8_t buf[64 * 1024];
    uint32_t base = seg_off(cpu->cx, cpu->bx);
    if (base + 12 >= 0x100000u) return;

    size_t avail = 0x100000u - base;
    if (avail > sizeof buf) avail = sizeof buf;
    memcpy(buf, &cpu->mem[base], avail);
    if (memcmp(buf, "FORM", 4) != 0) return;

    music_play_xmi(buf, avail);
}

/* ---- software interrupts ---------------------------------------------- */
static void int21(CPU *cpu) {
    uint8_t ah = cpu->ah;
    if (getenv("DINO_DBG")) fprintf(stderr, "[INT21] AH=%02X BX=%04X CX=%04X SS:SP=%04X:%04X BP=%04X\n", ah, cpu->bx, cpu->cx, cpu->ss, cpu->sp, cpu->bp);
    switch (ah) {
        case 0x25:                                   /* set interrupt vector */
            trace("[INT21] set vector %02X -> %04X:%04X\n",
                  cpu->al, cpu->ds, cpu->dx);
            g_vec_seg[cpu->al] = cpu->ds; g_vec_off[cpu->al] = cpu->dx;
            note_vector(cpu->ds, cpu->dx);
            break;
        case 0x35:                                   /* get interrupt vector */
            trace("[INT21] get vector %02X -> %04X:%04X\n",
                  cpu->al, g_vec_seg[cpu->al], g_vec_off[cpu->al]);
            cpu->es = g_vec_seg[cpu->al]; cpu->bx = g_vec_off[cpu->al];
            break;
        case 0x1A:                                   /* set the DTA */
            g_dta_seg = cpu->ds; g_dta_off = cpu->dx;
            break;
        case 0x2F:                                   /* get the DTA */
            cpu->es = g_dta_seg; cpu->bx = g_dta_off;
            break;
        case 0x4E: find_first(cpu); break;
        case 0x4F: find_next(cpu); break;
        case 0x30: cpu->ax = 0x0005; break;          /* DOS version 5.0 */
        case 0x2C: cpu->cx = 0; cpu->dx = 0; break;  /* get time */
        case 0x2A: cpu->cx = 1993; cpu->dh = 1; cpu->dl = 1; break;  /* get date */
        case 0x19: cpu->al = 2; break;               /* current drive = C: */
        case 0x47: cpu->mem[seg_off(cpu->ds, cpu->si)] = 0; cpu->flags &= ~FLAG_CF; break; /* cwd="" */
        case 0x48: {                                 /* allocate paragraphs */
            /* BX = 0xFFFF is the standard how-much-is-there probe: DOS fails
             * it and reports the largest block in BX, and the caller asks
             * again for that. Answering every request with one fixed segment
             * handed the game memory inside its own image to scribble on. */
            uint16_t want = cpu->bx;
            uint16_t avail = (uint16_t)(HEAP_HI - g_heap_next);
            if (want > avail) {
                cpu->ax = 0x0008;                    /* insufficient memory */
                cpu->bx = avail;                     /* largest block */
                cpu->flags |= FLAG_CF;
            } else {
                cpu->ax = g_heap_next;
                g_heap_next = (uint16_t)(g_heap_next + want);
                cpu->flags &= ~FLAG_CF;
            }
            trace("[INT21] alloc %04X para -> %04X (avail %04X)\n",
                  want, cpu->ax, avail);
            break;
        }
        case 0x49: cpu->flags &= ~FLAG_CF; break;    /* free: bump alloc, no reuse */
        case 0x4A:                                   /* resize the program block */
            cpu->flags &= ~FLAG_CF; break;           /* always granted; it never moves */
        case 0x3C: {                                 /* create/truncate file */
            /* Without this the game cannot write its sound-configuration file,
             * gets a junk handle from the default success reply, and reports
             * "Unable to create configuration file". Created files go beside
             * the game data so a second run finds them. */
            char name[128]; read_fname(cpu, cpu->ds, cpu->dx, name, sizeof name);
            int fh = -1; for (int i = 5; i < MAXFH; i++) if (!g_fh[i]) { fh = i; break; }
            FILE *f = fh >= 0 ? open_game_file(name, "wb") : NULL;
            if (!f && fh >= 0) {                     /* not there yet: make it */
                char path[256];
                snprintf(path, sizeof path, "original/%s", name);
                f = fopen(path, "wb");
            }
            trace("[INT21] create '%s' -> %s fh=%d\n", name, f ? "ok" : "FAIL", fh);
            if (f) { g_fh[fh] = f; cpu->ax = fh; cpu->flags &= ~FLAG_CF; }
            else   { cpu->ax = 3; cpu->flags |= FLAG_CF; }
            break;
        }
        case 0x3D: {                                 /* open file */
            char name[128]; read_fname(cpu, cpu->ds, cpu->dx, name, sizeof name);
            int fh = -1; for (int i = 5; i < MAXFH; i++) if (!g_fh[i]) { fh = i; break; }
            FILE *f = fh >= 0 ? open_game_file(name, "rb") : NULL;
            trace("[INT21] open '%s' -> %s fh=%d\n", name, f ? "ok" : "FAIL", fh);
            heap_summary(cpu, name);
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
            /* Show the head of what arrived. A read that returns the right
             * count and the wrong bytes looks identical in a trace otherwise,
             * and the first few bytes of a game file are usually its magic. */
            { char head[32]; int hn = 0;
              for (unsigned i = 0; i < got && i < 8; i++)
                  hn += snprintf(head + hn, sizeof head - hn, "%02X ", cpu->mem[buf + i]);
              head[hn] = 0;
              trace("[INT21] read fh=%d %u bytes -> %04X:%04X, got %u: %s\n",
                    fh, n, cpu->ds, cpu->dx, (unsigned)got, head); }
            /* And the stream the read is filling. Borland's table starts at
             * DGROUP:7630 with 20-byte entries: level, flags, fd, hold, bsize,
             * then the far buffer and the far current pointer. A read that
             * lands the right bytes and leaves `level` at zero is a stream the
             * library will treat as empty however full the buffer is. */
            if (g_dgroup && fh >= 0 && fh < MAXFH) {
                uint16_t f = (uint16_t)(0x7630 + fh * 20);
                trace("[INT21]   FILE %d: level=%d flags=%04X fd=%d bsize=%u "
                      "buf=%04X:%04X curp=%04X:%04X\n", fh,
                      (int16_t)mem_read16(cpu, g_dgroup, f),
                      mem_read16(cpu, g_dgroup, (uint16_t)(f + 2)),
                      cpu->mem[seg_off(g_dgroup, (uint16_t)(f + 4))],
                      mem_read16(cpu, g_dgroup, (uint16_t)(f + 6)),
                      mem_read16(cpu, g_dgroup, (uint16_t)(f + 10)),
                      mem_read16(cpu, g_dgroup, (uint16_t)(f + 8)),
                      mem_read16(cpu, g_dgroup, (uint16_t)(f + 14)),
                      mem_read16(cpu, g_dgroup, (uint16_t)(f + 12)));
            }
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
        case 0x09: {                                 /* print string, $-terminated */
            /* The game talks to us here. Route it to stdout: a startup that
             * bails out says why, and we were throwing the reason away. */
            uint32_t a = seg_off(cpu->ds, cpu->dx);
            for (int i = 0; i < 4096 && cpu->mem[a + i] != '$'; i++)
                putchar(cpu->mem[a + i]);
            fflush(stdout); break;
        }
        case 0x40: {                                 /* write to handle */
            uint32_t a = seg_off(cpu->ds, cpu->dx);
            uint16_t n = cpu->cx;
            int fh = cpu->bx;
            if (fh == 1 || fh == 2) {                /* stdout / stderr */
                fwrite(&cpu->mem[a], 1, n, fh == 2 ? stderr : stdout);
                fflush(fh == 2 ? stderr : stdout);
            } else if (fh >= 0 && fh < MAXFH && g_fh[fh]) {
                fwrite(&cpu->mem[a], 1, n, g_fh[fh]);
            }
            cpu->ax = n; cpu->flags &= ~FLAG_CF; break;
        }
        case 0x44:                                   /* IOCTL: get device info */
            /* Borland's _setupio asks whether each standard handle is a
             * character device, to decide which ones to line-buffer. Report
             * 0..2 as console devices and anything else as a disk file. */
            cpu->dx = (cpu->bx <= 2) ? 0x80D3 : 0x0002;
            cpu->ax = cpu->dx;
            cpu->flags &= ~FLAG_CF; break;
        case 0x43: {                                 /* get/set file attributes */
            /* Answer from the filesystem instead of always saying yes: the
             * game probes for files here, and a blanket success sends it down
             * whichever path assumes they are present. */
            char name[128]; read_fname(cpu, cpu->ds, cpu->dx, name, sizeof name);
            FILE *probe = open_game_file(name, "rb");
            if (probe) fclose(probe);
            trace("[INT21] attr AL=%02X '%s' -> %s\n",
                  cpu->al, name, probe ? "exists" : "MISSING");
            if (cpu->al == 0 && !probe) {
                cpu->ax = 2; cpu->flags |= FLAG_CF;  /* file not found */
            } else {
                if (cpu->al == 0) cpu->cx = 0x20;    /* archive */
                cpu->flags &= ~FLAG_CF;
            }
            break;
        }
        case 0x57:                                   /* get/set file date+time */
            if (cpu->al == 0) { cpu->cx = 0x6000; cpu->dx = 0x1A61; }  /* 1993-03-01 */
            cpu->flags &= ~FLAG_CF; break;
        case 0x4C:                                   /* terminate */
            /* DOS terminate does not return. Pretending otherwise let the
             * lifted code run on into the bytes after `int 21h`, which is
             * where the hundreds of bogus recursive exits and every
             * `Stack overflow!` were coming from -- all of it post-mortem. */
            trace("[INT21] exit code %d\n", cpu->al);
            cpu->halted = 1;
            text_snapshot(cpu);
            stack_message_scan(cpu);
#ifdef DINO_SPCHECK
            /* Who decided to quit? The audit's live call stack still holds the
             * chain that got here. */
            { extern unsigned g_stk[]; extern int g_stk_depth;
              fprintf(stderr, "[exit] call chain (%d deep):\n", g_stk_depth);
              for (int i = 0; i < g_stk_depth && i < 40; i++)
                  fprintf(stderr, "    fn_%05X\n", g_stk[i]); }
#endif
            /* The last frame is usually a clear; the interesting one is
             * whatever had the most colour in it. */
            vga_best_dump("work/best_frame.bmp");
            vga_plane_report();
            /* The game composes offscreen and may never blit, so also go
             * looking for a picture-shaped buffer in RAM. */
            fb_scan(cpu, "work/fb_scan.bmp");
            vga_snapshot(cpu, "work/vga_exit.bmp");
            fflush(stdout); fflush(stderr);
            exit(cpu->al);
        default:
            /* Report success. DOS returns errors in CF, and leaving it as we
             * found it means a stale carry from some earlier call reads as a
             * failure the game then reports -- which is how a working open
             * turned into `Error closing file`. */
            trace("[INT21] AH=%02X (unhandled, reporting success)\n", ah);
            cpu->flags &= ~FLAG_CF;
            break;
    }
}

void dos_int21(CPU *cpu)  { int_handler(cpu, 0x21); }
void bios_int10(CPU *cpu) { int_handler(cpu, 0x10); }
void bios_int16(CPU *cpu) { int_handler(cpu, 0x16); }
void mouse_int33(CPU *cpu){ int_handler(cpu, 0x33); }

/* INT 33h -- the mouse driver.
 *
 * Answering "absent" made the game say so on its own title screen and carry on
 * without a pointer. It only needs the handful of calls below; the window
 * already knows where the pointer is, so this is mostly a units conversion.
 *
 * Position is reported in the driver's virtual screen, which for the 320-wide
 * graphics modes is 640 across and the real height down -- the horizontal
 * coordinate always comes back doubled, and every game of the era divides it
 * again. Ranges set through functions 7 and 8 are honoured so a game that
 * clamps the pointer to part of the screen gets what it asked for.
 *
 * ponytail: no cursor is drawn here. DinoPark draws its own, and a driver
 * cursor would be a second one; add one if a later screen turns out to expect
 * the driver to do it. */
/* DINO_CLICK: drive the pointer for a run with nobody at the mouse.
 *
 *   DINO_CLICK=96,175            hover there, then click
 *   DINO_CLICK=96,175;160,120    the same, one point after another
 *   DINO_CLICK=sweep             a grid of twenty, to find what responds
 *   DINO_CLICK_AT=10000          ms before the first one (default 9000)
 *   DINO_CLICK_STEP=2400         ms per point
 *
 * Each point is hovered before the button goes down: the game tracks the
 * pointer itself, and a click teleporting in from nowhere is not a gesture it
 * would ever see from a person.
 */
#define CLICK_MAX   64
#define CLICK_HOVER 900                        /* ms in place before pressing */
#define CLICK_HOLD  300                        /* ms held down */

/* DINO_CLICK=monkey[,seed]: click all over the screen for as long as the run
 * lasts. Not a substitute for knowing the UI -- it is a way to find what breaks
 * without knowing it, paired with the SP audit, the dispatch misses and the
 * heap summary, none of which need to understand a screen to notice it went
 * wrong. */
static int g_monkey;
static unsigned g_monkey_seed = 1;

static void click_script(int *x, int *y, int *b)
{
    static int n = -1, step = 2400, delay = 9000;
    static int px[CLICK_MAX], py[CLICK_MAX];
    static unsigned long t0;

    if (n < 0) {
        n = 0;
        const char *e = getenv("DINO_CLICK");
        if (!e) return;
        { const char *v = getenv("DINO_CLICK_AT");   if (v) delay = atoi(v); }
        { const char *v = getenv("DINO_CLICK_STEP"); if (v) step  = atoi(v); }
        if (!strncmp(e, "monkey", 6)) {
            g_monkey = 1;
            { const char *c = strchr(e, ','); if (c) g_monkey_seed = (unsigned)atoi(c + 1); }
            n = 1;                             /* so the caller keeps calling */
            t0 = host_ms();
            return;
        }
        while (*e && n < CLICK_MAX) {
            if (!strncmp(e, "sweep", 5)) {
                for (int k = 0; k < 20 && n < CLICK_MAX; k++, n++) {
                    px[n] = 32 + (k % 5) * 64;
                    py[n] = 25 + (k / 5) * 50;
                }
            } else {
                px[n] = atoi(e);
                const char *c = strchr(e, ',');
                py[n] = c ? atoi(c + 1) : 100;
                n++;
            }
            const char *semi = strchr(e, ';');
            if (!semi) break;
            e = semi + 1;
        }
        t0 = host_ms();
    }
    if (!n) return;

    unsigned long dt = host_ms() - t0;
    if (g_monkey) {
        if (dt < (unsigned long)delay) return;
        unsigned i = (unsigned)((dt - (unsigned long)delay) / (unsigned long)step);
        /* Deterministic, so a run that breaks something can be run again. Two
         * rounds of a cheap mix per point, because consecutive seeds out of a
         * plain LCG walk the screen in a line rather than covering it. */
        unsigned h = (i + g_monkey_seed) * 2654435761u;
        h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
        *x = (int)(h % VGA_W);
        *y = (int)((h >> 8) % VGA_H);
        int phase = (int)((dt - (unsigned long)delay) % (unsigned long)step);
        if (phase >= CLICK_HOVER && phase < CLICK_HOVER + CLICK_HOLD) *b = 1;
        return;
    }
    if (dt < (unsigned long)delay) return;
    dt -= (unsigned long)delay;

    int i = (int)(dt / (unsigned long)step);
    if (i >= n) i = n - 1;                     /* stay on the last point */
    int phase = (int)(dt % (unsigned long)step);
    *x = px[i]; *y = py[i];
    if (phase >= CLICK_HOVER && phase < CLICK_HOVER + CLICK_HOLD) *b = 1;
}

static int g_m_xmin, g_m_xmax = 639, g_m_ymin, g_m_ymax = 199;
static int g_m_lastx, g_m_lasty;                /* for the motion counters */
/* Press and release counts since each was last asked for, with where they
 * happened. A menu reads these rather than the live button state: by the time
 * it looks, a click is usually over. Answering "no presses, ever" is why
 * clicking the title screen did nothing but move the cursor. */
static int g_m_prev;
static int g_m_press[2], g_m_px[2], g_m_py[2];
static int g_m_rel[2], g_m_rx[2], g_m_ry[2];

static unsigned long g_m_fn[0x40];

void mouse_fn_dump(void)
{
    fprintf(stderr, "[mouse] INT 33h functions used:");
    for (int i = 0; i < 0x40; i++)
        if (g_m_fn[i]) fprintf(stderr, " %02X=%lu", i, g_m_fn[i]);
    fprintf(stderr, "\n");
}

static void mouse33(CPU *cpu)
{
    if (cpu->ax < 0x40) g_m_fn[cpu->ax]++;
    int x, y, b;
    vga_window_mouse(&x, &y, &b);
    click_script(&x, &y, &b);

    for (int i = 0; i < 2; i++) {
        int mask = 1 << i;
        if ((b & mask) && !(g_m_prev & mask)) { g_m_press[i]++; g_m_px[i] = x * 2; g_m_py[i] = y; }
        if (!(b & mask) && (g_m_prev & mask)) { g_m_rel[i]++;   g_m_rx[i] = x * 2; g_m_ry[i] = y; }
    }
    g_m_prev = b;
    x *= 2;                                      /* 320 pixels across 640 units */
    if (x < g_m_xmin) x = g_m_xmin; else if (x > g_m_xmax) x = g_m_xmax;
    if (y < g_m_ymin) y = g_m_ymin; else if (y > g_m_ymax) y = g_m_ymax;

    switch (cpu->ax) {
        case 0x0000:                             /* reset and detect */
            cpu->ax = 0xFFFF;                    /* installed */
            cpu->bx = 2;                         /* two buttons */
            g_m_xmin = g_m_ymin = 0;
            g_m_xmax = 639; g_m_ymax = VGA_H - 1;
            trace("[MOUSE] reset -> present\n");
            break;
        case 0x0001: case 0x0002: break;         /* show / hide the driver cursor */
        case 0x0003:                             /* position and buttons */
            { static int said; if (b && !said) { said = 1;
                trace("[MOUSE] button %d seen at %d,%d\n", b, x, y); } }
            cpu->bx = (uint16_t)b;
            cpu->cx = (uint16_t)x;
            cpu->dx = (uint16_t)y;
            break;
        case 0x0004: break;                      /* set position: the host owns it */
        case 0x0007: g_m_xmin = cpu->cx; g_m_xmax = cpu->dx;
            trace("[MOUSE] x range %d..%d\n", g_m_xmin, g_m_xmax); break;
        case 0x0008: g_m_ymin = cpu->cx; g_m_ymax = cpu->dx;
            trace("[MOUSE] y range %d..%d\n", g_m_ymin, g_m_ymax); break;
        case 0x000B:                             /* motion since the last call */
            cpu->cx = (uint16_t)(int16_t)(x - g_m_lastx);
            cpu->dx = (uint16_t)(int16_t)(y - g_m_lasty);
            g_m_lastx = x; g_m_lasty = y;
            break;
        case 0x0005: case 0x0006: {              /* press / release counts */
            int i = cpu->bx & 1;                 /* BX selects the button */
            int press = cpu->ax == 0x0005;
            cpu->ax = (uint16_t)b;
            cpu->bx = (uint16_t)(press ? g_m_press[i] : g_m_rel[i]);
            cpu->cx = (uint16_t)(press ? g_m_px[i] : g_m_rx[i]);
            cpu->dx = (uint16_t)(press ? g_m_py[i] : g_m_ry[i]);
            if (press) g_m_press[i] = 0; else g_m_rel[i] = 0;
            break;
        }
        default:
            trace("[MOUSE] AX=%04X (unhandled)\n", cpu->ax);
            break;
    }
}

/* Which interrupts the game actually uses, by vector. The park loop turned out
 * to touch almost none of them, which is why input wired to the INT 16h path
 * never arrived. */
unsigned long g_int_calls[256];

void int_vec_dump(void)
{
    fprintf(stderr, "[int] calls by vector:");
    for (int v = 0; v < 256; v++)
        if (g_int_calls[v]) fprintf(stderr, " %02X=%lu", v, g_int_calls[v]);
    fprintf(stderr, "\n");
}

/* The game's own timer interrupt.
 *
 * DinoPark hooks INT 8 and drives itself from it; the runtime was recording the
 * vector and never calling it. The main loop therefore ran, polled the mouse
 * twenty million times, drew its cursor -- and nothing else ever happened,
 * because for the game no time had passed.
 *
 * Lifted code keeps all its state in the CPU struct and in guest memory, so a
 * handler can be run from here the way the hardware would: between guest
 * instructions, on the guest's own stack. `iret` lifts to a bare return that
 * leaves SP alone, so the interrupt frame is pushed and dropped here; and the
 * whole register file is put back afterwards, since an ISR that perturbs the
 * interrupted code's registers is a bug wherever it comes from.
 */
static void run_guest_isr(CPU *cpu, int vec)
{
    static int inside;
    uint16_t seg = g_vec_seg[vec], off = g_vec_off[vec];
    if ((!seg && !off) || inside) return;
    inside = 1;

#ifdef DINO_SPCHECK
    { extern int g_recomp_quiet; g_recomp_quiet = 1; }
#endif
    CPU save = *cpu;
    push16(cpu, cpu->flags);
    push16(cpu, cpu->cs);
    push16(cpu, cpu->ip);
    recomp_dispatch(cpu, seg, off);
    *cpu = save;
#ifdef DINO_SPCHECK
    { extern int g_recomp_quiet; g_recomp_quiet = 0; }
#endif

    inside = 0;
}

/* 18.2 Hz, the rate the PIT interrupts at out of reset. Ticks that go by while
 * the guest is busy are dropped rather than queued: a burst of catch-up
 * interrupts is not what the hardware would have done either. */
static void timer_tick(CPU *cpu)
{
    static unsigned long last;
    unsigned long t = vga_ticks();
    if (t == last) return;
    last = t;
    run_guest_isr(cpu, 0x08);
}

void int_handler(CPU *cpu, int vec) {
    g_int_calls[vec & 0xFF]++;
    vga_flush_paced(cpu);
    /* Both from here rather than from one particular service: the park loop
     * polls the mouse and touches almost nothing else, so anything wired to
     * the INT 16h path alone never runs once the intro is over. */
    key_tick(cpu);
    timer_tick(cpu);
    switch (vec) {
        case 0x21: int21(cpu); break;
        case 0x10:                                  /* video BIOS */
            switch (cpu->ah) {
                case 0x00:                          /* set mode */
                    trace("[INT10] set mode %02X\n", cpu->al);
                    g_video_mode = cpu->al;
                    if (cpu->al == 0x13) {
                        g_vga_live = vga_window_open("DinoPark", VGA_W, VGA_H);
                        memset(&cpu->mem[VGA_BASE], 0, VGA_W * VGA_H);
                    }
                    break;
                case 0x0F:                          /* get current mode */
                    cpu->al = g_video_mode;
                    cpu->ah = (g_video_mode == 0x13) ? 40 : 80;   /* columns */
                    cpu->bh = 0;
                    break;
                case 0x12:                          /* EGA/VGA config */
                    if (cpu->bl == 0x10) {          /* get EGA info */
                        cpu->bh = 0;                /* colour mode */
                        cpu->bl = 3;                /* 256K installed */
                        cpu->cx = 0;
                    }
                    break;
                case 0x1A:                          /* display combination code */
                    /* The game refuses to start without this: it is how you
                     * ask the BIOS whether the adapter is VGA/MCGA, and a
                     * stub that answers nothing reads as a CGA. */
                    if (cpu->al == 0x00) {
                        cpu->al = 0x1A;             /* function supported */
                        cpu->bl = 0x08;             /* active: VGA w/ colour */
                        cpu->bh = 0x00;             /* no second adapter */
                    }
                    break;
                default: trace("[INT10] AH=%02X AL=%02X (unhandled)\n", cpu->ah, cpu->al); break;
            }
            break;
        case 0x16:                                  /* BIOS keyboard */
            key_tick(cpu);
            if (cpu->ah == 0x01 || cpu->ah == 0x11) {   /* is one waiting? */
                if (g_key_pending) { cpu->ax = g_key_pending; cpu->flags &= ~FLAG_ZF; }
                else                { cpu->ax = 0;          cpu->flags |= FLAG_ZF; }
            } else {                                     /* AH=0/0x10: take it */
                cpu->ax = g_key_pending;
                g_key_pending = 0;
                cpu->flags &= ~FLAG_ZF;
            }
            break;
        case 0x33: mouse33(cpu); break;
        case 0x66:                                  /* Miles Sound System AIL */
            /* The game binds AIL through 25 little `mov ax,FN / int 66h / retf`
             * thunks. Leaving AX alone returns the function number, which is
             * non-zero -- and fn_09035 polls AIL 0x689 (is anything playing?)
             * until it answers zero, so a no-op stub spins there forever.
             * With no driver loaded nothing is ever playing: answer zero.
             *
             * Except 0x704, which registers a sequence and answers with its
             * handle. Zero there reads as failure: the game printed "Unable to
             * register music data for file 'DINOCITY.XMI'" and abandoned the
             * title sequence. Hand back a handle -- nothing dereferences it,
             * it only has to be non-zero. */
            trace("[AIL] fn=%04X bx=%04X cx=%04X si=%04X di=%04X\n",
                  cpu->ax, cpu->bx, cpu->cx, cpu->si, cpu->di);
            /* 0x704 registers a sequence, and its two far pointers are the only
             * handle we get on the music: one of them should be the XMI the
             * game just loaded. Show the head of each -- an IFF file announces
             * itself in the first four bytes. */
            if (cpu->ax == 0x704) {
                uint16_t p[2][2] = { { cpu->cx, cpu->bx }, { cpu->di, cpu->si } };
                for (int i = 0; i < 2; i++) {
                    char h[64]; int hn = 0;
                    for (int j = 0; j < 12; j++)
                        hn += snprintf(h + hn, sizeof h - hn, "%02X ",
                                       mem_read8(cpu, p[i][0], (uint16_t)(p[i][1] + j)));
                    char t[16];
                    for (int j = 0; j < 12; j++) {
                        uint8_t c = mem_read8(cpu, p[i][0], (uint16_t)(p[i][1] + j));
                        t[j] = (c >= 32 && c < 127) ? (char)c : '.';
                    }
                    t[12] = 0;
                    trace("[AIL]   arg%d %04X:%04X: %s |%s|\n",
                          i, p[i][0], p[i][1], h, t);
                }
            }
            if (cpu->ax == 0x704) ail_register(cpu);
            cpu->ax = cpu->ax == 0x704 ? ++g_ail_seq : 0;
            cpu->flags &= ~FLAG_CF;
            break;
        case 0x1A: {                                /* BIOS tick count */
            /* A frozen clock is an infinite wait: every `spin until ticks >=
             * start + n` loop in the game never finishes. Derive real ticks
             * from the host at the PC's 18.2 Hz. */
            unsigned long t = vga_ticks();
            cpu->cx = (uint16_t)(t >> 16);
            cpu->dx = (uint16_t)t;
            cpu->al = 0;                            /* no midnight rollover */
            break;
        }
        case 0x03: cpu->halted = 1; break;                      /* breakpoint -> stop */
        default: trace("[INT %02X] AH=%02X (unhandled)\n", vec, cpu->ah); break;
    }
}

/* DINO_DUMPDG: the whole data segment at exit.
 *
 * Watching one word needs its offset, and the way to find a game variable is a
 * value you know it holds. Scan the dump for it, then point DINO_STATEWORD at
 * whatever turns up. */
CPU *g_cpu_for_dump;

void dump_dgroup(const char *path)
{
    if (!getenv("DINO_DUMPDG") || !g_dgroup || !g_cpu_for_dump) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(&g_cpu_for_dump->mem[seg_off(g_dgroup, 0)], 1, 0x10000, f);
    fclose(f);
    fprintf(stderr, "[dgroup] %04X:0000 + 64K -> %s\n", g_dgroup, path);
}

/* ---- image loader (MZ + relocations into image space) ------------------ */
int dino_load_image(CPU *cpu, const char *path) {
    g_cpu_for_dump = cpu;
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
    /* Park the PSP and environment at the top, immediately below the video
     * window, so the heap gets the whole span beneath them. Both are small --
     * the PSP is one paragraph short of 0x100 bytes and the environment fits
     * in the 0x400 this code zeroes -- so 0x9F00 upwards is room to spare. */
    const uint16_t PSP_SEG = 0x9F00, ENV_SEG = 0x9F10, TOP_SEG = 0xA000;
    uint32_t psp = seg_off(PSP_SEG, 0), env = seg_off(ENV_SEG, 0);
    cpu->mem[psp + 0x00] = 0xCD; cpu->mem[psp + 0x01] = 0x20;          /* INT 20h */
    cpu->mem[psp + 0x02] = TOP_SEG & 0xFF; cpu->mem[psp + 0x03] = TOP_SEG >> 8; /* top of mem */
    cpu->mem[psp + 0x2C] = ENV_SEG & 0xFF; cpu->mem[psp + 0x2D] = ENV_SEG >> 8; /* env seg */
    cpu->mem[psp + 0x80] = 0;                                          /* cmdline len 0 */
    cpu->mem[psp + 0x81] = 0x0D;
    /* Environment block. DOS lays it out as NAME=VALUE strings, a double NUL,
     * a word count, then the program's full pathname -- which is how a program
     * finds its own directory, and how DinoPark builds the path to dino.cfg.
     * The path has to be NUL-terminated or that parse runs into whatever
     * follows it. */
    static const char *const envv[] = {
        "COMSPEC=C:\\COMMAND.COM",
        "PATH=C:\\DOS",
        "PROMPT=$p$g",
    };
    static const char progpath[] = "C:\\DINOPARK\\DINOPARK.EXE";
    memset(&cpu->mem[env], 0, 0x400);
    uint32_t e = env;
    for (size_t i = 0; i < sizeof envv / sizeof *envv; i++) {
        size_t n = strlen(envv[i]) + 1;
        memcpy(&cpu->mem[e], envv[i], n);
        e += n;
    }
    cpu->mem[e++] = 0;                        /* terminating empty string */
    cpu->mem[e++] = 1; cpu->mem[e++] = 0;     /* one trailing string follows */
    memcpy(&cpu->mem[e], progpath, sizeof progpath);   /* sizeof: keeps the NUL */

    /* A mouse. The game looks for one by fetching the INT 33h vector and
     * treating a null ES:BX as "no driver", then says so on its own title
     * screen. Point it somewhere non-null; the vector is never called, the
     * game goes through int86(0x33) once it believes a driver is there.
     *
     * Only the shadow table, not the real interrupt vector at 0000:00CC --
     * the image loads at linear 0 here, so that address is the game's own
     * code and writing a pointer over it would corrupt it. */
    g_vec_seg[0x33] = 0xC000; g_vec_off[0x33] = 0x0100;

    /* MZ initial register state (image space); DOS enters with DS=ES=PSP */
    cpu->cs = LOADSEG;  cpu->ip = 0;                      /* CS:IP = 0000:0000 */
    cpu->ss = 0x3D04;   cpu->sp = 0x0080;                /* from header */
    /* The startup's very first instruction is `mov dx, DGROUP` -- a relocated
     * immediate at image offset 1 -- so the data segment is readable from the
     * image rather than hardcoded. */
    g_dgroup = (uint16_t)(cpu->mem[1] | (cpu->mem[2] << 8));

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
