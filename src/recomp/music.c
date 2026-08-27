/*
 * music.c - an XMI player, for the sequences the game registers with AIL.
 *
 * XMI is IFF: a `FORM XDIR` holding a count, then a `CAT XMID` of one
 * `FORM XMID` per sequence, each with a `TIMB` of patches and an `EVNT` of
 * events. Chunk lengths are big-endian and padded to even.
 *
 * The event stream is MIDI with two differences, and both matter here:
 *
 *   - Delays are a run of bytes below 0x80 whose values add up, chaining while
 *     each is 0x7F. Not the variable-length quantity MIDI files use.
 *   - Note-on carries a DURATION after the velocity, and there is no note-off
 *     in the stream at all. The player owes every note its own note-off, which
 *     is why the events are collected and sorted rather than streamed: the
 *     note-off belongs at a tick the stream has not reached yet.
 *
 * Timing is a fixed 120 ticks per second. That is what the Miles player ran at,
 * and XMI files are written for it; the tempo meta events some of them carry
 * are not what set the rate. DINO_MUSIC_HZ overrides it if a piece sounds off.
 */
#include "music.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICKS_PER_SEC 120
#define MAX_EVENTS    16384

typedef struct { uint32_t tick; uint32_t order; uint8_t msg[3]; } Ev;

static Ev      *g_ev;
static unsigned g_ev_n;

/* ---- IFF walking ------------------------------------------------------- */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | p[3];
}

/* The first chunk with this tag, anywhere inside [p, end). FORM, CAT and LIST
 * carry a four-character type before their contents; everything else is a leaf.
 * Depth is bounded because a malformed file must not recurse forever. */
static const uint8_t *find_chunk(const uint8_t *p, const uint8_t *end,
                                 const char *tag, uint32_t *out_len, int depth)
{
    while (p + 8 <= end && depth < 8) {
        uint32_t len = be32(p + 4);
        const uint8_t *body = p + 8;
        if (len > (uint32_t)(end - body)) len = (uint32_t)(end - body);

        if (!memcmp(p, tag, 4)) { *out_len = len; return body; }

        if (!memcmp(p, "FORM", 4) || !memcmp(p, "CAT ", 4) ||
            !memcmp(p, "LIST", 4)) {
            const uint8_t *hit = find_chunk(body + 4, body + len, tag,
                                            out_len, depth + 1);
            if (hit) return hit;
        }
        p = body + len + (len & 1);           /* chunks pad to even */
    }
    return NULL;
}

/* ---- the event stream -------------------------------------------------- */

static uint32_t varlen(const uint8_t **pp, const uint8_t *end)
{
    uint32_t v = 0;
    while (*pp < end) {
        uint8_t b = *(*pp)++;
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return v;
}

static void emit(uint32_t tick, uint8_t a, uint8_t b, uint8_t c)
{
    if (g_ev_n >= MAX_EVENTS) return;
    Ev *e = &g_ev[g_ev_n];
    e->tick = tick;
    e->order = g_ev_n;
    e->msg[0] = a; e->msg[1] = b; e->msg[2] = c;
    g_ev_n++;
}

static int ev_cmp(const void *a, const void *b)
{
    const Ev *x = a, *y = b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    /* Stable: a note-off generated for an earlier note must not overtake the
     * note-on of a later one at the same tick. */
    return x->order < y->order ? -1 : 1;
}

static int parse_evnt(const uint8_t *p, const uint8_t *end)
{
    uint32_t tick = 0;
    g_ev_n = 0;

    while (p < end) {
        /* Delay: values below 0x80 accumulate, chaining while each is 0x7F. */
        while (p < end && *p < 0x80) {
            uint8_t b = *p++;
            tick += b;
            if (b < 0x7F) break;
        }
        if (p >= end) break;

        uint8_t st = *p++;
        if (st == 0xFF) {                              /* meta */
            if (p >= end) break;
            uint8_t type = *p++;
            uint32_t len = varlen(&p, end);
            if (type == 0x2F) break;                   /* end of track */
            if (len > (uint32_t)(end - p)) break;
            p += len;
        } else if (st == 0xF0 || st == 0xF7) {         /* sysex */
            uint32_t len = varlen(&p, end);
            if (len > (uint32_t)(end - p)) break;
            p += len;
        } else if ((st & 0xF0) == 0x90) {              /* note on + duration */
            if (p + 2 > end) break;
            uint8_t note = *p++, vel = *p++;
            uint32_t dur = varlen(&p, end);
            emit(tick, st, note, vel);
            emit(tick + dur, (uint8_t)(0x80 | (st & 0x0F)), note, 0);
        } else if (st >= 0x80) {                       /* other channel voice */
            int n = ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) ? 1 : 2;
            if (p + n > end) break;
            emit(tick, st, p[0], n > 1 ? p[1] : 0);
            p += n;
        } else {
            break;                                     /* not a status byte */
        }
    }

    if (g_ev_n) qsort(g_ev, g_ev_n, sizeof *g_ev, ev_cmp);
    return g_ev_n > 0;
}

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>

static HMIDIOUT g_midi;
static HANDLE   g_thread;
static volatile LONG g_stop;

static void all_notes_off(void)
{
    if (!g_midi) return;
    for (int ch = 0; ch < 16; ch++) {
        midiOutShortMsg(g_midi, (DWORD)(0xB0 | ch) | (123u << 8));  /* all notes off */
        midiOutShortMsg(g_midi, (DWORD)(0xB0 | ch) | (121u << 8));  /* reset controllers */
    }
}

static DWORD WINAPI player(LPVOID unused)
{
    (void)unused;
    int hz = TICKS_PER_SEC;
    { const char *e = getenv("DINO_MUSIC_HZ"); if (e && atoi(e) > 0) hz = atoi(e); }

    /* Sleep to absolute deadlines from one start time rather than to a delta
     * per event: a few hundred events of rounding is audible drift otherwise. */
    DWORD t0 = GetTickCount();
    for (unsigned i = 0; i < g_ev_n && !g_stop; i++) {
        DWORD due = t0 + (DWORD)((uint64_t)g_ev[i].tick * 1000 / (unsigned)hz);
        for (;;) {
            DWORD now = GetTickCount();
            if (g_stop || (long)(now - due) >= 0) break;
            DWORD wait = due - now;
            Sleep(wait > 20 ? 20 : wait);
        }
        if (g_stop) break;
        const uint8_t *m = g_ev[i].msg;
        midiOutShortMsg(g_midi, (DWORD)m[0] | ((DWORD)m[1] << 8) | ((DWORD)m[2] << 16));
    }
    all_notes_off();
    return 0;
}

void music_stop(void)
{
    if (g_thread) {
        InterlockedExchange(&g_stop, 1);
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    all_notes_off();
}

int music_play_xmi(const uint8_t *data, size_t len)
{
    { static int off = -1;
      if (off < 0) { const char *e = getenv("DINO_MUSIC"); off = e && !atoi(e); }
      if (off) return 0; }

    music_stop();

    uint32_t elen = 0;
    const uint8_t *evnt = find_chunk(data, data + len, "EVNT", &elen, 0);
    if (!evnt) return 0;

    if (!g_ev) { g_ev = calloc(MAX_EVENTS, sizeof *g_ev); if (!g_ev) return 0; }
    if (!parse_evnt(evnt, evnt + elen)) return 0;

    if (!g_midi && midiOutOpen(&g_midi, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_midi = NULL;
        fprintf(stderr, "[music] no MIDI output available\n");
        return 0;
    }

    InterlockedExchange(&g_stop, 0);
    g_thread = CreateThread(NULL, 0, player, NULL, 0, NULL);
    if (!g_thread) return 0;

    fprintf(stderr, "[music] %u events, %u ticks\n",
            g_ev_n, g_ev_n ? g_ev[g_ev_n - 1].tick : 0);
    return 1;
}

#else   /* not Windows: parse it, but there is nowhere to send it */

int  music_play_xmi(const uint8_t *d, size_t n) { (void)d; (void)n; return 0; }
void music_stop(void) { }

#endif
