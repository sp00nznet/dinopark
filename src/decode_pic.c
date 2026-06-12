/*
 * decode_pic.c - render a full-screen DinoPark .PIC via the LIFTED RLE blitter.
 *
 * Full-screen UNCP pictures: [magic"UNCP"][u16 type=8][u16 pad][u32 tail_ptr]
 * [u16 width][u16 height][RLE stream ...]. The stream is decoded by fn_1907
 * (lifted FUN_1907_00be): control byte c; c&0x80==0 -> literal run of c+1 bytes;
 * c&0x80 -> fill run of (1-(int8)c) of the next byte. mode>=0x41 selects RLE.
 *
 * Output: a raw 8bpp index buffer (work/pic_<name>.bin) we render to PNG.
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
    unsigned tail = u32(file, 8);          /* RLE stream is [16 .. tail) */
    unsigned count = W * H;
    printf("%s : %ux%u (%u px), RLE stream [16:%u] = %u bytes\n",
           path, W, H, count, tail, tail - 16);

    CPU cpu; cpu_init(&cpu);
    if (cpu_alloc_mem(&cpu) != 0) return 1;
    memcpy(&cpu.mem[seg_off(SEG_SRC, 16)], &file[16], tail - 16);

    /* call fn_1907(src far=SEG_SRC:16, dst far=SEG_DST:0, count, mode=0x80) */
    cpu.ss = SEG_SS; cpu.sp = 0xFFF0;
    push16(&cpu, 0x0080);                 /* param_5 mode (>=0x41 => RLE) */
    push16(&cpu, (uint16_t)(count >> 16));/* param_4 count_hi */
    push16(&cpu, (uint16_t)(count));      /* param_3 count_lo */
    push16(&cpu, SEG_DST); push16(&cpu, 0);   /* dst far */
    push16(&cpu, SEG_SRC); push16(&cpu, 16);  /* src far */
    push16(&cpu, 0); push16(&cpu, 0);     /* far-return slot */
    fn_1907(&cpu);

    unsigned char *out = &cpu.mem[seg_off(SEG_DST, 0)];
    char outpath[256];
    snprintf(outpath, sizeof outpath, "work/pic_decoded.bin");
    FILE *o = fopen(outpath, "wb");
    if (o) { fwrite(out, 1, count, o); fclose(o); }
    printf("wrote %s (%ux%u 8bpp)\n", outpath, W, H);
    /* emit dims for the renderer */
    FILE *m = fopen("work/pic_dims.txt", "w");
    if (m) { fprintf(m, "%u %u\n", W, H); fclose(m); }
    return 0;
}
