// The "You!" label over your own fish in online co-op: the word with a small arrow under it,
// white with a dark outline, as premultiplied BGRA (drawn once with GDI). Also any text in a
// color without the arrow (the competitive modes' winner banner).
#pragma once
#include <windows.h>

typedef struct {
    int w, h;
    BYTE *bgra;                     // w * h * 4, premultiplied alpha
} Label;

// text_px: the letters' height in pixels. 0 on failure.
int label_make(Label *l, const char *text, int text_px);
// The text alone, in a color (0..255 each), with the same outline.
int label_make_text(Label *l, const char *text, int text_px, int r, int g, int b);
// Onto 32-bit pixels, the arrow's tip at (x, y); clipped to w x h.
void label_blit(const Label *l, BYTE *bits, int pitch, int w, int h, int x, int y);
