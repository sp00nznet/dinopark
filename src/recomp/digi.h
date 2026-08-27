/*
 * digi.h - the digital half of the game's audio.
 *
 * The game hands Miles a DIGPAK SNDSTRUC describing a sample it has already
 * decompressed into its own heap. The runtime reads that struct and calls
 * digi_play with what it points at.
 *
 * DINO_SFX=0 turns it off.
 */
#ifndef DINO_DIGI_H
#define DINO_DIGI_H

#include <stdint.h>

/* Unsigned 8-bit mono PCM. Returns 1 if it started playing. */
int  digi_play(const uint8_t *pcm, unsigned len, unsigned rate);
void digi_stop(void);

#endif
