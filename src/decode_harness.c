/*
 * decode_harness.c - run the lifted DPCM sprite decoder on a real .ACT sprite.
 *
 * Loads one UNC2 sprite's bytes into the recomp16 flat memory, sets up the
 * far-pointer cdecl frame the lifted fn_0E50 expects, and calls it. fn_0E50
 * decodes `decompsize` (= first word of the sprite) output bytes into the dst
 * buffer, calling the DPCM helpers (fn_0F2E/103B/10DE) via shared CPU state.
 *
 * Output: prints the decoded bytes and writes a raw .data dump we can render.
 *
 *   cl /I src/recomp decode_harness.c src/recomp/cpu.c src/recomp/gen/dino_decode.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp/cpu.h"
#include "recomp/gen/dino_decode.h"
extern unsigned g_end_si, g_end_di;

/* memory layout (segments) */
#define SEG_CS   0x1000   /* code/scratch seg: delta table at cs:0xE20 */
#define SEG_SRC  0x2000   /* compressed sprite bytes              */
#define SEG_DST  0x3000   /* decoded output                       */
#define SEG_SS   0x8000   /* stack                                */

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "original/ALBERT.ACT";
    long sprite_index = argc > 2 ? atol(argv[2]) : 0;

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *file = malloc(n); fread(file, 1, n, f); fclose(f);

    if (memcmp(file, "UNC2", 4) != 0) { fprintf(stderr, "not UNC2\n"); return 1; }
    unsigned cnt = file[4] | (file[5] << 8);
    unsigned tbl = file[6] | (file[7] << 8) | (file[8] << 16) | (file[9] << 24);
    if (sprite_index >= cnt) { fprintf(stderr, "only %u sprites\n", cnt); return 1; }
    unsigned so = 0; { unsigned o = tbl + 4 * sprite_index;
        so = file[o] | (file[o+1]<<8) | (file[o+2]<<16) | (file[o+3]<<24); }
    unsigned eo = (sprite_index + 1 < (long)cnt)
        ? (file[tbl+4*(sprite_index+1)] | (file[tbl+4*(sprite_index+1)+1]<<8)
           | (file[tbl+4*(sprite_index+1)+2]<<16) | (file[tbl+4*(sprite_index+1)+3]<<24))
        : tbl;
    unsigned slen = eo - so;
    unsigned decompsize = file[so] | (file[so+1] << 8);
    printf("%s sprite %ld: file[%u:%u] len=%u  decompsize(word0)=%u  word1=%u\n",
           path, sprite_index, so, eo, slen, decompsize,
           file[so+2] | (file[so+3] << 8));

    unsigned width = decompsize;                 /* word0 (raw file) */
    unsigned height = file[so+2] | (file[so+3] << 8);  /* word1 */
    printf("running lifted fn_0E50 + DPCM helpers on real sprite bytes...\n");

    CPU cpu; cpu_init(&cpu);
    if (cpu_alloc_mem(&cpu) != 0) return 1;
    memcpy(&cpu.mem[seg_off(SEG_SRC, 0)], &file[so], slen);

    /* One call: fn_0E50 reads its 9-byte in-buffer header then decodes `word0`
     * pixels. NOTE: this feeds the *raw file* layout; the real game first runs a
     * loader (FUN_1862_0797) that reframes the sprite into the decoder's input
     * buffer (word0 = full decompressed size). So this proves the lifted decoder
     * *executes* on real data; loader-correct framing is the next step. */
    cpu.ss = SEG_SS; cpu.sp = 0xFFF0;
    cpu.cs = SEG_CS; cpu.ds = SEG_SRC; cpu.es = SEG_DST;
    push16(&cpu, SEG_DST); push16(&cpu, 0);  /* dst far */
    push16(&cpu, SEG_SRC); push16(&cpu, 0);  /* src far */
    push16(&cpu, 0); push16(&cpu, 0);        /* far-return slot */
    fn_0E50(&cpu);

    unsigned char *out = &cpu.mem[seg_off(SEG_DST, 0)];
    printf("decoder ran: consumed %u input bytes, produced %u pixels\n",
           g_end_si, g_end_di);
    FILE *o = fopen("work/decoded.bin", "wb");
    if (o) { fwrite(out, 1, decompsize, o); fclose(o); }
    printf("wrote work/decoded.bin (%u px)\n", decompsize);
    (void)height;
    return 0;
}
