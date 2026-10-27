#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "label.h"

#define OUTLINE 2                   // pixels of dark edge around the white
static const BYTE edge[3] = { 40, 24, 16 }; // B, G, R: dark navy

// Draws the text and the arrow in white on black into a DIB: a coverage mask.
static BYTE *mask(HDC dc, HBITMAP *bmp, int w, int h, const char *text, int text_w, int text_h, int dx, int dy, int arrow)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;     // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits;
    *bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!*bmp) return NULL;
    HGDIOBJ old = SelectObject(dc, *bmp);
    memset(bits, 0, (size_t)w * h * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    int x = (w - text_w) / 2 + dx, y = OUTLINE + 1 + dy;
    TextOutA(dc, x, y, text, lstrlenA(text));
    // the arrow: a small triangle under the word, pointing down at the fish
    if (arrow) {
        int a = text_h / 3, cx = w / 2 + dx, top = y + text_h;
        POINT tri[3] = { { cx - a, top }, { cx + a, top }, { cx, top + a } };
        HGDIOBJ brush = SelectObject(dc, GetStockObject(WHITE_BRUSH)), pen = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, tri, 3);
        SelectObject(dc, brush);
        SelectObject(dc, pen);
    }
    GdiFlush();
    SelectObject(dc, old);
    return bits;
}

static int make(Label *l, const char *text, int text_px, const int *color, int arrow)
{
    memset(l, 0, sizeof *l);
    HDC dc = CreateCompatibleDC(NULL);
    HFONT font = CreateFontA(-text_px, 0, 0, 0, FW_BLACK, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    SIZE size;
    GetTextExtentPoint32A(dc, text, lstrlenA(text), &size);
    int w = size.cx + 2 * OUTLINE + 4, h = size.cy + (arrow ? size.cy / 3 : 0) + 2 * OUTLINE + 3;
    HBITMAP fill_bmp, edge_bmp;
    BYTE *fill = mask(dc, &fill_bmp, w, h, text, size.cx, size.cy, 0, 0, arrow);
    BYTE *grown = fill ? calloc((size_t)w * h, 1) : NULL;
    // the outline: the same shape drawn shifted all around, the largest coverage kept
    for (int dy = -OUTLINE; dy <= OUTLINE && grown; dy++)
        for (int dx = -OUTLINE; dx <= OUTLINE; dx++) {
            if (dx * dx + dy * dy > OUTLINE * OUTLINE + 1) continue;
            BYTE *m = mask(dc, &edge_bmp, w, h, text, size.cx, size.cy, dx, dy, arrow);
            if (!m) continue;
            for (int i = 0; i < w * h; i++)
                if (m[i * 4] > grown[i]) grown[i] = m[i * 4];
            DeleteObject(edge_bmp);
        }
    if (grown) {
        l->w = w;
        l->h = h;
        l->bgra = malloc((size_t)w * h * 4);
        for (int i = 0; i < w * h; i++) {
            int f = fill[i * 4], o = grown[i];          // coverage 0..255
            int a = f + o * (255 - f) / 255;            // the color over the edge
            for (int c = 0; c < 3; c++)
                l->bgra[i * 4 + c] = (BYTE)(f * color[2 - c] / 255 + edge[c] * o * (255 - f) / (255 * 255));
            l->bgra[i * 4 + 3] = (BYTE)a;
        }
    }
    free(grown);
    if (fill) DeleteObject(fill_bmp);
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    return l->bgra != NULL;
}

int label_make(Label *l, const char *text, int text_px)
{
    static const int white[3] = { 255, 255, 255 };
    return make(l, text, text_px, white, 1);
}

int label_make_text(Label *l, const char *text, int text_px, int r, int g, int b)
{
    const int color[3] = { r, g, b };
    return make(l, text, text_px, color, 0);
}

void label_blit(const Label *l, BYTE *bits, int pitch, int w, int h, int x, int y)
{
    if (!l->bgra) return;
    int x0 = x - l->w / 2, y0 = y - l->h;
    for (int ly = 0; ly < l->h; ly++) {
        int sy = y0 + ly;
        if (sy < 0 || sy >= h) continue;
        BYTE *d = bits + (size_t)sy * pitch;
        const BYTE *s = l->bgra + (size_t)ly * l->w * 4;
        for (int lx = 0; lx < l->w; lx++) {
            int sx = x0 + lx, a = s[lx * 4 + 3];
            if (sx < 0 || sx >= w || !a) continue;
            BYTE *p = d + sx * 4;
            for (int c = 0; c < 3; c++) p[c] = (BYTE)(s[lx * 4 + c] + p[c] * (255 - a) / 255);
        }
    }
}
