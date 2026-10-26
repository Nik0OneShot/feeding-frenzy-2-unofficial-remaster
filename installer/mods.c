// The Remaster's changes to the game's data (FF2.saf and splash/splashScreen.xml), made
// on the player's PC from their own copy, so no game file is ever handed out.
//
// PopCap's engine pairs name.jpg with a grayscale _name.png alpha mask. Fading the outer
// strips of an 800x600 backdrop lets it blend into the level or menu drawn behind it instead
// of ending in a hard vertical seam on a wide screen.
//
// - resource/shell/_shell_stageinfo.png: mask for the stage start / complete backdrop.
// - resource/shell/gamemap/_gamemap_bg.png + gamemapbase.xml: mask for the level map, the map
//   layout without alphaType="none" so the mask is used, and the menu painting behind it.
// - resource/hud/hud_bgtopext.png, hud_bgbottomext.png + hudlayout.xml: plain wood extensions
//   of the HUD bar (mirror-tiled from a strip without lettering) placed left and right of it,
//   so the bar reaches the screen edges while its lettering and panels stay unscaled; and
//   the co-op players' own scores.
// - resource/shell/options.xml: the Co-op checkbox and players 2 to 4's color buttons.
// - resource/themes/basetheme.xml: per-player sounds may play once per player at a time.
// - resource/stages/stagescreendata.xml: stage 16's intro points at a text layout that exists.
// - loose splash/splashScreen.xml: names the splash background "splashBg" so the patch can
//   scale it to cover the screen (scene.c).
#define COBJMACROS
#include <windows.h>
#include <wincodec.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mods.h"

typedef struct {
    char *p;
    size_t len;
} Text;

static char *mod_err;
static int mod_err_len;

static int fail(const char *fmt, const char *what)
{
    snprintf(mod_err, mod_err_len, fmt, what);
    return 0;
}

static Text entry_text(Saf *saf, const char *name)
{
    Text t = { NULL, 0 };
    SafEntry *e = saf_find(saf, name);
    if (!e) return t;
    t.p = malloc(e->size + 1);
    memcpy(t.p, e->data, e->size);
    t.p[e->size] = 0;
    t.len = e->size;
    return t;
}

static size_t count_of(const Text *t, const char *s)
{
    size_t n = 0, l = strlen(s);
    for (const char *p = t->p; (p = strstr(p, s)); p += l) n++;
    return n;
}

static int replace_once(Text *t, const char *old, const char *new, const char *what)
{
    if (!t->p || count_of(t, old) != 1) return fail("%s isn't as expected: is this the Steam version of the game?", what);
    char *at = strstr(t->p, old);
    size_t lo = strlen(old), ln = strlen(new), head = at - t->p;
    char *out = malloc(t->len - lo + ln + 1);
    memcpy(out, t->p, head);
    memcpy(out + head, new, ln);
    memcpy(out + head + ln, at + lo, t->len - head - lo + 1);
    free(t->p);
    t->p = out;
    t->len = t->len - lo + ln;
    return 1;
}

// The first line containing `key` (up to, not including, its '\n'), copied.
static char *line_with(const Text *t, const char *key)
{
    char *at = t->p ? strstr(t->p, key) : NULL;
    if (!at) return NULL;
    char *start = at, *end = strchr(at, '\n');
    while (start > t->p && start[-1] != '\n') start--;
    if (!end) end = t->p + t->len;
    char *line = malloc(end - start + 1);
    memcpy(line, start, end - start);
    line[end - start] = 0;
    return line;
}

static void put_text(Saf *saf, const char *name, Text *t)
{
    saf_put(saf, name, (BYTE *)t->p, (DWORD)t->len);
}

// --- PNG writing: uncompressed (stored) deflate, which every PNG reader takes ----------

static DWORD crc_table[256];

static DWORD crc32_of(DWORD crc, const BYTE *p, size_t n)
{
    if (!crc_table[1])
        for (DWORD i = 0; i < 256; i++) {
            DWORD c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static void be32(BYTE *p, DWORD v)
{
    p[0] = (BYTE)(v >> 24), p[1] = (BYTE)(v >> 16), p[2] = (BYTE)(v >> 8), p[3] = (BYTE)v;
}

// channels 1 (gray) or 4 (RGBA); pixels row after row.
static BYTE *png_write(int width, int height, int channels, const BYTE *pixels, DWORD *len)
{
    size_t row = (size_t)width * channels + 1, raw_len = row * height;
    BYTE *raw = malloc(raw_len);
    for (int y = 0; y < height; y++) {
        raw[y * row] = 0;
        memcpy(raw + y * row + 1, pixels + (size_t)y * width * channels, row - 1);
    }
    size_t blocks = (raw_len + 65534) / 65535, z_len = 2 + raw_len + blocks * 5 + 4;
    size_t total = 8 + 25 + 12 + z_len + 12;
    BYTE *png = malloc(total), *o = png;
    memcpy(o, "\x89PNG\r\n\x1a\n", 8), o += 8;
    be32(o, 13), memcpy(o + 4, "IHDR", 4);
    be32(o + 8, width), be32(o + 12, height);
    o[16] = 8, o[17] = channels == 1 ? 0 : 6, o[18] = o[19] = o[20] = 0;
    be32(o + 21, crc32_of(0, o + 4, 17)), o += 25;
    be32(o, (DWORD)z_len), memcpy(o + 4, "IDAT", 4);
    BYTE *z = o + 8, *zo = z;
    *zo++ = 0x78, *zo++ = 0x01;
    DWORD a = 1, b = 0;
    for (size_t at = 0; at < raw_len; at += 65535) {
        size_t n = raw_len - at < 65535 ? raw_len - at : 65535;
        *zo++ = at + n == raw_len;
        zo[0] = (BYTE)n, zo[1] = (BYTE)(n >> 8), zo[2] = (BYTE)~n, zo[3] = (BYTE)(~n >> 8), zo += 4;
        memcpy(zo, raw + at, n), zo += n;
        for (size_t i = 0; i < n; i++) a = (a + raw[at + i]) % 65521, b = (b + a) % 65521;
    }
    be32(zo, b << 16 | a), zo += 4;
    be32(zo, crc32_of(0, o + 4, z_len + 4));
    o = zo + 4;
    be32(o, 0), memcpy(o + 4, "IEND", 4), be32(o + 8, crc32_of(0, o + 4, 4));
    *len = (DWORD)total;
    free(raw);
    return png;
}

// --- Picture reading (Windows Imaging Component) -------------------------------------

// The picture as 32-bit BGRA, or 8-bit gray. 0 if it can't be read.
static BYTE *picture_read(const BYTE *data, DWORD len, int gray, int *width, int *height)
{
    IWICImagingFactory *f = NULL;
    IWICStream *stream = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    BYTE *pixels = NULL;
    UINT w, h;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f)))
        return NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(f, &stream)) &&
        SUCCEEDED(IWICStream_InitializeFromMemory(stream, (BYTE *)data, len)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &conv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame,
                                                 gray ? &GUID_WICPixelFormat8bppGray : &GUID_WICPixelFormat32bppBGRA,
                                                 WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(IWICFormatConverter_GetSize(conv, &w, &h))) {
        UINT stride = w * (gray ? 1 : 4);
        pixels = malloc((size_t)stride * h);
        if (FAILED(IWICFormatConverter_CopyPixels(conv, NULL, stride, stride * h, pixels))) {
            free(pixels);
            pixels = NULL;
        }
        *width = (int)w, *height = (int)h;
    }
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (stream) IWICStream_Release(stream);
    IWICImagingFactory_Release(f);
    return pixels;
}

// --- The masks and extensions ----------------------------------------------------------

static double smoothstep(double edge0, double edge1, double x)
{
    double t = (x - edge0) / (edge1 - edge0);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

static void put_edge_fade(Saf *saf, const char *name, int left, int right, int top, int bottom)
{
    const int width = 800, height = 600;
    BYTE *px = malloc(width * height);
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            double a = smoothstep(0, left, x) * smoothstep(0, right, width - 1 - x);
            if (top) a *= smoothstep(0, top, y);
            if (bottom) a *= smoothstep(0, bottom, height - 1 - y);
            px[y * width + x] = (BYTE)rint(255 * a); // round half to even, like the original tool
        }
    DWORD len;
    BYTE *png = png_write(width, height, 1, px, &len);
    free(px);
    saf_put(saf, name, png, len);
}

// Plain wood in hud_bgTop.jpg (no lettering or panels), found by scanning its columns.
#define HUD_WOOD_FROM  72
#define HUD_WOOD_TO    107
#define HUD_EXT_WIDTH  720   // covers the side of screens up to 32:9
#define HUD_EXT_OVERLAP 8    // how far each extension reaches over the bar's end, fading out

// Mirror-tiles the wood columns of a BGRA picture (with an optional gray alpha mask) into an
// RGBA strip whose ends fade out over HUD_EXT_OVERLAP + 2 pixels.
static int put_hud_extension(Saf *saf, const char *name, const char *jpg, const char *mask_name)
{
    SafEntry *e = saf_find(saf, jpg), *m = mask_name ? saf_find(saf, mask_name) : NULL;
    if (!e || (mask_name && !m)) return fail("%s is missing", e ? mask_name : jpg);
    int w, h, mw = 0, mh = 0;
    BYTE *bgra = picture_read(e->data, e->size, 0, &w, &h), *mask = m ? picture_read(m->data, m->size, 1, &mw, &mh) : NULL;
    if (!bgra || (m && (!mask || mw != w || mh != h))) return fail("%s can't be read", jpg);
    const int span = HUD_WOOD_TO - HUD_WOOD_FROM, fade = HUD_EXT_OVERLAP + 2;
    BYTE *out = malloc((size_t)HUD_EXT_WIDTH * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < HUD_EXT_WIDTH; x++) {
            int t = x % (2 * span), sx = HUD_WOOD_FROM + (t < span ? t : 2 * span - 1 - t);
            const BYTE *src = bgra + ((size_t)y * w + sx) * 4;
            BYTE *dst = out + ((size_t)y * HUD_EXT_WIDTH + x) * 4;
            double a = mask ? mask[y * w + sx] : 255;
            a = a * smoothstep(0, fade, x) * smoothstep(0, fade, HUD_EXT_WIDTH - 1 - x);
            dst[0] = src[2], dst[1] = src[1], dst[2] = src[0], dst[3] = (BYTE)(int)a;
        }
    DWORD len;
    BYTE *png = png_write(HUD_EXT_WIDTH, h, 4, out, &len);
    free(out), free(bgra), free(mask);
    saf_put(saf, name, png, len);
    return 1;
}

int mods_apply(Saf *saf, const BYTE *splash, DWORD splash_len, BYTE **splash_out, DWORD *splash_out_len,
               char *err, int err_len)
{
    char buf[4096];
    mod_err = err, mod_err_len = err_len;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    // Stage start / complete: the wooden frame's posts start ~40px from the left edge and
    // end ~55px from the right edge.
    put_edge_fade(saf, "resource/shell/_shell_stageinfo.png", 40, 52, 0, 0);

    // Level map: fade the dark blue border outside the torn parchment on all four sides.
    put_edge_fade(saf, "resource/shell/gamemap/_gamemap_bg.png", 36, 36, 22, 18);
    Text layout = entry_text(saf, "resource/shell/gamemapbase.xml");
    if (!replace_once(&layout, "gameMap_bg.jpg\" alphaType=\"none\"", "gameMap_bg.jpg\"", "gamemapbase.xml") ||
        // Behind the map, the main menu painting (scaled to cover by scene.c), so the faded
        // map edges blend into it even while a level loads and the menu backdrop is gone.
        !replace_once(&layout, "<compositeGroup>",
                      "<image name=\"ambientBg\" file=\"resource/shell/loadBg.jpg\" alphaType=\"none\" />\n\n\t<compositeGroup>",
                      "gamemapbase.xml"))
        return 0;
    put_text(saf, "resource/shell/gamemapbase.xml", &layout);

    // HUD bar extensions.
    if (!put_hud_extension(saf, "resource/hud/hud_bgtopext.png", "resource/hud/hud_bgtop.jpg", NULL) ||
        !put_hud_extension(saf, "resource/hud/hud_bgbottomext.png", "resource/hud/hud_bgbottom.jpg",
                           "resource/hud/_hud_bgbottom.png"))
        return 0;
    int left = -HUD_EXT_WIDTH + HUD_EXT_OVERLAP, right = 800 - HUD_EXT_OVERLAP;
    Text hud = entry_text(saf, "resource/hud/hudlayout.xml");
    const char *bottom = "<image name=\"bgBottom\" file=\"resource\\hud\\hud_bgBottom.jpg\" x=\"0\" y=\"72\" />";
    snprintf(buf, sizeof buf,
             "%s\n"
             "\t<image name=\"bgLeftExt\" file=\"resource/hud/hud_bgTopExt.png\" x=\"%d\" y=\"0\" />\n"
             "\t<image name=\"bgRightExt\" file=\"resource/hud/hud_bgTopExt.png\" x=\"%d\" y=\"0\" />\n"
             "\t<image name=\"bgBottomLeftExt\" file=\"resource/hud/hud_bgBottomExt.png\" x=\"%d\" y=\"72\" />\n"
             "\t<image name=\"bgBottomRightExt\" file=\"resource/hud/hud_bgBottomExt.png\" x=\"%d\" y=\"72\" />",
             bottom, left, right, left, right);
    if (!replace_once(&hud, bottom, buf, "hudlayout.xml")) return 0;
    // Co-op: each player's own points (coop.c fills them in and colors the others' in their
    // tints), under the shared score, just below the bar: two to a line (1 and 2, 3 and 4, ...).
    static const char *const score_colors[10] = { "255,230,131", "255,140,90", "110,240,110", "100,150,255",
                                                  "255,120,210", "255,225,80", "185,120,255", "80,220,255",
                                                  "255,85,85", "150,150,150" };
    char *score_line = line_with(&hud, "name=\"scoreText\"");
    if (!score_line) return fail("%s has no score text", "hudlayout.xml");
    int o = snprintf(buf, sizeof buf, "%s", score_line);
    for (int n = 1; n <= 10; n++)
        o += snprintf(buf + o, sizeof buf - o,
                      "\n\t<text name=\"p%dScoreText\" x=\"%d\" y=\"%d\" xAlign=\"right\" yAlign=\"middle\""
                      " font=\"textFontMed\" string=\"\" color=\"%s\" />",
                      n, 672 + 108 * ((n - 1) % 2), 104 + 20 * ((n - 1) / 2), score_colors[n - 1]);
    if (!replace_once(&hud, score_line, buf, "hudlayout.xml")) return 0;
    put_text(saf, "resource/hud/hudlayout.xml", &hud);

    // Options: a "Co-op:" checkbox (read and stored by the patch, scene.c / coop.c) and a row
    // of buttons that cycle players 2 to 4's colors (coop.c handles their actions) below the
    // existing rows, which move up to make room above the Done button.
    Text options = entry_text(saf, "resource/shell/options.xml");
    static const struct { const char *name; int old_y, new_y; } rows[] = {
        { "soundOptionsGrp", -110, -132 },   { "musicOptionsGrp", -60, -94 },     { "fullscreenOptionGrp", -10, -56 },
        { "mouseSpeedOptionGrp", 40, -18 }, { "screenDetailOptionGrp", 90, 24 },
    };
    for (int i = 0; i < (int)(sizeof rows / sizeof *rows); i++) {
        char from[96], to[96];
        snprintf(from, sizeof from, "<group name=\"%s\" y=\"%d\"", rows[i].name, rows[i].old_y);
        snprintf(to, sizeof to, "<group name=\"%s\" y=\"%d\"", rows[i].name, rows[i].new_y);
        if (!replace_once(&options, from, to, "options.xml")) return 0;
    }
    o = snprintf(buf, sizeof buf, "<group name=\"colorOptionGrp\" y=\"112\" x=\"0\" >\n");
    for (int n = 2; n <= 4; n++)
        o += snprintf(buf + o, sizeof buf - o,
                      "\t\t\t\t\t<comboButton name=\"p%dColorBtn\" x=\"%d\" y=\"0\" action=\"p%dColor\" string=\"P%d\""
                      " font=\"tinyBtnFont\" upColor=\"textEmphasisColor\" overColor=\"white\" xAlign=\"center\""
                      " yAlign=\"middle\" overSound=\"resource/sounds/mouseOver.ogg\""
                      " activeSound=\"resource/sounds/mouseDown.ogg\" upImg=\"tinyBtnA_normal\" overImg=\"tinyBtnA_high\""
                      " pivot=\"center\" imageOffset=\"1,0\" />\n",
                      n, 136 * (n - 3), n, n);
    snprintf(buf + o, sizeof buf - o, "\t\t\t\t</group>\n\n\t\t\t</group> <!-- button group -->");
    if (!replace_once(&options, "</group> <!-- button group -->", buf, "options.xml") ||
        !replace_once(&options, "</group> <!-- button group -->",
                      "<group name=\"coopOptionGrp\" y=\"70\" x=\"-40\" >\n"
                      "\t\t\t\t\t<text name=\"coopTitleText\" x=\"0\" y=\"1\" string=\"Co-op:\" xAlign=\"right\""
                      " yAlign=\"middle\" font=\"textFontMed\" />\n"
                      "\t\t\t\t\t<checkBox pivot=\"center\" name=\"coopOptionCheck\" action=\"toggleCoop\""
                      " uncheckedImg=\"checkBtn_normal\" uncheckedOverImg=\"checkBtn_high\""
                      " checkedImg=\"checkBtnChecked_normal\" checkedOverImg=\"checkBtnChecked_high\""
                      " x=\"24\" y=\"0\" />\n"
                      "\t\t\t\t</group>\n\n\t\t\t</group> <!-- button group -->",
                      "options.xml"))
        return 0;
    put_text(saf, "resource/shell/options.xml", &options);

    // Sounds: per-player action sounds may play once for each player at the same time (co-op).
    Text theme = entry_text(saf, "resource/themes/basetheme.xml");
    static const char *const sounds[] = { "playerDash", "playerFlip", "playerSuck", "playerPoisoned", "playerStunned" };
    for (int i = 0; i < (int)(sizeof sounds / sizeof *sounds); i++) {
        char key[64];
        snprintf(key, sizeof key, "name=\"%s\"", sounds[i]);
        char *line = line_with(&theme, key);
        if (!line) return fail("basetheme.xml has no %s sound", sounds[i]);
        char *changed = _strdup(line);
        for (char *c = changed; (c = strstr(c, "count=\"1\"")); c += 9) c[7] = '2';
        if (!replace_once(&theme, line, changed, "basetheme.xml")) return 0;
    }
    put_text(saf, "resource/themes/basetheme.xml", &theme);

    // Stage 16's intro names a text layout the game never shipped (1chm2chLayout.xml, so its
    // text fell back to default spots). Its second paragraph is empty, and the 1ch* layouts
    // all place the first one alike, so 1chm2rhLayout.xml (one letter off) does.
    Text screens = entry_text(saf, "resource/stages/stagescreendata.xml");
    if (!replace_once(&screens, "textLayout=\"resource/stages/text/1chm2chLayout.xml\"",
                      "textLayout=\"resource/stages/text/1chm2rhLayout.xml\"", "stagescreendata.xml"))
        return 0;
    put_text(saf, "resource/stages/stagescreendata.xml", &screens);

    Text splash_text = { malloc(splash_len + 1), splash_len };
    memcpy(splash_text.p, splash, splash_len);
    splash_text.p[splash_len] = 0;
    if (!replace_once(&splash_text, "<image file=\"splash/splashBg.jpg\"", "<image name=\"splashBg\" file=\"splash/splashBg.jpg\"",
                      "splashScreen.xml"))
        return 0;
    *splash_out = (BYTE *)splash_text.p;
    *splash_out_len = (DWORD)splash_text.len;
    return 1;
}
