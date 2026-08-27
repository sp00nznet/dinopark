/*
 * test_xmi.c - check the XMI parser, and write something you can listen to.
 *
 *   scripts\build_test.ps1 xmi            checks every original\*.XMI
 *   scripts\build_test.ps1 xmi FILE.XMI   just that one
 *
 * The parser is the risky part of music.c: XMI's delays are not MIDI's
 * variable-length quantities, and its note-on carries a duration where a MIDI
 * file would carry a matching note-off later in the stream. Both are easy to
 * get subtly wrong in a way that still produces plausible-looking output, so
 * this asserts the invariants instead of eyeballing them:
 *
 *   - every note-on is answered by exactly one note-off on the same channel
 *     and note, at a tick no earlier than its own
 *   - ticks never go backwards after the sort
 *   - channel, note and velocity all stay in range
 *
 * It also writes work\<name>.mid, because the one thing a check cannot tell you
 * is whether the tune is right.
 *
 * music.c is included rather than linked: the events live in file statics, and
 * a test is not a reason to widen the interface.
 */
#include "recomp/music.c"

#include <assert.h>

/* ---- a standard MIDI file, so the result can be played ------------------ */

static void put_be32(FILE *f, uint32_t v)
{
    fputc((v >> 24) & 0xFF, f); fputc((v >> 16) & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);  fputc(v & 0xFF, f);
}

static void put_varlen(FILE *f, uint32_t v)
{
    uint8_t b[5]; int n = 0;
    b[n++] = v & 0x7F;
    while ((v >>= 7)) b[n++] = (uint8_t)((v & 0x7F) | 0x80);
    while (n) fputc(b[--n], f);
}

static void write_mid(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  cannot write %s\n", path); return; }

    /* 60 ticks per quarter note at 500,000 us per quarter is 120 ticks a
     * second, which is the rate the events are already in. */
    fwrite("MThd", 1, 4, f); put_be32(f, 6);
    fputc(0, f); fputc(0, f);                 /* format 0 */
    fputc(0, f); fputc(1, f);                 /* one track */
    fputc(0, f); fputc(60, f);                /* division */

    fwrite("MTrk", 1, 4, f);
    long len_at = ftell(f);
    put_be32(f, 0);
    long body = ftell(f);

    put_varlen(f, 0);                         /* tempo: 120 bpm */
    fputc(0xFF, f); fputc(0x51, f); fputc(3, f);
    fputc(0x07, f); fputc(0xA1, f); fputc(0x20, f);

    uint32_t last = 0;
    for (unsigned i = 0; i < g_ev_n; i++) {
        put_varlen(f, g_ev[i].tick - last);
        last = g_ev[i].tick;
        uint8_t st = g_ev[i].msg[0];
        fputc(st, f);
        fputc(g_ev[i].msg[1], f);
        if ((st & 0xF0) != 0xC0 && (st & 0xF0) != 0xD0) fputc(g_ev[i].msg[2], f);
    }
    put_varlen(f, 0);                         /* end of track */
    fputc(0xFF, f); fputc(0x2F, f); fputc(0, f);

    long end = ftell(f);
    fseek(f, len_at, SEEK_SET);
    put_be32(f, (uint32_t)(end - body));
    fclose(f);
    printf("  wrote %s\n", path);
}

/* ---- the checks -------------------------------------------------------- */

static int check_one(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("%s: cannot open\n", path); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n);
    if (fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(data); return 1; }
    fclose(f);

    uint32_t elen = 0;
    const uint8_t *evnt = find_chunk(data, data + n, "EVNT", &elen, 0);
    if (!evnt) { printf("%s: no EVNT chunk\n", path); free(data); return 1; }

    if (!g_ev) g_ev = calloc(MAX_EVENTS, sizeof *g_ev);
    if (!parse_evnt(evnt, evnt + elen)) { printf("%s: no events\n", path); free(data); return 1; }

    /* Note-ons and note-offs must pair up. A velocity-zero note-on is a
     * note-off in MIDI, and the parser never emits one, but count it that way
     * so the check does not depend on that staying true. */
    int open_notes[16][128];
    memset(open_notes, 0, sizeof open_notes);
    uint32_t last_tick = 0;
    unsigned n_on = 0, n_off = 0, n_other = 0;
    int bad = 0;

    for (unsigned i = 0; i < g_ev_n; i++) {
        const Ev *e = &g_ev[i];
        assert(e->tick >= last_tick && "events are sorted by tick");
        last_tick = e->tick;

        uint8_t st = e->msg[0], ch = st & 0x0F, kind = st & 0xF0;
        assert(st >= 0x80 && st < 0xF0 && "a channel-voice status byte");
        assert(e->msg[1] < 128 && "data byte in range");

        if (kind == 0x90 && e->msg[2] > 0) {
            open_notes[ch][e->msg[1]]++;
            n_on++;
        } else if (kind == 0x80 || kind == 0x90) {
            if (--open_notes[ch][e->msg[1]] < 0) {
                printf("%s: note off with nothing playing, ch %d note %d\n",
                       path, ch, e->msg[1]);
                bad = 1;
            }
            n_off++;
        } else {
            n_other++;
        }
    }

    for (int c = 0; c < 16 && !bad; c++)
        for (int k = 0; k < 128; k++)
            if (open_notes[c][k]) {
                printf("%s: %d note(s) left on, ch %d note %d\n",
                       path, open_notes[c][k], c, k);
                bad = 1;
            }

    printf("%s: %u events (%u on, %u off, %u other), %u ticks, %.1fs%s\n",
           path, g_ev_n, n_on, n_off, n_other, last_tick,
           last_tick / (double)TICKS_PER_SEC, bad ? "  FAILED" : "  ok");

    if (!bad) {
        const char *base = path;
        for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
        char out[256];
        snprintf(out, sizeof out, "work/%s.mid", base);
        write_mid(out);
    }

    free(data);
    return bad;
}

int main(int argc, char **argv)
{
    static const char *const all[] = {
        "original/AUCTION.XMI",  "original/DINO5A.XMI",  "original/DINOCITY.XMI",
        "original/DINOFKEY.XMI", "original/DINOPARK.XMI", "original/EXTINCT.XMI",
        "original/MUSICTST.XMI",
    };
    int bad = 0;
    if (argc > 1) {
        for (int i = 1; i < argc; i++) bad |= check_one(argv[i]);
    } else {
        for (size_t i = 0; i < sizeof all / sizeof *all; i++) bad |= check_one(all[i]);
    }
    puts(bad ? "FAILED" : "all ok");
    return bad;
}
