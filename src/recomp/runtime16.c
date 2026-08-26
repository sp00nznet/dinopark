/*
 * runtime16.c - minimal DOS/VGA runtime for the DinoPark bring-up.
 *
 * First-boot scaffolding: enough INT 21h (DOS), INT 10h (video), keyboard,
 * mouse and port handling to get the Borland startup + game init running. Many
 * services are logged-and-stubbed; we fill them in as the boot demands them.
 */
#include "runtime16.h"
#include "video.h"
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
#define HEAP_HI 0x9000u
static uint16_t g_heap_next = HEAP_LO;
static uint16_t g_dgroup;                  /* from `mov dx, DGROUP` at image 0 */

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

static void watch_note(uint32_t addr, uint8_t val)
{
    static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("DINO_WATCH");
        if (e) g_watch = (uint32_t)strtoul(e, NULL, 16);
    }
    if (addr != g_watch || g_watch_fired >= 8) return;
    g_watch_fired++;
    fprintf(stderr, "[watch] %05X write #%d = %02X\n", addr, g_watch_fired, val);
#ifdef DINO_SPCHECK
    if (g_watch_fired == 1) {
        extern unsigned g_stk[]; extern int g_stk_depth;
        for (int i = 0; i < g_stk_depth && i < 40; i++)
            fprintf(stderr, "[watch]     fn_%05X\n", g_stk[i]);
    }
#endif
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

int recomp_mem_write8(CPU *cpu, uint32_t addr, uint8_t val)
{
    /* remember the blitter's mode word as it is written */
    if (addr == 0x091D0u + 0x14) g_blit_cs14_lo = val;
    if (addr == 0x091D0u + 0x15) g_blit_cs14_hi = val;
    watch_note(addr, val);
    hdr_write_note(addr, val);
    chain_check(cpu, addr, val);
    f5_note(addr, val);
    if (addr < VGA_LOW || addr >= VGA_HIGH || g_chain4) return 0;
    uint32_t off = addr - VGA_LOW;
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

void vga_flush(CPU *cpu)
{
    if (!g_vga_live) return;
    vga_window_present(vga_frame(cpu), (const uint8_t *)g_palette);
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
            static uint8_t t; static unsigned n;
            g_port_reads[0]++;
            if (g_vga_live && ((n++ & 0x3FF) == 0)) vga_flush(cpu);
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

/* ---- software interrupts ---------------------------------------------- */
static void int21(CPU *cpu) {
    uint8_t ah = cpu->ah;
    if (getenv("DINO_DBG")) fprintf(stderr, "[INT21] AH=%02X BX=%04X CX=%04X SS:SP=%04X:%04X BP=%04X\n", ah, cpu->bx, cpu->cx, cpu->ss, cpu->sp, cpu->bp);
    switch (ah) {
        case 0x25:                                   /* set interrupt vector */
            trace("[INT21] set vector %02X -> %04X:%04X\n",
                  cpu->al, cpu->ds, cpu->dx);
            g_vec_seg[cpu->al] = cpu->ds; g_vec_off[cpu->al] = cpu->dx;
            break;
        case 0x35:                                   /* get interrupt vector */
            cpu->es = g_vec_seg[cpu->al]; cpu->bx = g_vec_off[cpu->al];
            break;
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
            trace("[INT21] read fh=%d %u bytes -> %04X:%04X (linear %05X..%05X)\n",
                  fh, n, cpu->ds, cpu->dx, buf, buf + (unsigned)got);
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

void int_handler(CPU *cpu, int vec) {
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
        case 0x16: cpu->ax = 0; cpu->flags |= FLAG_ZF; break;  /* keyboard: no key */
        case 0x33: cpu->ax = 0; break;                          /* mouse: absent */
        case 0x66:                                  /* Miles Sound System AIL */
            /* The game binds AIL through 25 little `mov ax,FN / int 66h / retf`
             * thunks. Leaving AX alone returns the function number, which is
             * non-zero -- and fn_09035 polls AIL 0x689 (is anything playing?)
             * until it answers zero, so a no-op stub spins there forever.
             * With no driver loaded nothing is ever playing: answer zero. */
            trace("[AIL] fn=%04X\n", cpu->ax);
            cpu->ax = 0;
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
