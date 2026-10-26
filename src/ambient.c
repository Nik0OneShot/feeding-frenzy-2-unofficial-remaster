// Ambient fill: when nothing was drawn beside the centered 800x600 UI area (loading screens,
// menus without a backdrop behind them), fill that side with a blurred, darkened copy of the
// picture, scaled to cover the screen, like video players do around portrait videos.
#include <windows.h>
#include <string.h>
#include "ambient.h"

#define UI_WIDTH  800
#define UI_HEIGHT 600
#define CELL      16                       // the picture is averaged in CELLxCELL blocks
#define GRID_W    (UI_WIDTH / CELL)        // 50
#define GRID_H    (UI_HEIGHT / CELL + 1)   // 38 (the last row is partial)
#define DARKEN    140                      // out of 256

// IDirectDrawSurface vtable: Lock = 25, Unlock = 32 (same in every interface version).
typedef HRESULT (WINAPI *Lock_t)(void *self, RECT *rect, void *desc, DWORD flags, HANDLE event);
typedef HRESULT (WINAPI *Unlock_t)(void *self, void *rect);
#define DDLOCK_WAIT 1
#define DESC_PITCH      16
#define DESC_SURFACE    36
#define DESC_BIT_COUNT  84

static int g_width, g_shift;

void ambient_init(int width, int shift)
{
    g_width = width;
    g_shift = shift;
}

// All sampled pixels in [x0, x1) are black: nothing was drawn there this frame.
static int side_is_empty(const BYTE *bits, int pitch, int x0, int x1)
{
    for (int y = 8; y < UI_HEIGHT; y += 37)
        for (int x = x0; x < x1; x += (x1 - x0) / 5 + 1)
            if (*(const DWORD *)(bits + y * pitch + x * 4) & 0xffffff) return 0;
    return 1;
}

static void build_grid(const BYTE *bits, int pitch, int grid[GRID_H][GRID_W][3])
{
    for (int gy = 0; gy < GRID_H; gy++)
        for (int gx = 0; gx < GRID_W; gx++) {
            int sum[3] = { 0, 0, 0 }, n = 0;
            for (int y = gy * CELL; y < gy * CELL + CELL && y < UI_HEIGHT; y += 2)
                for (int x = gx * CELL; x < gx * CELL + CELL; x += 2) {
                    DWORD p = *(const DWORD *)(bits + y * pitch + (g_shift + x) * 4);
                    sum[0] += p & 0xff, sum[1] += (p >> 8) & 0xff, sum[2] += (p >> 16) & 0xff;
                    n++;
                }
            for (int c = 0; c < 3; c++) grid[gy][gx][c] = n ? sum[c] / n : 0;
        }
    // Two box-blur passes over the 50x38 grid.
    static int tmp[GRID_H][GRID_W][3];
    for (int pass = 0; pass < 2; pass++) {
        memcpy(tmp, grid, sizeof tmp);
        for (int gy = 0; gy < GRID_H; gy++)
            for (int gx = 0; gx < GRID_W; gx++)
                for (int c = 0; c < 3; c++) {
                    int sum = 0, n = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int y = gy + dy, x = gx + dx;
                            if (y < 0 || y >= GRID_H || x < 0 || x >= GRID_W) continue;
                            sum += tmp[y][x][c], n++;
                        }
                    grid[gy][gx][c] = sum / n;
                }
    }
}

// alpha: 256 replaces what is there, less blends over it.
static void fill_side(BYTE *bits, int pitch, int x0, int x1, int grid[GRID_H][GRID_W][3], int alpha)
{
    // Screen pixel -> picture pixel, with the picture scaled to cover the whole width.
    float inv = (float)UI_WIDTH / g_width;
    for (int y = 0; y < UI_HEIGHT; y++) {
        float gy = ((y - UI_HEIGHT / 2) * inv + UI_HEIGHT / 2) / CELL - 0.5f;
        int y0 = (int)gy;
        float fy = gy - y0;
        if (gy < 0) y0 = 0, fy = 0;
        if (y0 >= GRID_H - 1) y0 = GRID_H - 2, fy = 1;
        DWORD *row = (DWORD *)(bits + y * pitch);
        for (int x = x0; x < x1; x++) {
            float gx = ((x - g_width / 2) * inv + UI_WIDTH / 2) / CELL - 0.5f;
            int gx0 = (int)gx;
            float fx = gx - gx0;
            if (gx < 0) gx0 = 0, fx = 0;
            if (gx0 >= GRID_W - 1) gx0 = GRID_W - 2, fx = 1;
            int rgb[3];
            for (int c = 0; c < 3; c++) {
                float top = grid[y0][gx0][c] + (grid[y0][gx0 + 1][c] - grid[y0][gx0][c]) * fx;
                float bottom = grid[y0 + 1][gx0][c] + (grid[y0 + 1][gx0 + 1][c] - grid[y0 + 1][gx0][c]) * fx;
                rgb[c] = (int)(top + (bottom - top) * fy) * DARKEN >> 8;
            }
            if (alpha < 256) {
                DWORD old = row[x];
                for (int c = 0; c < 3; c++) {
                    int o = (old >> (8 * c)) & 0xff;
                    rgb[c] = o + ((rgb[c] - o) * alpha >> 8);
                }
            }
            row[x] = (DWORD)rgb[0] | (DWORD)rgb[1] << 8 | (DWORD)rgb[2] << 16;
        }
    }
}

// Locks a 32-bit surface; returns its pixels (NULL if that fails, nothing to unlock then).
static BYTE *lock_surface(void **surface, int *pitch)
{
    BYTE desc[124];
    memset(desc, 0, sizeof desc);
    *(DWORD *)desc = sizeof desc; // DDSURFACEDESC2; older interfaces want DDSURFACEDESC
    Lock_t lock = (Lock_t)(*(void ***)surface)[25];
    if (lock(surface, NULL, desc, DDLOCK_WAIT, NULL) != 0) {
        memset(desc, 0, sizeof desc);
        *(DWORD *)desc = 108;
        if (lock(surface, NULL, desc, DDLOCK_WAIT, NULL) != 0) return NULL;
    }
    BYTE *bits = *(BYTE **)(desc + DESC_SURFACE);
    *pitch = *(int *)(desc + DESC_PITCH);
    if (!bits || *(DWORD *)(desc + DESC_BIT_COUNT) != 32) {
        ((Unlock_t)(*(void ***)surface)[32])(surface, NULL);
        return NULL;
    }
    return bits;
}

BYTE *ambient_lock(void **surface, int *pitch)
{
    return lock_surface(surface, pitch);
}

void ambient_unlock(void **surface)
{
    ((Unlock_t)(*(void ***)surface)[32])(surface, NULL);
}

// --- Co-op zoom surround -----------------------------------------------------------
// The scenery's picture (below the HUD) is averaged into a ZGRID_W x ZGRID_H grid and
// blurred; around the scenery the screen shows that picture enlarged to cover it, darkened
// like the menus' backdrop (ambient_fill). The grid is taken after a frame is drawn and
// fills the next frame before it's drawn, so everything drawn over the surround (the
// see-through water in front, the HUD) lands on it, and nothing is left black.
#define ZGRID_W     64
#define ZGRID_H     24
#define SAMPLE_TOP  100      // below the HUD bar

static int zgrid[ZGRID_H][ZGRID_W][3];
static int zgrid_ok;
static RECT zrect;           // the sampled area

static void blur_zgrid(void)
{
    static int tmp[ZGRID_H][ZGRID_W][3];
    for (int pass = 0; pass < 3; pass++) {
        memcpy(tmp, zgrid, sizeof tmp);
        for (int gy = 0; gy < ZGRID_H; gy++)
            for (int gx = 0; gx < ZGRID_W; gx++)
                for (int c = 0; c < 3; c++) {
                    int sum = 0, n = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int y = gy + dy, x = gx + dx;
                            if (y < 0 || y >= ZGRID_H || x < 0 || x >= ZGRID_W) continue;
                            sum += tmp[y][x][c], n++;
                        }
                    zgrid[gy][gx][c] = sum / n;
                }
    }
}

void ambient_zoom_capture(void **surface, const RECT *content)
{
    RECT r = *content;
    if (r.left < 0) r.left = 0;
    if (r.right > g_width) r.right = g_width;
    if (r.top < SAMPLE_TOP) r.top = SAMPLE_TOP;
    if (r.bottom > UI_HEIGHT) r.bottom = UI_HEIGHT;
    zgrid_ok = 0;
    if (!surface || r.right - r.left < ZGRID_W || r.bottom - r.top < ZGRID_H) return;
    int pitch;
    BYTE *bits = lock_surface(surface, &pitch);
    if (!bits) return;
    int w = r.right - r.left, h = r.bottom - r.top;
    for (int gy = 0; gy < ZGRID_H; gy++)
        for (int gx = 0; gx < ZGRID_W; gx++) {
            int y0 = r.top + h * gy / ZGRID_H, y1 = r.top + h * (gy + 1) / ZGRID_H;
            int x0 = r.left + w * gx / ZGRID_W, x1 = r.left + w * (gx + 1) / ZGRID_W;
            int sum[3] = { 0, 0, 0 }, n = 0;
            for (int y = y0; y < y1; y += 3)
                for (int x = x0; x < x1; x += 3) {
                    DWORD p = *(const DWORD *)(bits + y * pitch + x * 4);
                    sum[0] += p & 0xff, sum[1] += (p >> 8) & 0xff, sum[2] += (p >> 16) & 0xff;
                    n++;
                }
            for (int c = 0; c < 3; c++) zgrid[gy][gx][c] = n ? sum[c] / n : 0;
        }
    ((Unlock_t)(*(void ***)surface)[32])(surface, NULL);
    blur_zgrid();
    zrect = r;
    zgrid_ok = 1;
}

static DWORD surround_color(int x, int y)
{
    float gx = (float)x * ZGRID_W / g_width - 0.5f;
    float gy = (float)y * ZGRID_H / UI_HEIGHT - 0.5f;
    if (gx < 0) gx = 0;
    if (gy < 0) gy = 0;
    if (gx > ZGRID_W - 1.001f) gx = ZGRID_W - 1.001f;
    if (gy > ZGRID_H - 1.001f) gy = ZGRID_H - 1.001f;
    int x0 = (int)gx, y0 = (int)gy;
    float fx = gx - x0, fy = gy - y0;
    float k = DARKEN / 256.0f;
    int rgb[3];
    for (int c = 0; c < 3; c++) {
        float top = zgrid[y0][x0][c] + (zgrid[y0][x0 + 1][c] - zgrid[y0][x0][c]) * fx;
        float bottom = zgrid[y0 + 1][x0][c] + (zgrid[y0 + 1][x0 + 1][c] - zgrid[y0 + 1][x0][c]) * fx;
        rgb[c] = (int)((top + (bottom - top) * fy) * k);
    }
    return (DWORD)rgb[0] | (DWORD)rgb[1] << 8 | (DWORD)rgb[2] << 16;
}

void ambient_zoom_fill(void **surface, const RECT *content)
{
    if (!surface || !zgrid_ok) return;
    if (content->left <= 0 && content->top <= 0 && content->right >= g_width && content->bottom >= UI_HEIGHT) return;
    int pitch;
    BYTE *bits = lock_surface(surface, &pitch);
    if (!bits) return;
    for (int y = 0; y < UI_HEIGHT; y++) {
        DWORD *row = (DWORD *)(bits + y * pitch);
        int inside = y >= content->top && y < content->bottom;
        int skip0 = inside ? (content->left > 0 ? content->left : 0) : g_width;
        int skip1 = inside ? (content->right < g_width ? content->right : g_width) : g_width;
        for (int x = 0; x < g_width; x++) {
            if (x == skip0) {
                x = skip1 - 1;
                continue;
            }
            row[x] = surround_color(x, y);
        }
    }
    ((Unlock_t)(*(void ***)surface)[32])(surface, NULL);
}

void ambient_fill(void **surface)
{
    if (!surface || g_shift < 8) return;
    int pitch;
    BYTE *bits = lock_surface(surface, &pitch);
    if (!bits) return;
    int left = side_is_empty(bits, pitch, 0, g_shift);
    int right = side_is_empty(bits, pitch, g_shift + UI_WIDTH, g_width);
    if (left || right) {
        static int grid[GRID_H][GRID_W][3];
        build_grid(bits, pitch, grid);
        if (left) fill_side(bits, pitch, 0, g_shift, grid, 256);
        if (right) fill_side(bits, pitch, g_shift + UI_WIDTH, g_width, grid, 256);
    }
    ((Unlock_t)(*(void ***)surface)[32])(surface, NULL);
}

// Over the level beside a stage complete panel: the panel's own blurred, darkened picture,
// faded in with it, so the sides match the panel instead of showing the level.
void ambient_panel_fill(void **surface, float opacity)
{
    if (!surface || g_shift < 8 || opacity <= 0) return;
    int pitch;
    BYTE *bits = lock_surface(surface, &pitch);
    if (!bits) return;
    static int grid[GRID_H][GRID_W][3];
    build_grid(bits, pitch, grid);
    int alpha = (int)(opacity * 256);
    if (alpha > 256) alpha = 256;
    fill_side(bits, pitch, 0, g_shift, grid, alpha);
    fill_side(bits, pitch, g_shift + UI_WIDTH, g_width, grid, alpha);
    ((Unlock_t)(*(void ***)surface)[32])(surface, NULL);
}
