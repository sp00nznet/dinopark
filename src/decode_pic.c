/*
 * decode_pic.c - render a full-screen DinoPark .PIC via the LIFTED RLE blitter.
 *
 * Full-screen UNCP picture layout (recovered from the loader FUN_1d88_0f75):
 *   +0  char[4] "UNCP"
 *   +4  u32     block offset (= 8)
 *   +8  u32     block size (= bytes to EOF-ish; also the RLE stream end ptr)
 *   +12 u16     width   (320)
 *   +14 u16     height  (200)
 *   +16 u8[768] PALETTE (256 * 6-bit RGB; index 0 forced to black on load)
 *   +784 ...    RLE stream  (decoded by fn_1907 / FUN_1907_00be)
 *
 * fn_1907 RLE: control c; c&0x80==0 -> literal run of c+1 bytes; c&0x80 -> fill
 * run of (1-(int8)c) of the next byte. mode>=0x41 selects RLE.
 *
 * Output: 8bpp indices (work/pic_decoded.bin) + the 768-byte palette
 * (work/pic_palette.pal), rendered to PNG by tools/render_pic.py.
 *
 *   cl /I src /I src\recomp decode_pic.c src\recomp\cpu.c src\recomp\gen\dino_decode.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp/cpu.h"
#include "recomp/gen/dino_decode.h"

#define SEG_SRC 0x2000
#define SEG_DST 0x5000
#define SEG_SS  0x8000

static unsigned u16(const unsigned char *d, int o){ return d[o] | (d[o+1] << 8); }
static unsigned u32(const unsigned char *d, int o){ return u16(d,o) | (u16(d,o+2) << 16); }

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "original/AUCTION.PIC";
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *file = malloc(n); fread(file, 1, n, f); fclose(f);
    if (memcmp(file, "UNCP", 4) != 0) { fprintf(stderr, "not UNCP\n"); return 1; }

    unsigned W = u16(file, 12), H = u16(file, 14);
    unsigned tail = u32(file, 8) + 12;     /* block size is rel. to +12; RLE ends here */
    unsigned PAL = 16, IMG = 16 + 768;     /* palette then RLE stream */
    unsigned count = W * H;
    if (tail > (unsigned)n) tail = n;
    printf("%s : %ux%u (%u px), palette[16:784], RLE stream [%u:%u]\n",
           path, W, H, count, IMG, tail);

    /* the 768-byte 6-bit palette rides right after W/H; index 0 -> black */
    unsigned char pal[768];
    memcpy(pal, &file[PAL], 768);
    pal[0] = pal[1] = pal[2] = 0;
    FILE *pf = fopen("work/pic_palette.pal", "wb");
    if (pf) { fwrite(pal, 1, 768, pf); fclose(pf); }

    CPU cpu; cpu_init(&cpu);
    if (cpu_alloc_mem(&cpu) != 0) return 1;
    memcpy(&cpu.mem[seg_off(SEG_SRC, 0)], &file[IMG], tail - IMG);

    /* call fn_1907(src far=SEG_SRC:0, dst far=SEG_DST:0, count, mode=0x80) */
    cpu.ss = SEG_SS; cpu.sp = 0xFFF0;
    push16(&cpu, 0x0080);                 /* param_5 mode (>=0x41 => RLE) */
    push16(&cpu, (uint16_t)(count >> 16));/* param_4 count_hi */
    push16(&cpu, (uint16_t)(count));      /* param_3 count_lo */
    push16(&cpu, SEG_DST); push16(&cpu, 0);   /* dst far */
    push16(&cpu, SEG_SRC); push16(&cpu, 0);   /* src far */
    push16(&cpu, 0); push16(&cpu, 0);     /* far-return slot */
    fn_1907(&cpu);

    unsigned char *out = &cpu.mem[seg_off(SEG_DST, 0)];
    FILE *o = fopen("work/pic_decoded.bin", "wb");
    if (o) { fwrite(out, 1, count, o); fclose(o); }
    FILE *m = fopen("work/pic_dims.txt", "w");
    if (m) { fprintf(m, "%u %u\n", W, H); fclose(m); }
    printf("wrote work/pic_decoded.bin (%ux%u 8bpp) + work/pic_palette.pal\n", W, H);
    return 0;
}
