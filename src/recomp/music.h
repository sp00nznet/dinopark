/*
 * music.h - play the XMI the game hands to Miles AIL.
 *
 * DinoPark drives Miles through INT 66h and registers a sequence with function
 * 0x704, whose argument turned out to be the loaded file itself -- the trace
 * showed `FORM....XDIR` sitting at the far pointer it passed. That is enough to
 * play: hand the bytes here and they go out of the host's MIDI port.
 *
 * Off Windows this is all no-ops; the game is silent and otherwise unaffected.
 */
#ifndef DINO_MUSIC_H
#define DINO_MUSIC_H

#include <stddef.h>
#include <stdint.h>

/* Take a copy of an XMI and start playing its first sequence. Any sequence
 * already playing is stopped. Returns 1 if playback started. */
int  music_play_xmi(const uint8_t *data, size_t len);

/* Silence the output and stop the player. */
void music_stop(void);

#endif
