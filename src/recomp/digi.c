/*
 * digi.c - the game's digital sound effects, out of the host's speakers.
 *
 * DinoPark drives Miles for both halves of its audio. The music side registers
 * an XMI and is handled in music.c; the digital side hands over a DIGPAK
 * SNDSTRUC, which the trace found by dumping what each INT 66h call was given:
 *
 *     +0  far pointer to the sample      +6  far pointer to an is-playing flag
 *     +4  length in bytes                +10 sample rate in Hz
 *
 * The samples themselves are unsigned 8-bit PCM at 7000, 11000 or 22000 Hz --
 * the .ABT files on disk are compressed, but the game decompresses them into
 * its own heap before telling the driver about them, so none of that format
 * needs to be understood here. Follow the pointer and play what is there.
 *
 * Everything is resampled to one output rate so the device is opened once. A
 * game that changed rate per effect would otherwise reopen it constantly, and
 * each reopen is an audible click. Nearest-sample stepping is enough for 8-bit
 * effects recorded at 7 kHz.
 *
 * One sound at a time, because that is what the original did: DIGPAK has a
 * single channel and a new effect replaces whatever is playing.
 */
#include "digi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIGI_RATE  22050
#define DIGI_SLOTS 4

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>

static int       g_on = -1;
static HWAVEOUT  g_wo;
static WAVEHDR   g_hdr[DIGI_SLOTS];
static uint8_t  *g_buf[DIGI_SLOTS];
static unsigned  g_cap[DIGI_SLOTS];

static int enabled(void)
{
    if (g_on < 0) {
        const char *e = getenv("DINO_SFX");
        g_on = !(e && !atoi(e));
    }
    return g_on;
}

static int open_device(void)
{
    if (g_wo) return 1;
    WAVEFORMATEX f;
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 1;
    f.nSamplesPerSec = DIGI_RATE;
    f.wBitsPerSample = 8;
    f.nBlockAlign = 1;
    f.nAvgBytesPerSec = DIGI_RATE;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_wo = NULL;
        g_on = 0;
        fprintf(stderr, "[sfx] no wave output available\n");
        return 0;
    }
    return 1;
}

/* A slot the device has finished with, or a fresh one. */
static int free_slot(void)
{
    for (int i = 0; i < DIGI_SLOTS; i++) {
        if (!g_hdr[i].lpData) return i;
        if (g_hdr[i].dwFlags & WHDR_DONE) {
            waveOutUnprepareHeader(g_wo, &g_hdr[i], sizeof g_hdr[i]);
            g_hdr[i].lpData = NULL;
            return i;
        }
    }
    return -1;
}

int digi_play(const uint8_t *pcm, unsigned len, unsigned rate)
{
    /* Guard rails, because the pointer comes out of guest memory and a struct
     * that is not a SNDSTRUC still reads as one. */
    if (!enabled() || !pcm || len < 64) return 0;
    if (rate < 4000 || rate > 45000) return 0;
    if (!open_device()) return 0;

    /* One channel: whatever is playing gives way. */
    waveOutReset(g_wo);

    int s = free_slot();
    if (s < 0) return 0;

    unsigned out_len = (unsigned)((uint64_t)len * DIGI_RATE / rate);
    if (out_len < 1 || out_len > 4u * 1024 * 1024) return 0;
    if (g_cap[s] < out_len) {
        uint8_t *nb = realloc(g_buf[s], out_len);
        if (!nb) return 0;
        g_buf[s] = nb;
        g_cap[s] = out_len;
    }
    for (unsigned i = 0; i < out_len; i++) {
        uint64_t src = (uint64_t)i * rate / DIGI_RATE;
        g_buf[s][i] = pcm[src < len ? (unsigned)src : len - 1];
    }

    /* DINO_SFX_DUMP=1: keep what was played, as a .wav per effect. The one
     * thing a check cannot tell you about audio is whether it sounds right, and
     * a sample that is silence or noise reads the same in a trace as one that
     * is a dinosaur. The mean should sit near the 0x80 centre of unsigned 8-bit
     * and the spread should not be zero. */
    { static int dump = -1, n;
      if (dump < 0) dump = getenv("DINO_SFX_DUMP") != NULL;
      if (dump && n < 64) {
          long sum = 0; int lo = 255, hi = 0;
          for (unsigned i = 0; i < len; i++) {
              sum += pcm[i];
              if (pcm[i] < lo) lo = pcm[i];
              if (pcm[i] > hi) hi = pcm[i];
          }
          char path[64];
          snprintf(path, sizeof path, "work/sfx_%02d.wav", n++);
          FILE *f = fopen(path, "wb");
          if (f) {
              unsigned dl = len, rl = 36 + dl;
              fwrite("RIFF", 1, 4, f); fwrite(&rl, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
              unsigned sz = 16; unsigned short one = 1, bits = 8;
              unsigned sr = rate, br = rate;
              fwrite(&sz, 4, 1, f); fwrite(&one, 2, 1, f); fwrite(&one, 2, 1, f);
              fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
              fwrite(&one, 2, 1, f); fwrite(&bits, 2, 1, f);
              fwrite("data", 1, 4, f); fwrite(&dl, 4, 1, f);
              fwrite(pcm, 1, len, f);
              fclose(f);
          }
          fprintf(stderr, "[sfx] %s  %u bytes @%u Hz  mean %ld, range %d..%d\n",
                  path, len, rate, sum / (long)len, lo, hi);
      } }

    memset(&g_hdr[s], 0, sizeof g_hdr[s]);
    g_hdr[s].lpData = (LPSTR)g_buf[s];
    g_hdr[s].dwBufferLength = out_len;
    if (waveOutPrepareHeader(g_wo, &g_hdr[s], sizeof g_hdr[s]) != MMSYSERR_NOERROR) {
        g_hdr[s].lpData = NULL;
        return 0;
    }
    if (waveOutWrite(g_wo, &g_hdr[s], sizeof g_hdr[s]) != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(g_wo, &g_hdr[s], sizeof g_hdr[s]);
        g_hdr[s].lpData = NULL;
        return 0;
    }
    return 1;
}

void digi_stop(void)
{
    if (g_wo) waveOutReset(g_wo);
}

#else   /* not Windows */

int  digi_play(const uint8_t *p, unsigned n, unsigned r) { (void)p; (void)n; (void)r; return 0; }
void digi_stop(void) { }

#endif
