/*
 * test_abt.c - run the game's own decompressor over every sound and check it.
 *
 * .ABT is a compressed sound. The header is
 *
 *     +0  u16 decompressed length   +4  u16 0x0220
 *     +2  u16 sample rate           +6  u16 0x0080
 *     +8  the compressed data
 *
 * and fn_00E50 -- the same in-place DPCM routine the sprites use -- expands it.
 * The game lays the block out so the compressed bytes sit at the TAIL of a
 * buffer the size of the output and expands forward into itself: for CLAPPER,
 * 3376 bytes out and 857 in, the source starts at 3376-857 = 0x09D7, which is
 * exactly what the live call passes. Two far pointers, source then destination,
 * and the decompressed length comes back in AX.
 *
 * The result is unsigned 8-bit PCM, so silence is 128 and a correct sample sits
 * around there. A buffer that is mostly zeros is one the codec did not fill --
 * which is what happens above about 3x compression, and is why the MECC harp
 * and the clapperboard came out silent while the crash cymbal and the comet
 * were fine.
 *
 *   scripts\build_test.ps1 abt            every sound
 *   scripts\build_test.ps1 abt FILE.ABT   just one
 *
 * Writes work/<name>.wav for anything it decodes, because the one thing this
 * cannot tell you is whether it sounds like a dinosaur.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "recomp/cpu.h"
#include "recomp/runtime16.h"
#include "recomp/gen/recomp_all.h"

#define BLK_SEG 0x4000            /* somewhere harmless for the block */
#define STK_SEG 0x8800

static void write_wav(const char *name, const uint8_t *pcm, unsigned len, unsigned rate)
{
    char path[256];
    const char *base = name;
    for (const char *p = name; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(path, sizeof path, "work/%.*s.wav", (int)(strcspn(base, ".")), base);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    unsigned dl = len, rl = 36 + dl, sz = 16, br = rate;
    unsigned short one = 1, bits = 8;
    fwrite("RIFF", 1, 4, f); fwrite(&rl, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&sz, 4, 1, f); fwrite(&one, 2, 1, f); fwrite(&one, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&one, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dl, 4, 1, f);
    fwrite(pcm, 1, len, f); fclose(f);
}

static int one(CPU *cpu, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("%-24s cannot open\n", path); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc((size_t)n);
    if (fread(file, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(file); return 1; }
    fclose(f);

    unsigned out_len = file[0] | (file[1] << 8);
    unsigned rate    = file[2] | (file[3] << 8);
    unsigned comp    = (unsigned)n - 8;
    if (!out_len || out_len > 0xF000u || comp >= out_len) {
        printf("%-24s implausible header: out=%u rate=%u\n", path, out_len, rate);
        free(file); return 1;
    }

    /* Lay the block out the way the game does and expand into itself.
     *
     * The WHOLE file goes at the tail, header included -- the codec reads its
     * own header. CLAPPER is 857 bytes and expands to 3376, and the live call
     * passes a source offset of 0x09D7 = 3376 - 857, not 3376 - 849. Placing
     * only the payload leaves the routine reading the wrong bytes as a header,
     * and it never terminates. */
    uint8_t *blk = &cpu->mem[seg_off(BLK_SEG, 0)];
    memset(blk, 0, out_len);
    unsigned src_off = out_len - (unsigned)n;
    memcpy(blk + src_off, file, (size_t)n);

    /* The codec reads its tables through DS, so it needs the real data
     * segment -- the startup's first instruction is `mov dx, DGROUP`, a
     * relocated immediate at image offset 1. Without it the routine reads
     * image bytes as a table and never terminates. */
    cpu->ds = (uint16_t)(cpu->mem[1] | (cpu->mem[2] << 8));
    cpu->es = cpu->ds;
    cpu->ss = STK_SEG; cpu->sp = 0xFFF0;
    push16(cpu, BLK_SEG); push16(cpu, 0);            /* dst far */
    push16(cpu, BLK_SEG); push16(cpu, (uint16_t)src_off); /* src far */
    push16(cpu, 0); push16(cpu, 0xFFFF);             /* far-return slot */
    fn_00E50(cpu);
    unsigned got = cpu->ax;

    /* Sound is smooth; noise is not. The average step between neighbouring
     * samples separates a decode that worked from one that merely produced
     * plausible-looking bytes far better than any statistic about their
     * values: effects at 7-22 kHz move by a few levels per sample, while
     * random bytes average about 85. */
    long sum = 0, dsum = 0; unsigned zeros = 0;
    for (unsigned i = 0; i < out_len; i++) {
        sum += blk[i];
        zeros += (blk[i] == 0);
        if (i) dsum += abs((int)blk[i] - (int)blk[i - 1]);
    }
    long mean = sum / (long)out_len;
    long step = dsum / (long)(out_len > 1 ? out_len - 1 : 1);
    /* Both ends matter. A step of 0 is a flat buffer -- the codec returned the
     * right length and wrote nothing that varies -- and a step near 85 is
     * random bytes. Real effects here land between about 3 and 40. Mostly-zero
     * output fails too: silence in unsigned 8-bit is 128, so a buffer that is
     * 40% zeros is one the routine left as it found it. */
    int ok = (got == out_len) && step >= 2 && step <= 60 && zeros * 4 < out_len;

    printf("%-24s %6u -> %5u (%.1fx)  ret=%5u  mean=%3ld  step=%3ld  zeros=%2u%%  %s\n",
           path, comp, out_len, (double)out_len / comp, got, mean, step,
           zeros * 100 / out_len, ok ? "ok" : "BAD");
    write_wav(path, blk, out_len, rate);
    free(file);
    return !ok;
}

int dino_load_image(CPU *cpu, const char *path);

int main(int argc, char **argv)
{
    CPU cpu; cpu_init(&cpu);
    if (cpu_alloc_mem(&cpu) != 0) return 1;
    /* The decompressor lives in the image and reads its own tables, so the
     * image has to be loaded even though nothing is booted. */
    if (dino_load_image(&cpu, "original/DINOPARK.EXE") != 0) return 1;

    static const char *const all[] = {
        "original/MECCHARP.ABT", "original/CLAPPER.ABT", "original/CRASHSYM.ABT",
        "original/COMET.ABT",    "original/HOWL.ABT",    "original/RICOCHE3.ABT",
        "original/POPP1.ABT",    "original/BELL.ABT",    "original/BIRDS.ABT",
        "original/AUCTIONA.ABT", "original/BEEZD.ABT",   "original/CRACKLE.ABT",
    };
    int bad = 0, n = 0;
    if (argc > 1) { for (int i = 1; i < argc; i++) { bad += one(&cpu, argv[i]); n++; } }
    else for (size_t i = 0; i < sizeof all / sizeof *all; i++) { bad += one(&cpu, all[i]); n++; }
    printf("%d of %d decoded\n", n - bad, n);
    return bad != 0;
}
