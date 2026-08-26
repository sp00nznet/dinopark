/*
 * video.h - put a 320x200 8-bit VGA framebuffer on screen.
 *
 * Win32 GDI rather than SDL2: the recomp already targets MSVC/Windows, an 8-bit
 * DIB takes the VGA palette directly, and it costs no dependency. Everything
 * degrades to a no-op elsewhere, so callers need no #ifdef.
 *
 * Palettes are VGA DAC format: 256 * 3 bytes, 6 bits per channel.
 */
#ifndef DINO_VIDEO_H
#define DINO_VIDEO_H

#include <stdint.h>

/* Open the window (idempotent). Returns 1 if there is one to draw into. */
int  vga_window_open(const char *title, int w, int h);

/* Copy a w*h byte framebuffer to the window through pal6. */
void vga_window_present(const uint8_t *pixels, const uint8_t *pal6);

/* Service the message queue. Returns 0 once the user has closed the window. */
int  vga_window_pump(void);
/* Pointer position in VGA pixels and a button bitmask (1 left, 2 right).
 * Off-Windows there is no window and no mouse: it reports the origin. */
void vga_window_mouse(int *x, int *y, int *buttons);
/* Next key as a PC scancode, 0x80 set on release, or -1 when none is waiting. */
int  vga_window_key(void);

/* Block until the window is closed (or return at once if there is none). */
void vga_window_wait(void);

/* Write the same framebuffer to a 24-bit BMP. Works with no window, so a
 * headless run can still be looked at afterwards. Returns 0 on success. */
int  vga_write_bmp(const char *path, const uint8_t *pixels,
                   int w, int h, const uint8_t *pal6);

#endif
