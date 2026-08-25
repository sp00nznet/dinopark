/* video.c - see video.h. */
#include "video.h"
#include <stdio.h>
#include <string.h>

/* VGA DAC channels are 6-bit; replicate the high bits so 63 reaches 255. */
static unsigned char chan8(unsigned char v6) { return (unsigned char)((v6 << 2) | (v6 >> 4)); }

int vga_write_bmp(const char *path, const unsigned char *pixels,
                  int w, int h, const unsigned char *pal6)
{
    int pad = (4 - (w * 3) % 4) % 4;                 /* BMP rows are DWORD-aligned */
    unsigned size = 54 + (unsigned)(w * 3 + pad) * h;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    unsigned char hdr[54];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(&hdr[2], &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(&hdr[18], &w, 4);
    memcpy(&hdr[22], &h, 4);                         /* positive: bottom-up */
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    unsigned char zero[3] = {0, 0, 0};
    for (int y = h - 1; y >= 0; y--) {               /* BMP stores the last row first */
        for (int x = 0; x < w; x++) {
            const unsigned char *c = &pal6[pixels[y * w + x] * 3];
            unsigned char bgr[3] = { chan8(c[2]), chan8(c[1]), chan8(c[0]) };
            fwrite(bgr, 1, 3, f);
        }
        if (pad) fwrite(zero, 1, pad, f);
    }
    fclose(f);
    return 0;
}

#ifdef _WIN32
#include <windows.h>

static HWND g_wnd;
static int  g_w, g_h, g_scale = 3, g_closed;
static struct { BITMAPINFOHEADER h; RGBQUAD pal[256]; } g_dib;

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_CLOSE || m == WM_DESTROY ||
        (m == WM_KEYDOWN && wp == VK_ESCAPE)) {
        g_closed = 1;
        return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

int vga_window_open(const char *title, int w, int h)
{
    if (g_wnd) return 1;
    if (g_closed) return 0;                          /* closed once, stay closed */

    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "DinoParkVGA";
    RegisterClassA(&wc);

    RECT r = { 0, 0, w * g_scale, h * g_scale };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowA("DinoParkVGA", title ? title : "DinoPark",
                          WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                          r.right - r.left, r.bottom - r.top,
                          NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) return 0;

    g_w = w; g_h = h;
    g_dib.h.biSize = sizeof g_dib.h;
    g_dib.h.biWidth = w;
    g_dib.h.biHeight = -h;                           /* negative: top-down, like VGA */
    g_dib.h.biPlanes = 1;
    g_dib.h.biBitCount = 8;
    g_dib.h.biCompression = BI_RGB;
    g_dib.h.biClrUsed = 256;

    ShowWindow(g_wnd, SW_SHOW);
    UpdateWindow(g_wnd);
    return 1;
}

void vga_window_present(const unsigned char *pixels, const unsigned char *pal6)
{
    if (!g_wnd || !vga_window_pump()) return;

    for (int i = 0; i < 256; i++) {
        g_dib.pal[i].rgbRed   = chan8(pal6[i * 3 + 0]);
        g_dib.pal[i].rgbGreen = chan8(pal6[i * 3 + 1]);
        g_dib.pal[i].rgbBlue  = chan8(pal6[i * 3 + 2]);
    }
    /* An 8-bit DIB needs DWORD-aligned rows. 320 divides by 4, and so does every
     * VGA width we present, so the framebuffer can be handed over untouched. */
    HDC dc = GetDC(g_wnd);
    StretchDIBits(dc, 0, 0, g_w * g_scale, g_h * g_scale, 0, 0, g_w, g_h,
                  pixels, (BITMAPINFO *)&g_dib, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_wnd, dc);
}

int vga_window_pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (g_closed && g_wnd) { DestroyWindow(g_wnd); g_wnd = NULL; }
    return g_wnd != NULL;
}

void vga_window_wait(void)
{
    while (vga_window_pump()) Sleep(16);
}

#else   /* not Windows: the BMP is the output */

int  vga_window_open(const char *t, int w, int h) { (void)t; (void)w; (void)h; return 0; }
void vga_window_present(const unsigned char *p, const unsigned char *q) { (void)p; (void)q; }
int  vga_window_pump(void) { return 0; }
void vga_window_wait(void) { }

#endif
