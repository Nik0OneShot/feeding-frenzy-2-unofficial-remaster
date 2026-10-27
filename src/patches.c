// Widescreen patches for Feeding Frenzy 2 Deluxe (Steam, popcapgame1.exe).
//
// How it fits together:
//  - The back buffer is WIDTH x 600 (WIDTH matches the monitor's shape).
//  - All menus and the HUD are authored for 800x600. The whole scene is drawn shifted
//    right by SHIFT = (WIDTH - 800) / 2, the game's layout code is told the screen is
//    800 wide, and mouse positions are shifted back, so UI is centered and clickable.
//  - Levels are drawn at their normal size over the whole screen. A level whose play area
//    is narrower than the screen gets a wider play area; backdrop layers narrower than the
//    view are scaled up individually and slide in proportion to their original size.
//  - scene.c adjusts a few full-screen menu backgrounds and the HUD while drawing.
#include <windows.h>
#include <stdio.h>
#include "log.h"
#include "crashlog.h"
#include "patches.h"
#include "scene.h"
#include "ambient.h"
#include "mouse.h"
#include "mice.h"
#include "coop.h"
#include "online.h"
#include "tweaks.h"
#include <stdlib.h>
#include <math.h>

// RelativeMouse::Recenter (0x42b8c0), replaced by mouse.c.
#define FN_RELATIVE_MOUSE_RECENTER 0x42b8c0
// PlayerFish::Physics (0x43b2e0) calls PlayerFish::ApplyDrag here; mouse.c scales it by time.
#define ADDR_PLAYER_DRAG_CALL 0x43b331
#define FN_PLAYER_DRAG        0x43ae90

#define GAME_HEIGHT 600
#define UI_WIDTH    800

// FeedingFrenzyTwo::GetAppConfig (0x48c080) stores the window size as immediates:
//   48c235: mov dword [esi+0x24], 800
//   48c23c: mov dword [esi+0x28], 600
#define ADDR_CONFIG_WIDTH  0x48c238
#define ADDR_CONFIG_HEIGHT 0x48c23f
// ...and the frame rate cap, twice (48c22e and 48c251: mov dword [esi+0x20], 60). The engine
// turns it into a frame interval (timer frequency / fps, 0x49eed0); updates get the real
// elapsed time (App::Frame, 0x438700), so a higher cap doesn't speed the game up.
#define ADDR_CONFIG_FPS_1  0x48c231
#define ADDR_CONFIG_FPS_2  0x48c254
// The game also sets the rate at runtime through EngineImpl::SetFrameRate (0x48da20, vtable
// slot 0xc8): 30 while a level loads (LevelLoadState::enter, 0x409910), then 60 again
// (LevelLoadState::exit, 0x48ddd0), and from the splash screen. Requests of 60 or more get
// the chosen rate instead; lower ones (loading) are left alone.
#define ADDR_ENGINE_SET_FRAME_RATE_SLOT 0x5614dc
#define FN_ENGINE_SET_FRAME_RATE        0x48da20

// DDrawDisplay::setupFullScreen (0x539da1) asks for a display mode with 0 bits per pixel
// (539e4b: push dword [esi+0xf8], which it set to 0). Under cnc-ddraw that primary surface
// doesn't match the 32-bit back buffer, so the game falls back to presenting every frame
// with GDI ("Incompatible primary surface. Forcing GDI blt."), which caps it at 60 fps.
// Asking for 32 bits keeps it on the DirectDraw blit path.
#define ADDR_DISPLAY_MODE_BPP 0x539e4b

// App::Frame (0x438700) draws the scene with `call SignalDraw` (0x435ab0, ret 4);
// eax = signal, [esp+4] = DrawContext*, edi = app.
#define ADDR_FRAME_DRAW_CALL 0x4389ab
#define FN_SIGNAL_DRAW       0x435ab0
#define APP_DISPLAY          0x2a4   // DDrawDisplay*
#define DISPLAY_BACK_SURFACE 0x78    // IDirectDrawSurface* the frame is drawn into
#define DRAWCTX_STATE_DEQUE  0x38    // std::deque<DrawState*>; state[0], [1] = x, y offset

// Stage::Setup (0x4146b0) calls LayerSet parallax setup (0x4858d0, stdcall, 7 args) with
// the play viewport rect, then positions the layer set at the rect's top-left; ebx = stage.
#define ADDR_STAGE_PARALLAX_CALL 0x4148d5
#define FN_LAYERSET_PARALLAX     0x4858d0
#define STAGE_VIEW_RECT          0x6c    // float viewport rect x0, y0, x1, y1
// Right after the parallax setup, Stage::Setup copies the base layer's panSize into the play
// area (stage+0x88: 0, 0, w, h) that fish spawn and roam in (0x414984..0x41499c).
#define WIDGET_POS               0x2c    // float x, y
#define WIDGET_SCALE_X           0x3c    // float, applied to children when drawn (0x443c20)
#define WIDGET_SCALE_Y           0x40
#define WIDGET_VTBL_SET_POSITION 0x14    // Widget::SetPosition(const Vector2f*)
#define LAYERSET_BASE_INDEX      0xd8    // int: the play layer (fish live in it)
#define LAYERSET_INFOS_BEGIN     0xbc    // std::vector<LayerInfo*> begin / end
#define LAYERSET_INFOS_END       0xc0
#define LAYERSET_PANS_BEGIN      0xcc    // std::vector<LayoutPan*>, same order as the infos
#define LAYERSET_PANS_END        0xd0
#define LAYERINFO_PAN_SIZE       0x20    // float panSize.x, .y
#define FN_RUNTIME_CHECK_FAILED  0x4a9ee6 // see crashlog.c
#define LAYERSET_WATER_LEVEL     0xdc    // int, play layer y of the water surface (set waterLevel=), or 0
#define LAYERSET_CAMERA          0xfc    // float camera x, y (LayerSet::UpdateCamera, 0x43b960)
// LayoutPan::ApplyCamera (0x43c090) moves its layout widget (+0x1c) to -camera * speed.
#define ADDR_LAYOUTPAN_APPLY_SLOT 0x557a0c
#define ADDR_LAYOUTPAN_POS_SLOT   0x557a08  // LayoutPan::GetPosition(float *out): its widget's
#define FN_LAYOUTPAN_POS          0x50a8b8
#define FN_LAYOUTPAN_APPLY        0x43c090
#define LAYOUTPAN_WIDGET          0x1c

// EngineImpl vtable slot for GetWidth (0x424720: mov eax, [ecx+0x1e4]; ret).
#define ADDR_ENGINE_GETWIDTH_SLOT 0x56143c
#define FN_ENGINE_GETWIDTH        0x424720
#define ENGINE_WIDTH              0x1e4

// Engine::ClampMouse (0x424730): esi = POINT* from a mouse message, edi = engine.
// Clamps to the screen and stores the result in engine+0x2b0 / +0x2b4.
#define FN_ENGINE_CLAMP_MOUSE 0x424730
#define ENGINE_HEIGHT         0x1e8
#define ENGINE_MOUSE_X        0x2b0
#define ENGINE_MOUSE_Y        0x2b4
// During play the mouse is in relative mode: Mouse::Recenter (0x42b8c0) moves the fish by
// (position - center) and warps the cursor back to the center, both in window pixels.
#define ENGINE_RELATIVE_MOUSE 0x2a8   // RelativeMouse*
#define RELATIVE_MOUSE_ACTIVE 0x50    // bool

static int g_width = UI_WIDTH;   // back buffer width
static int g_shift;              // scene x offset that centers the 800-wide UI

static int patch_bytes(DWORD addr, const void *expected, const void *value, int len)
{
    BYTE *p = (BYTE *)addr;
    DWORD old;
    if (IsBadReadPtr(p, len) || memcmp(p, expected, len) != 0) {
        log_printf("patch at %08lx skipped: unexpected code", addr);
        return 0;
    }
    VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, value, len);
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    return 1;
}

static int patch_u32(DWORD addr, DWORD expected, DWORD value)
{
    int ok = patch_bytes(addr, &expected, &value, 4);
    if (ok) log_printf("patch at %08lx: %08lx -> %08lx", addr, expected, value);
    return ok;
}

// Redirects a `call rel32` at `site` from `expected_target` to `hook`.
static int patch_call(DWORD site, DWORD expected_target, void *hook)
{
    BYTE *p = (BYTE *)site;
    if (IsBadReadPtr(p, 5) || p[0] != 0xe8 || site + 5 + *(DWORD *)(p + 1) != expected_target) {
        log_printf("call hook at %08lx skipped: unexpected code", site);
        return 0;
    }
    return patch_u32(site + 1, expected_target - (site + 5), (DWORD)hook - (site + 5));
}

// Replaces the first 5 bytes of a function with `jmp hook`.
static int patch_jmp(DWORD site, const BYTE expected[5], void *hook)
{
    BYTE jmp[5] = { 0xe9 };
    *(DWORD *)(jmp + 1) = (DWORD)hook - (site + 5);
    int ok = patch_bytes(site, expected, jmp, 5);
    if (ok) log_printf("jmp hook at %08lx", site);
    return ok;
}

// ff2wide.ini next to the game: [ff2wide] width=0 means "match the monitor".
static int ini_int(const char *key, int def)
{
    char path[MAX_PATH], *slash = path;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    for (char *p = path; *p; p++)
        if (*p == '\\') slash = p + 1;
    lstrcpyA(slash, "ff2wide.ini");
    return GetPrivateProfileIntA("ff2wide", key, def, path);
}

static float ini_float(const char *key, float def)
{
    char path[MAX_PATH], *slash = path, text[32];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    for (char *p = path; *p; p++)
        if (*p == '\\') slash = p + 1;
    lstrcpyA(slash, "ff2wide.ini");
    if (!GetPrivateProfileStringA("ff2wide", key, "", text, sizeof text, path)) return def;
    float v = (float)atof(text);
    return v > 0 ? v : def;
}

static int g_fps = 60;
typedef void (__thiscall *SetFrameRate_t)(BYTE *engine, int fps);

void __thiscall engine_set_frame_rate_hook(BYTE *engine, int fps)
{
    if (fps >= 60) fps = g_fps;
    ((SetFrameRate_t)FN_ENGINE_SET_FRAME_RATE)(engine, fps);
}

// [ff2wide] fps=0 means "match the monitor's refresh rate".
static int frame_rate(void)
{
    int fps = ini_int("fps", 0);
    if (fps <= 0) {
        DEVMODEA mode = { .dmSize = sizeof mode };
        fps = EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1
            ? (int)mode.dmDisplayFrequency : 60;
    }
    if (fps < 30) fps = 30;
    if (fps > 1000) fps = 1000;
    log_printf("frame rate cap %d", fps);
    return fps;
}

static int wide_width(void)
{
    int forced = ini_int("width", 0);
    if (forced >= 640) {
        log_printf("width forced to %d by ff2wide.ini", forced);
        return forced;
    }
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    int width = h > 0 ? (GAME_HEIGHT * w + h / 2) / h : UI_WIDTH;
    width += width & 1;
    if (width < UI_WIDTH) width = UI_WIDTH;
    log_printf("desktop %dx%d -> game %dx%d", w, h, width, GAME_HEIGHT);
    return width;
}

// --- Frame drawing: clear, shift the scene, paint bars ---------------------------

typedef struct { DWORD dwSize; BYTE pad[0x50 - 4]; DWORD dwFillColor; BYTE pad2[100 - 0x54]; } BltFx;
typedef HRESULT (WINAPI *SurfaceBlt_t)(void *self, RECT *dst, void *src, RECT *srcRect, DWORD flags, BltFx *fx);
#define DDBLT_COLORFILL 0x00000400
#define DDBLT_WAIT      0x01000000

static void fill_black(BYTE *app, RECT *rect)
{
    BYTE *display = *(BYTE **)(app + APP_DISPLAY);
    if (!display) return;
    void **surface = *(void ***)(display + DISPLAY_BACK_SURFACE);
    if (!surface) return;
    BltFx fx = { .dwSize = sizeof fx, .dwFillColor = 0 };
    SurfaceBlt_t blt = (SurfaceBlt_t)(*(void ***)surface)[5];
    blt(surface, rect, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
}

// The draw context's current state, as DrawContext::CurrentState (0x444140) finds it.
static float *draw_state(BYTE *ctx)
{
    BYTE *deque = ctx + DRAWCTX_STATE_DEQUE;
    void **map = *(void ***)(deque + 4);
    DWORD map_size = *(DWORD *)(deque + 8), first = *(DWORD *)(deque + 0xc), count = *(DWORD *)(deque + 0x10);
    if (!map || !count) return NULL;
    DWORD i = first + count - 1;
    if (i >= map_size) i -= map_size;
    return (float *)map[i];
}

// --- Frame timing -------------------------------------------------------------------
// Logs the measured frame rate every 5 seconds, with how long drawing the scene and
// presenting the frame took on average, to see what limits the frame rate.

static LONGLONG draw_start, draw_ticks, present_ticks;

static LONGLONG now_ticks(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void count_frame(void)
{
    static LARGE_INTEGER freq;
    static LONGLONG start;
    static int frames;
    LONGLONG now = now_ticks();
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        start = now;
    }
    frames++;
    double seconds = (double)(now - start) / freq.QuadPart;
    if (seconds >= 5) {
        double ms = 1000.0 / freq.QuadPart / frames;
        log_printf("measured %.1f fps (per frame: draw %.2f ms, present %.2f ms)",
                   frames / seconds, draw_ticks * ms, present_ticks * ms);
        frames = 0;
        start = now;
        draw_ticks = present_ticks = 0;
    }
}

// DDrawDisplay::present (0x438240, thiscall with one argument), timed.
#define ADDR_DISPLAY_PRESENT_SLOT 0x562724
#define FN_DISPLAY_PRESENT        0x438240
typedef void (__thiscall *Present_t)(BYTE *display, void *app);

// A point on the play layer as back-buffer pixels: the layer's absolute position plus the
// point, zoomed about the layer set like layer_to_overlay, and the UI's shift.
static int play_to_screen(const float *p, int *out);

// Online co-op labels each fish "You!" on its own player's screen: the frame goes to each
// guest with its own player's label spot (its viewer draws it), and only then gets player 1's.
static void you_label_spot(int which, int *spot, int *ok)
{
    float p[2];
    *ok = coop_label_spot(which, p) && play_to_screen(p, spot);
}

void __thiscall display_present_hook(BYTE *display, void *app)
{
    void **back = *(void ***)(display + DISPLAY_BACK_SURFACE);
    int spot[2], ok = 0, marks[ONLINE_MAX_GUESTS][2];
    for (int g = 0; g < ONLINE_MAX_GUESTS; g++) {
        ok = 0;
        if (online_connected()) you_label_spot(coop_guest_player(g), spot, &ok);
        marks[g][0] = ok ? spot[0] : ONLINE_NO_MARK;
        marks[g][1] = ok ? spot[1] : 0;
    }
    online_frame(back, marks);
    if (online_connected()) {
        you_label_spot(1, spot, &ok);
        if (ok) online_draw_you(back, spot[0], spot[1]);
    }
    LONGLONG t = now_ticks();
    ((Present_t)FN_DISPLAY_PRESENT)(display, app);
    present_ticks += now_ticks() - t;
}

static void zoom_surround(void **surface);
static void zoom_backdrop(BYTE *app);
static void mixed_layers_before(void);
static void mixed_layers_after(void);

void __cdecl before_scene_draw(BYTE *app, BYTE *ctx)
{
    draw_start = now_ticks();
    fill_black(app, NULL);
    zoom_backdrop(app);
    float *state = draw_state(ctx);
    if (state) state[0] += (float)g_shift;
    scene_before_draw(app);
    mixed_layers_before();
    coop_before_draw();
}

void __cdecl after_scene_draw(BYTE *app)
{
    count_frame();
    coop_after_draw();
    coop_frame(app);
    mixed_layers_after();
    scene_after_draw();
    BYTE *display = *(BYTE **)(app + APP_DISPLAY);
    if (display) {
        zoom_surround(*(void ***)(display + DISPLAY_BACK_SURFACE));
        ambient_fill(*(void ***)(display + DISPLAY_BACK_SURFACE));
        ambient_panel_fill(*(void ***)(display + DISPLAY_BACK_SURFACE), scene_complete_panel_opacity());
    }
    draw_ticks += now_ticks() - draw_start;
}

// Replaces `call SignalDraw`, keeping its contract (eax and [esp+4] in, callee pops 4).
void frame_draw_thunk(void);
__asm__(
    ".globl _frame_draw_thunk\n"
    "_frame_draw_thunk:\n"
    "    pushal\n"
    "    pushl 36(%esp)\n"          // DrawContext*
    "    pushl %edi\n"              // app
    "    call _before_scene_draw\n"
    "    addl $8, %esp\n"
    "    popal\n"
    "    pushl 4(%esp)\n"
    "    movl $0x435ab0, %ecx\n"
    "    call *%ecx\n"              // pops the copied argument
    "    pushal\n"
    "    pushl %edi\n"
    "    call _after_scene_draw\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    ret $4\n");

// --- Full-screen play view ---------------------------------------------------------
// The level is drawn at normal size over the whole screen width. The play view keeps the
// game's own top offset (75 below the HUD bar during play, 0 in the main menu), so the
// arena and camera stay clear of the HUD, but the scenery is drawn up to the screen's top
// edge behind the HUD:
//  - if the play layer is narrower or shorter than the view, its panSize is enlarged; the
//    stage copies that into the play area right after this call, so fish spawn and roam in
//    the wider area (play layers are all empty layouts, so nothing visual stretches);
//  - every other layer is scaled up as far as needed to cover the drawn area, and slides
//    as far as its original size would have, so far layers still move less than near ones.

BYTE *g_stage;
#define MAX_CLAMS 32
static BYTE *clams_moved[MAX_CLAMS];     // clams already moved to where they're drawn (place_slots)
static int clams_moved_count;
typedef void (__stdcall *LayerSetParallax_t)(BYTE *, float, float, float, float, float, float);

typedef struct {
    BYTE *pan;                // LayoutPan*
    int is_play;              // the play layer moves exactly with the camera, unscaled
    BYTE *layer_set;
    int mixed;                // scenery sized like the play layer, see stage_parallax_hook
    int art_count;
    BYTE *art[64];            // its own children at setup: the art, scaled with it
    float w, h;               // the layer's panSize
    float view_w, view_h;     // the play view at zoom 1, and the strip above it (behind
    float above;              // the HUD) the scenery also covers
    float range_x, range_y;   // camera range of the layer set it belongs to, at zoom 1
    float rate_x, rate_y;     // how far it slid per camera pixel in the original (parallax)
    float water;              // the level's water line (play layer y) if it's as tall as the
                              // level, so the art's surface stays on it; else 0
} PanLayout;

float g_zoom = 1;             // the layer set's draw scale (co-op camera)
static int camera_override;   // the co-op camera is in charge
static BYTE *g_layer_set;
static float layer_set_origin[2]; // its position when not zoomed

#define MAX_PANS 48
static PanLayout pans[MAX_PANS];
static int pan_next;
#define MAX_ART 64
#define CONTAINER_CHILDREN_BEGIN 0x9c
#define CONTAINER_CHILDREN_END   0xa0
static BYTE *g_play_widget;   // the current level's play layer widget
static BYTE *g_play_pan;      // and its LayoutPan
static BYTE *g_layer_set_now; // and its layer set

static PanLayout *find_pan(BYTE *pan)
{
    for (int i = 0; i < MAX_PANS; i++)
        if (pans[i].pan == pan) return &pans[i];
    return NULL;
}

void __thiscall layout_pan_apply_hook(BYTE *pan, float *camera);

// --- Clip fish to the arena ---------------------------------------------------------
// The play layer's widget is the "PlayGroup" all fish are added to (Stage::Setup names it,
// 0x41493e); its local y = 0 is the top of the arena. Originally the HUD hid everything
// above that line; now the scenery shows there, so fish above it (spawning, or leaving
// through the top) are clipped. The widget gets a private copy of its vtable whose
// ApplyTransform also pushes a clip rect, the way engine::ClipPane does (0x526368).
// Clip rects are in back-buffer pixels, compared after the draw offset is applied.

typedef void (__thiscall *ApplyTransform_t)(BYTE *widget, BYTE *ctx);
typedef void (__stdcall *PushClip_t)(BYTE *ctx, int *rect);
#define FN_DRAWCTX_PUSH_CLIP 0x52004a
#define VTBL_APPLY_TRANSFORM 0x6c
#define VTBL_COPY_SIZE       0x200

typedef struct {
    void *original;
    void **copy;
    ApplyTransform_t apply;
} ClipVtable;

#define MAX_CLIP_VTABLES 4
static ClipVtable clip_vtables[MAX_CLIP_VTABLES];
static int g_clip_floor; // HUD bottom during play, 0 in the main menu (set per stage)

void __thiscall play_group_apply_transform(BYTE *widget, BYTE *ctx)
{
    ClipVtable *v = NULL;
    for (int i = 0; i < MAX_CLIP_VTABLES; i++)
        if (clip_vtables[i].copy == *(void ***)widget) v = &clip_vtables[i];
    if (!v) return;
    v->apply(widget, ctx);
    float *state = draw_state(ctx);
    if (!state) return;
    // The arena's top edge, but never above the HUD's bottom edge during play: fish the
    // camera hasn't scrolled to yet would otherwise show (half hidden) behind the HUD.
    int top = (int)(state[1] + 0.5f);
    if (top < g_clip_floor) top = g_clip_floor;
    int rect[4] = { -10000, top, 20000, 10000 };
    ((PushClip_t)FN_DRAWCTX_PUSH_CLIP)(ctx, rect);
}

static void clip_play_group(BYTE *widget)
{
    void *vtable = *(void **)widget;
    ClipVtable *v = NULL;
    for (int i = 0; i < MAX_CLIP_VTABLES && !v; i++) {
        if (clip_vtables[i].copy == vtable) return; // already clipped
        if (clip_vtables[i].original == vtable || !clip_vtables[i].original) v = &clip_vtables[i];
    }
    if (!v) return;
    if (!v->original) {
        // Include the slot before the vtable: MSVC keeps the RTTI locator there, which
        // dynamic_cast reads.
        void **block = VirtualAlloc(NULL, VTBL_COPY_SIZE + 4, MEM_COMMIT, PAGE_READWRITE);
        if (!block) return;
        memcpy(block, (void **)vtable - 1, VTBL_COPY_SIZE + 4);
        v->copy = block + 1;
        v->apply = (ApplyTransform_t)v->copy[VTBL_APPLY_TRANSFORM / 4];
        v->copy[VTBL_APPLY_TRANSFORM / 4] = (void *)play_group_apply_transform;
        v->original = vtable;
        log_printf("play group clip installed (vtable %p)", vtable);
    }
    *(void ***)widget = v->copy;
}

// engine::ClipPane (<clipGroup bounds="...">) clips its children to its bounds (+0xa8..+0xb4,
// local x0, y0, x1, y1). Its ApplyTransform (0x526368, vtable slot 0x560c20) pushes a rect
// computed from its absolute position without scale or the scene shift, so in a scaled-up
// scenery layer (the pier's sky and water) it cut the picture off at its unscaled size.
// The rect is now taken from the draw state, which has both.
#define ADDR_CLIPPANE_APPLY_SLOT 0x560c20
#define FN_CLIPPANE_APPLY        0x526368
#define FN_WIDGET_APPLY_TRANSFORM 0x443c20
#define CLIPPANE_BOUNDS          0xa8

void __thiscall clip_pane_apply_transform(BYTE *widget, BYTE *ctx)
{
    ((ApplyTransform_t)FN_WIDGET_APPLY_TRANSFORM)(widget, ctx);
    float *state = draw_state(ctx), *b = (float *)(widget + CLIPPANE_BOUNDS);
    if (!state) return;
    // state: x, y offset and x, y scale (DrawContext state, as ApplyTransform 0x443c20 builds it)
    int rect[4] = {
        (int)floorf(state[0] + b[0] * state[2] + 0.5f), (int)floorf(state[1] + b[1] * state[3] + 0.5f),
        (int)floorf(state[0] + b[2] * state[2] + 0.5f), (int)floorf(state[1] + b[3] * state[3] + 0.5f),
    };
    ((PushClip_t)FN_DRAWCTX_PUSH_CLIP)(ctx, rect);
}

#define STAGE_DARKNESS       0x2ec   // MaskContainer*, see stage_parallax_hook
#define VTBL_MASK_CONTAINER  0x557e64
#define MASK_BOUNDS          0xa8    // float x0, y0, x1, y1
#define MASK_VTBL_SET_BOUNDS 0x100   // thiscall(x0, y0, x1, y1), as Stage setup calls it (0x41479b)

// The HUD bar plus the stage name below it reach y = 95; during play the arena starts
// there (the game itself used 75, partly under the bar).
#define HUD_BOTTOM 95

void __stdcall stage_parallax_hook(BYTE *layer_set, float margin_x, float margin_y,
                                   float x0, float y0, float x1, float y1)
{
    float game_view_w = x1 - x0, game_view_h = y1 - y0; // the view the level was made for
    if (y0 > 0) y0 = HUD_BOTTOM;
    g_clip_floor = (int)y0;
    float view_w = (float)g_width, view_h = y1 - y0, above = y0;
    x0 = (float)-g_shift; // the screen's left edge in scene coordinates
    x1 = x0 + view_w;

    int base = *(int *)(layer_set + LAYERSET_BASE_INDEX);
    BYTE **infos = *(BYTE ***)(layer_set + LAYERSET_INFOS_BEGIN);
    int count = *(BYTE ***)(layer_set + LAYERSET_INFOS_END) - infos;
    BYTE **layouts = *(BYTE ***)(layer_set + LAYERSET_PANS_BEGIN);
    int layout_count = *(BYTE ***)(layer_set + LAYERSET_PANS_END) - layouts;
    float range_x = 0, range_y = 0, play_w = 0, play_h = 0;
    if (infos && base >= 0 && base < count) {
        float *play = (float *)(infos[base] + LAYERINFO_PAN_SIZE);
        play_w = play[0];
        play_h = play[1];
        log_printf("stage: play layer %d is %.0fx%.0f, view %.0fx%.0f below %.0f", base, play[0], play[1], view_w, view_h, above);
        if (play[0] >= UI_WIDTH && play[0] < view_w) play[0] = view_w;
        if (play[1] >= GAME_HEIGHT * 0.5f && play[1] < view_h) play[1] = view_h;
        range_x = play[0] > view_w ? play[0] - view_w : 0;
        range_y = play[1] > view_h ? play[1] - view_h : 0;
    }

    float *rect = (float *)(g_stage + STAGE_VIEW_RECT);
    rect[0] = x0, rect[1] = y0, rect[2] = x1, rect[3] = y1;
    // Night levels' darkness (a MaskContainer) took its bounds from that rect before it was
    // widened, and with full light its hole grows to exactly them: only the 4:3 middle.
    BYTE *mask = *(BYTE **)(g_stage + STAGE_DARKNESS);
    if (mask && *(DWORD *)mask == VTBL_MASK_CONTAINER) {
        typedef void (__thiscall *SetBounds_t)(BYTE *, float, float, float, float);
        float *b = (float *)(mask + MASK_BOUNDS);
        ((SetBounds_t)(*(void ***)mask)[MASK_VTBL_SET_BOUNDS / 4])(mask, x0, b[1], x1, b[3]);
    }
    *(float *)(layer_set + WIDGET_SCALE_X) = 1;
    *(float *)(layer_set + WIDGET_SCALE_Y) = 1;
    if (camera_override && g_layer_set == layer_set)
        memcpy(layer_set + WIDGET_POS, layer_set_origin, sizeof layer_set_origin);
    g_zoom = 1;
    camera_override = 0;
    static BYTE *clams_layer_set;  // a new level's clams are new (it may be set up again)
    if (layer_set != clams_layer_set) clams_moved_count = 0, clams_layer_set = layer_set;
    ((LayerSetParallax_t)FN_LAYERSET_PARALLAX)(layer_set, margin_x, margin_y, x0, y0, x1, y1);

    // A layer set at the address of an old one (freed with its level): forget the old one's
    // layers, or they'd pass for this one's.
    for (int i = 0; i < MAX_PANS; i++)
        if (pans[i].layer_set == layer_set) pans[i].pan = NULL, pans[i].layer_set = NULL;
    for (int i = 0; i < count && i < layout_count; i++) {
        float w = ((float *)(infos[i] + LAYERINFO_PAN_SIZE))[0];
        float h = ((float *)(infos[i] + LAYERINFO_PAN_SIZE))[1];
        if (w <= 0 || h <= 0) continue; // sets sized by width= instead of panSize
        PanLayout *p = find_pan(layouts[i]);
        if (!p) {
            p = &pans[pan_next];
            pan_next = (pan_next + 1) % MAX_PANS;
        }
        p->pan = layouts[i];
        p->layer_set = layer_set;
        p->is_play = i == base;
        BYTE *widget = *(BYTE **)(layouts[i] + LAYOUTPAN_WIDGET);
        if (i == base && widget) clip_play_group(widget);
        if (i == base) g_play_widget = widget, g_play_pan = layouts[i], g_layer_set_now = layer_set;
        // A layer as big as the play layer (as the level made it) moved exactly with it in
        // the original, and the game treats its contents as being in the play area: clams
        // are placed on layer="-1" next to it, and checked against the fish there. It's
        // scaled like any scenery (its art has to cover the screen), but what the game adds
        // to it during play is drawn at play-layer size and position (mixed_layers_before).
        // Only in levels, and never the backmost layer.
        p->mixed = above > 0 && i > 0 && i != base && w == play_w && h == play_h;
        p->art_count = 0;
        if (p->mixed && widget) {
            BYTE **child = *(BYTE ***)(widget + CONTAINER_CHILDREN_BEGIN);
            BYTE **child_end = *(BYTE ***)(widget + CONTAINER_CHILDREN_END);
            for (; child && child < child_end && p->art_count < MAX_ART; child++)
                p->art[p->art_count++] = *child;
        }
        p->w = w;
        p->h = h;
        p->view_w = view_w;
        p->view_h = view_h;
        p->above = above;
        p->range_x = range_x;
        p->range_y = range_y;
        // The original slid each layer by its share of the camera's travel: as far as it's
        // wider than the view, against how far the play layer is.
        p->rate_x = play_w > game_view_w && w > game_view_w ? (w - game_view_w) / (play_w - game_view_w) : 0;
        p->water = above > 0 && h == play_h ? (float)*(int *)(layer_set + LAYERSET_WATER_LEVEL) : 0;
        p->rate_y = play_h > game_view_h && h > game_view_h ? (h - game_view_h) / (play_h - game_view_h) : 0;
        log_printf("  layer %d: %.0fx%.0f%s", i, w, h, p->is_play ? " (play)" : p->mixed ? " (holds play-area things)" : "");
        // Lay it out now: until play starts the camera doesn't move, so ApplyCamera
        // isn't called and the layer would keep its unscaled position.
        layout_pan_apply_hook(layouts[i], (float *)(layer_set + LAYERSET_CAMERA));
    }
}

// Clams on a mixed layer. The layer is scaled like scenery (its art covers the screen), and
// the clams, in the stage's slots on it (ContainerSlot children), sat on its rocks with it.
// But the game checks them (their bite, their pearls) as if on the play layer, where they'd
// be elsewhere. So each clam's position is moved once to where it's drawn: a point c of the
// layer is drawn at L + s c (layer at L, scaled s), which the play layer (at P = -camera)
// draws at P + c' for c' = L - P + s c. L - P stays the same as the camera moves (the layer
// is the play layer's size, so it travels with it). And the slots are kept where the play
// layer is, at its size ((P - L) / s, scaled 1 / s), so what's in them is drawn at P + c':
// the clam stays on its rock, at its usual size, and its pearl comes out where it is.
#define VTBL_CONTAINER_SLOT 0x5508d4
#define VTBL_OYSTER         0x5523bc
#define VTBL_OYSTER_VISUAL  0x55227c
#define STAGE_CLAMS         0x354        // begin, end at +0x358: what Oyster::EnterStage (0x53eb71)
#define CLAM_ENTRY          0x11c        //   adds, each clam's part at +0x11c
#define ACTOR_LAYER         0x88
#define ACTOR_POS           0x98
#define VTBL_ACTOR_SET_POS  0x28         // thiscall(actor, float x, float y)
#define STAGE_VTBL_LAYER    0x24         // thiscall(stage, int layer) -> its widget

typedef void (__thiscall *SetPos_t)(BYTE *, float, float);

// The clam behind a clam list entry, if it's on `layer` (or in one of its slots).
static BYTE *clam_on(BYTE *entry, BYTE *layer)
{
    typedef BYTE *(__thiscall *GetLayer_t)(BYTE *, int);
    BYTE *a = entry ? entry - CLAM_ENTRY : NULL;
    if (!a || *(DWORD *)a != VTBL_OYSTER) return NULL;
    BYTE *home = ((GetLayer_t)(*(void ***)g_stage)[STAGE_VTBL_LAYER / 4])(g_stage, *(int *)(a + ACTOR_LAYER));
    if (home == layer) return a;
    BYTE **child = *(BYTE ***)(layer + CONTAINER_CHILDREN_BEGIN), **end = *(BYTE ***)(layer + CONTAINER_CHILDREN_END);
    for (; child && child < end; child++)
        if (*child == home) return a;
    return NULL;
}

static int clam_was_moved(BYTE *a)
{
    for (int i = 0; i < clams_moved_count; i++)
        if (clams_moved[i] == a) return 1;
    return 0;
}

static void clam_moved(BYTE *a, float x, float y)
{
    float *c = (float *)(a + ACTOR_POS);
    log_printf("clam %p moved from %.0f,%.0f to %.0f,%.0f, where it's drawn", a, c[0], c[1], x, y);
    ((SetPos_t)(*(void ***)a)[VTBL_ACTOR_SET_POS / 4])(a, x, y);
    if (clams_moved_count < MAX_CLAMS) clams_moved[clams_moved_count++] = a;
}

// Clams on other scenery layers (the caves'), which slide at their own pace: the game checks
// them at their position plus where their layer is (each frame, and through the real position
// here), so at L + c, but they're drawn with the layer's scale, at L + s c. So each clam's
// position becomes s c, and the layer's slots are drawn unscaled (1 / s), putting the clam
// at L + s c both ways: on its rock wherever the camera goes, at its usual size. Only layers
// with clams on them.
static void scale_clams(BYTE *layer, float s)
{
    if (!g_stage || s <= 0) return;
    int any = 0;
    BYTE **entry = *(BYTE ***)(g_stage + STAGE_CLAMS), **entries_end = *(BYTE ***)(g_stage + STAGE_CLAMS + 4);
    for (BYTE **e = entry; e && e < entries_end; e++) {
        BYTE *a = clam_on(*e, layer);
        if (!a) continue;
        any = 1;
        float *c = (float *)(a + ACTOR_POS);
        if (!clam_was_moved(a)) clam_moved(a, s * c[0], s * c[1]);
    }
    if (!any) return;
    BYTE **child = *(BYTE ***)(layer + CONTAINER_CHILDREN_BEGIN), **end = *(BYTE ***)(layer + CONTAINER_CHILDREN_END);
    for (; child && child < end; child++) {
        BYTE *c = *child;
        if (!c || *(DWORD *)c != VTBL_CONTAINER_SLOT) continue;
        float *pos = (float *)(c + WIDGET_POS), *scale = (float *)(c + WIDGET_SCALE_X);
        pos[0] = pos[1] = 0;
        scale[0] = scale[1] = 1 / s;
    }
}

// s: the layer's scale across and down (they differ for a mixed layer with a surface).
static void place_slots(BYTE *layer, const float *at, const float *s, const float *camera)
{
    if (s[0] <= 0 || s[1] <= 0) return;
    BYTE **child = *(BYTE ***)(layer + CONTAINER_CHILDREN_BEGIN), **end = *(BYTE ***)(layer + CONTAINER_CHILDREN_END);
    for (; child && child < end; child++) {
        BYTE *c = *child;
        if (!c || *(DWORD *)c != VTBL_CONTAINER_SLOT) continue;
        float *pos = (float *)(c + WIDGET_POS), *scale = (float *)(c + WIDGET_SCALE_X);
        pos[0] = (-camera[0] - at[0]) / s[0];
        pos[1] = (-camera[1] - at[1]) / s[1];
        scale[0] = 1 / s[0];
        scale[1] = 1 / s[1];
    }
    if (!g_stage) return;
    BYTE **entry = *(BYTE ***)(g_stage + STAGE_CLAMS), **entries_end = *(BYTE ***)(g_stage + STAGE_CLAMS + 4);
    for (; entry && entry < entries_end; entry++) {
        BYTE *a = clam_on(*entry, layer);
        if (!a || clam_was_moved(a)) continue;
        float *c = (float *)(a + ACTOR_POS);
        clam_moved(a, at[0] + camera[0] + s[0] * c[0], at[1] + camera[1] + s[1] * c[1]);
    }
}

// A thing on a layer other than the play layer (clams, on layer -1) is checked against the
// fish at its position plus how far its layer is from the play layer (Actor 0x4690f0, through
// 0x50a65d, which compares the layers' LayoutPan::GetPosition). A mixed layer moved exactly
// with the play layer in the original, so that was nothing; scaled, it's off by a lot (the
// clams' bite and pearls were 108 too high). Its contents are kept where the play layer would
// draw them (place_slots), so it reports the play layer's position.
typedef float *(__thiscall *LayoutPanPos_t)(BYTE *, float *);

float *__thiscall layout_pan_position_hook(BYTE *pan, float *out)
{
    PanLayout *p = g_play_pan ? find_pan(pan) : NULL;
    if (p && p->mixed && p->layer_set == g_layer_set_now) pan = g_play_pan;
    return ((LayoutPanPos_t)FN_LAYOUTPAN_POS)(pan, out);
}

// Particles of effects that emit into a named group (parent="PlayGroup" offsetType=
// "independent": ink clouds, star trails, splashes) stay where they were emitted as the
// emitter moves: Particle::ApplyTransform (0x444b00) ends by putting the draw position at
// the group's absolute position plus the particle's (0x444e1a), dropping what the draw
// had added above that: the scene shift (g_shift), so they sat 317 to the left, and the
// co-op zoom. Both are put back: a point at absolute a in the layer set (at L, scaled z)
// is drawn at g_shift + L + z (a - L).
#define ADDR_PARTICLE_APPLY_SLOT 0x55f7f0   // Particle vtable 0x55f784 + 0x6c
#define FN_PARTICLE_APPLY        0x444b00
#define PARTICLE_GROUP           0xec       // the named group it's placed in, or none
#define PARTICLE_HIDDEN          0xf0       // set: returns before placing it

void __thiscall particle_apply_hook(BYTE *particle, BYTE *ctx)
{
    ((ApplyTransform_t)FN_PARTICLE_APPLY)(particle, ctx);
    if (!*(BYTE **)(particle + PARTICLE_GROUP) || *(int *)(particle + PARTICLE_HIDDEN)) return;
    float *state = draw_state(ctx);
    if (!state) return;
    float lx = 0, ly = 0, z = 1;
    if (*(BYTE **)(particle + PARTICLE_GROUP) == g_play_widget && g_layer_set_now) {
        lx = ((float *)(g_layer_set_now + WIDGET_POS))[0];
        ly = ((float *)(g_layer_set_now + WIDGET_POS))[1];
        z = *(float *)(g_layer_set_now + WIDGET_SCALE_X);
    }
    state[0] = g_shift + lx + z * (state[0] - lx);
    state[1] = ly + z * (state[1] - ly);
}

// Replaces LayoutPan::ApplyCamera for the layers set up above.
typedef void (__thiscall *LayoutPanApply_t)(BYTE *, float *);

void __thiscall layout_pan_apply_hook(BYTE *pan, float *camera)
{
    PanLayout *p = find_pan(pan);
    BYTE *widget = *(BYTE **)(pan + LAYOUTPAN_WIDGET);
    if (!p || !widget) {
        ((LayoutPanApply_t)FN_LAYOUTPAN_APPLY)(pan, camera);
        return;
    }
    float pos[2], s = 1, s_down = 0; // s_down: a different scale down than across, if not 0
    // A mixed layer as wide as the view (the shipwreck's) needs no scaling to cover it, only
    // for the strip behind the HUD, and scaled it pushed its clams outward, one out of the
    // level. So it moves exactly with the play layer, as in the original; the strip shows the
    // layers behind it.
    if (p->is_play || (p->mixed && p->w >= p->view_w)) {
        pos[0] = -camera[0];
        pos[1] = -camera[1];
    } else {
        // Slide as far as the original size allows relative to the view, and cover the
        // drawn area: the view plus the strip above it (behind the HUD) plus that slide.
        // No faster than the original did, though: in a level barely wider than the view
        // (the caves: 16 pixels of camera travel) a layer swung all the way across that.
        float travel_x = p->w > p->view_w ? p->w - p->view_w : 0;
        float travel_y = p->h > p->view_h ? p->h - p->view_h : 0;
        travel_x = fminf(travel_x, p->rate_x * p->range_x);
        travel_y = fminf(travel_y, p->rate_y * p->range_y);
        float need_w = p->view_w + travel_x, need_h = p->view_h + p->above + travel_y;
        s = need_w / p->w > need_h / p->h ? need_w / p->w : need_h / p->h;
        if (s < 1) s = 1;
        float tx = p->range_x > 0 ? camera[0] / p->range_x : 0.5f;
        float ty = p->range_y > 0 ? camera[1] / p->range_y : 0.5f;
        tx = tx < 0 ? 0 : tx > 1 ? 1 : tx;
        ty = ty < 0 ? 0 : ty > 1 ? 1 : ty;
        pos[0] = -(p->w * s - need_w) / 2 - tx * travel_x;
        pos[1] = -p->above - (p->h * s - need_h) / 2 - ty * travel_y;
        if (p->mixed && p->water > 0) {
            // A mixed layer with a surface holds the clams on its floor too: lined up at the
            // surface and scaled, its floor went below the level's (Wrasse Reef's clams sat on
            // the bottom edge, cut off). It needs both lines where they were, so it's only
            // stretched across to cover the width, and up and down it moves exactly with the
            // play layer, as in the original; the strip behind the HUD shows the layers behind.
            s = need_w / p->w > 1 ? need_w / p->w : 1;
            s_down = 1;
            pos[0] = -(p->w * s - need_w) / 2 - tx * travel_x;
            pos[1] = -camera[1];
        } else if (p->water > 0) {
            // A layer as tall as the level moved up and down exactly with the play layer in
            // the original, so its water surface (pier, reef) sat on the level's water line,
            // which fish leap out of and butterflies stay above. Scaled, it's lined up there
            // instead (the line w is drawn at P + w for the play layer at P = -camera): it
            // then reaches w (s - 1) less far up, a strip the HUD covers.
            pos[1] = -camera[1] + p->water * (1 - s);
        } else if (p->mixed) {
            // A mixed layer without a surface (the reef's rocks) sits on the level's floor
            // instead: scaled up to cover the width, its art grows taller than the level,
            // and centered, its bottom (with the clams on its rocks) went below the level's
            // bottom edge, off screen. Its bottom is lined up with the level's (the line h,
            // drawn at P + h for the play layer at P); the extra height goes up, behind the HUD.
            pos[1] = -camera[1] + p->h * (1 - s);
        }
    }
    float scale[2] = { s, s_down ? s_down : s };
    *(float *)(widget + WIDGET_SCALE_X) = scale[0];
    *(float *)(widget + WIDGET_SCALE_Y) = scale[1];
    typedef void (__thiscall *SetPosition_t)(BYTE *, float *);
    ((SetPosition_t)(*(void ***)widget)[WIDGET_VTBL_SET_POSITION / 4])(widget, pos);
    if (p->mixed) place_slots(widget, pos, scale, camera);
    else if (!p->is_play && s != 1) scale_clams(widget, s);
}

// Things the game added to a mixed layer (clams, pearls, their effects: children that
// weren't there at setup) are drawn where the play layer would draw them: a child at c
// lands at L + s * c in a layer at L scaled by s, and at P + c in the play layer at P. So
// for the draw it moves to (P + c - L) / s and shrinks by 1 / s, and is put back after.
typedef struct {
    BYTE *widget;
    float pos[2], scale[2];
} MovedChild;

#define MAX_MOVED 256
static MovedChild moved[MAX_MOVED];
static int moved_count;

static void mixed_layers_before(void)
{
    moved_count = 0;
    if (!g_play_widget || !scene_stage_shown()) return;
    float *play = (float *)(g_play_widget + WIDGET_POS);
    for (int i = 0; i < MAX_PANS; i++) {
        PanLayout *p = &pans[i];
        if (!p->pan || p->is_play || p->layer_set != g_layer_set_now) continue;
        BYTE *layer = *(BYTE **)(p->pan + LAYOUTPAN_WIDGET);
        if (!layer) continue;
        if (!p->mixed) {
            float s = *(float *)(layer + WIDGET_SCALE_X);
            if (s != 1) scale_clams(layer, s); // as at the camera's move, in case that's missed
            continue;
        }
        float *at = (float *)(layer + WIDGET_POS), s[2] = { *(float *)(layer + WIDGET_SCALE_X), *(float *)(layer + WIDGET_SCALE_Y) };
        if (s[0] <= 0 || s[1] <= 0) continue;
        float camera[2] = { -play[0], -play[1] };
        place_slots(layer, at, s, camera); // as at the camera's move, in case that's missed
        BYTE **child = *(BYTE ***)(layer + CONTAINER_CHILDREN_BEGIN);
        BYTE **end = *(BYTE ***)(layer + CONTAINER_CHILDREN_END);
        for (; child && child < end && moved_count < MAX_MOVED; child++) {
            BYTE *c = *child;
            int art = 0;
            if (!c || *(DWORD *)c == VTBL_CONTAINER_SLOT) continue; // placed for good: place_slots
            for (int k = 0; k < p->art_count && !art; k++) art = p->art[k] == c;
            if (art && *(DWORD *)c != VTBL_OYSTER_VISUAL) continue;
            MovedChild *m = &moved[moved_count++];
            m->widget = c;
            memcpy(m->pos, c + WIDGET_POS, sizeof m->pos);
            memcpy(m->scale, c + WIDGET_SCALE_X, sizeof m->scale);
            float *pos = (float *)(c + WIDGET_POS), *scale = (float *)(c + WIDGET_SCALE_X);
            pos[0] = (play[0] + m->pos[0] - at[0]) / s[0];
            pos[1] = (play[1] + m->pos[1] - at[1]) / s[1];
            scale[0] /= s[0];
            scale[1] /= s[1];
        }
    }
}

static void mixed_layers_after(void)
{
    while (moved_count > 0) {
        MovedChild *m = &moved[--moved_count];
        memcpy(m->widget + WIDGET_POS, m->pos, sizeof m->pos);
        memcpy(m->widget + WIDGET_SCALE_X, m->scale, sizeof m->scale);
    }
}

// --- Co-op camera -----------------------------------------------------------------
// The game's camera follows player 1 (Stage::UpdateCamera, 0x43a950, vtable slot 0x558624:
// LayerSet::UpdateCamera 0x43b960 keeps the fish inside a margin of the view, clamps the
// camera to the level and moves every layer). In co-op, while both fish are in play:
//  - if they're too far apart to both fit, the whole layer set (scenery and fish alike) is
//    drawn shrunk, as far as needed (down to showing the whole level, or COOP_ZOOM_MIN);
//  - the level window shown (view size / zoom) is centered between them, kept inside the
//    level where it fits, and always drawn into the view below the HUD;
//  - the scene is laid out as usual for a normal-size camera centered on that window, and
//    the layer set is shrunk and moved so the window fills the view. The level art barely
//    reaches past the normal view, so the area around it is filled with a dark, blurred
//    extension of its edges (ambient_zoom_fill).
// With one fish in play the zoom and camera ease back to the game's own.
#define ADDR_STAGE_CAMERA_SLOT  0x558624
#define FN_STAGE_CAMERA         0x43a950
#define STAGE_LAYER_SET         0x2d8
#define STAGE_CAMERA_ON         0x260   // byte: the camera follows the player (during play)
#define LAYERSET_RANGE          0x10c   // float x, y: camera range (level size minus view)
#define LAYERSET_PANS_VTBL_APPLY 0x1c   // LayoutPan::ApplyCamera(const float *camera)
#define COOP_MARGIN_X           180     // room kept beside each fish, level pixels
#define COOP_MARGIN_Y           120
#define COOP_ZOOM_MIN           0.5f
#define ZOOM_TIME               0.45f   // seconds to get ~2/3 of the way to the new zoom
#define CAMERA_TIME             0.15f
typedef void (__thiscall *StageCamera_t)(BYTE *stage, BYTE *update);
static float window_x, window_y;        // top-left of the level window shown, level pixels
static RECT g_content;                  // back-buffer pixels the scenery covers while zoomed

static float ease(float t)
{
    return t > 0 ? 1 - expf(-t) : 0;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// The back-buffer rect the scenery layers cover (their union's bounding box).
static void update_content_rect(BYTE *layer_set)
{
    float *origin = (float *)(layer_set + WIDGET_POS);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    BYTE **pans = *(BYTE ***)(layer_set + LAYERSET_PANS_BEGIN);
    BYTE **end = *(BYTE ***)(layer_set + LAYERSET_PANS_END);
    for (BYTE **pp = pans; pans && pp < end; pp++) {
        PanLayout *p = find_pan(*pp);
        BYTE *widget = *(BYTE **)(*pp + LAYOUTPAN_WIDGET);
        if (!p || p->is_play || !widget) continue;
        if (pp != pans) break; // only the backmost layer: opaque, and scaled to cover
        float *pos = (float *)(widget + WIDGET_POS), s = *(float *)(widget + WIDGET_SCALE_X);
        float l = origin[0] + g_zoom * pos[0], t = origin[1] + g_zoom * pos[1];
        float r = l + g_zoom * p->w * s, b = t + g_zoom * p->h * s;
        if (l < x0) x0 = l;
        if (t < y0) y0 = t;
        if (r > x1) x1 = r;
        if (b > y1) y1 = b;
    }
    if (x1 <= x0) x0 = -1e9f, y0 = -1e9f, x1 = 1e9f, y1 = 1e9f; // unknown: assume covered
    // Scene coordinates to back-buffer pixels (the scene is drawn shifted by g_shift).
    g_content.left = (LONG)clampf(x0 + g_shift + 0.5f, -1, 100000);
    g_content.top = (LONG)clampf(y0 + 0.5f, -1, 100000);
    g_content.right = (LONG)clampf(x1 + g_shift + 0.5f, -1, 100000);
    g_content.bottom = (LONG)clampf(y1 + 0.5f, -1, 100000);
}

void __thiscall stage_camera_hook(BYTE *stage, BYTE *update)
{
    ((StageCamera_t)FN_STAGE_CAMERA)(stage, update);
    BYTE *layer_set = *(BYTE **)(stage + STAGE_LAYER_SET);
    // (the camera doesn't follow anyone while player 1's death plays; see coop.c)
    if (!layer_set || (!stage[STAGE_CAMERA_ON] && !coop_respawn_framing())) return;
    float fish[MAX_PLAYERS + 1][2];
    int count = coop_player_positions(fish, MAX_PLAYERS + 1), both = count >= 2;
    if (!camera_override && !both) return;
    // the box around everyone in play
    float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f };
    for (int i = 0; i < count; i++)
        for (int k = 0; k < 2; k++) {
            if (fish[i][k] < lo[k]) lo[k] = fish[i][k];
            if (fish[i][k] > hi[k]) hi[k] = fish[i][k];
        }

    float dt = *(float *)(update + 8);
    float *rect = (float *)(stage + STAGE_VIEW_RECT), *range = (float *)(layer_set + LAYERSET_RANGE);
    float *camera = (float *)(layer_set + LAYERSET_CAMERA); // the game's, just updated
    float view_w = rect[2] - rect[0], view_h = rect[3] - rect[1];
    float level_w = range[0] + view_w, level_h = range[1] + view_h;
    if (!camera_override || g_layer_set != layer_set) {
        camera_override = 1;
        g_layer_set = layer_set;
        memcpy(layer_set_origin, layer_set + WIDGET_POS, sizeof layer_set_origin);
        window_x = camera[0];
        window_y = camera[1];
        log_printf("co-op camera on: view %.0f,%.0f %.0fx%.0f, level %.0fx%.0f, layer set at %.0f,%.0f",
                   rect[0], rect[1], view_w, view_h, level_w, level_h, layer_set_origin[0], layer_set_origin[1]);
    }

    // How far to shrink so all the fish fit with some room around them, and where the window
    // goes: centered on them, inside the level where it fits.
    float zoom = 1, wx = camera[0], wy = camera[1];
    if (both) {
        // no further out than shows the whole level that way: beyond it is only the dark
        // surround (a level as wide as the view never needs to zoom for width)
        float need_w = fminf(hi[0] - lo[0] + 2 * COOP_MARGIN_X, level_w);
        float need_h = fminf(hi[1] - lo[1] + 2 * COOP_MARGIN_Y, level_h);
        if (view_w / need_w < zoom) zoom = view_w / need_w;
        if (view_h / need_h < zoom) zoom = view_h / need_h;
        float whole = fminf(view_w / level_w, view_h / level_h);
        if (zoom < whole) zoom = whole; // the whole level is already in view
        if (zoom < COOP_ZOOM_MIN) zoom = COOP_ZOOM_MIN;
    }
    g_zoom += (zoom - g_zoom) * ease(dt / ZOOM_TIME);
    if (fabsf(g_zoom - zoom) < 0.0005f) g_zoom = zoom;
    float size_w = view_w / g_zoom, size_h = view_h / g_zoom;
    if (both) {
        wx = (lo[0] + hi[0]) / 2 - size_w / 2;
        wy = (lo[1] + hi[1]) / 2 - size_h / 2;
    }
    wx = size_w <= level_w ? clampf(wx, 0, level_w - size_w) : (level_w - size_w) / 2;
    wy = size_h <= level_h ? clampf(wy, 0, level_h - size_h) : (level_h - size_h) / 2;
    window_x += (wx - window_x) * ease(dt / CAMERA_TIME);
    window_y += (wy - window_y) * ease(dt / CAMERA_TIME);
    if (!both && g_zoom == 1 && fabsf(window_x - camera[0]) < 0.5f && fabsf(window_y - camera[1]) < 0.5f) {
        camera_override = 0; // back to the game's camera
        window_x = camera[0];
        window_y = camera[1];
    }

    // The normal-size camera centered on the window lays out the scene as usual; the game's
    // camera continues from there next frame.
    camera[0] = clampf(window_x + (size_w - view_w) / 2, 0, range[0]);
    camera[1] = clampf(window_y + (size_h - view_h) / 2, 0, range[1]);
    BYTE **pans = *(BYTE ***)(layer_set + LAYERSET_PANS_BEGIN);
    BYTE **end = *(BYTE ***)(layer_set + LAYERSET_PANS_END);
    typedef void (__thiscall *PanApply_t)(BYTE *, float *);
    for (BYTE **p = pans; pans && p < end; p++)
        ((PanApply_t)(*(void ***)*p)[LAYERSET_PANS_VTBL_APPLY / 4])(*p, camera);
    // Shrink the layer set and move it so the window's top-left lands on the view's: a level
    // point w is drawn at origin + zoom * (w - camera).
    float *origin = (float *)(layer_set + WIDGET_POS);
    origin[0] = layer_set_origin[0] - g_zoom * (window_x - camera[0]);
    origin[1] = layer_set_origin[1] - g_zoom * (window_y - camera[1]);
    *(float *)(layer_set + WIDGET_SCALE_X) = g_zoom;
    *(float *)(layer_set + WIDGET_SCALE_Y) = g_zoom;
    update_content_rect(layer_set);
}

// Stage::LayerToOverlay (0x4678b0, thiscall(stage, int layer, float x, float y), result in
// [esi], ret 12): a point in a layer as a screen position for the overlay widgets beside the
// layer set (thought bubbles, score splats, ...): the layer's absolute position (Widget
// vtable +0x10 adds up the positions of it and its parents, ignoring scale) plus the point.
// While zoomed, the part inside the layer set is shrunk: measured from the layer set's
// absolute position, it's scaled by the zoom.
#define FN_STAGE_LAYER_TO_OVERLAY 0x4678b0
#define WIDGET_VTBL_ABSOLUTE_POS  0x10
void __cdecl layer_to_overlay(BYTE *stage, int layer, float x, float y, float *out)
{
    typedef BYTE *(__thiscall *GetLayer_t)(BYTE *, int);
    typedef float *(__thiscall *GetPos_t)(BYTE *, float *);
    BYTE *widget = ((GetLayer_t)(*(void ***)stage)[0x24 / 4])(stage, layer);
    // Things on a mixed layer are drawn as if on the play layer (mixed_layers_before).
    for (int i = 0; i < MAX_PANS && g_play_widget; i++)
        if (pans[i].pan && pans[i].mixed && *(BYTE **)(pans[i].pan + LAYOUTPAN_WIDGET) == widget) widget = g_play_widget;
    float buf[3];
    float *pos = ((GetPos_t)(*(void ***)widget)[WIDGET_VTBL_ABSOLUTE_POS / 4])(widget, buf);
    out[0] = pos[0] + x;
    out[1] = pos[1] + y;
    BYTE *layer_set = *(BYTE **)(stage + STAGE_LAYER_SET);
    if (camera_override && layer_set == g_layer_set && g_zoom != 1) {
        float base_buf[3];
        float *base = ((GetPos_t)(*(void ***)layer_set)[WIDGET_VTBL_ABSOLUTE_POS / 4])(layer_set, base_buf);
        float bx = base[0], by = base[1];
        out[0] = bx + (out[0] - bx) * g_zoom;
        out[1] = by + (out[1] - by) * g_zoom;
    }
}

static int play_to_screen(const float *p, int *out)
{
    typedef float *(__thiscall *GetPos_t)(BYTE *, float *);
    if (!g_play_widget || !scene_stage_shown()) return 0;
    float buf[3];
    float *pos = ((GetPos_t)(*(void ***)g_play_widget)[WIDGET_VTBL_ABSOLUTE_POS / 4])(g_play_widget, buf);
    float x = pos[0] + p[0], y = pos[1] + p[1];
    if (camera_override && g_layer_set && g_zoom != 1) {
        float base_buf[3];
        float *base = ((GetPos_t)(*(void ***)g_layer_set)[WIDGET_VTBL_ABSOLUTE_POS / 4])(g_layer_set, base_buf);
        x = base[0] + (x - base[0]) * g_zoom;
        y = base[1] + (y - base[1]) * g_zoom;
    }
    out[0] = (int)(x + g_shift + 0.5f);
    out[1] = (int)(y + 0.5f);
    return 1;
}

void layer_to_overlay_thunk(void);
__asm__(
    ".globl _layer_to_overlay_thunk\n"
    "_layer_to_overlay_thunk:\n"
    "    pushal\n"
    "    pushl %esi\n"
    "    pushl 48(%esp)\n"         // y
    "    pushl 48(%esp)\n"         // x
    "    pushl 48(%esp)\n"         // layer
    "    pushl %ecx\n"             // stage
    "    call _layer_to_overlay\n"
    "    addl $20, %esp\n"
    "    popal\n"
    "    movl %esi, %eax\n"
    "    ret $12\n");

// While zoomed out and the level is on screen, after the frame is drawn: the area around
// the scenery that nothing was drawn on gets the blurred extension of its edges.
static void zoom_surround(void **surface)
{
    if (!camera_override || g_zoom >= 0.999f || !scene_stage_shown()) return;
    ambient_zoom_capture(surface, &g_content);
}

// Before a zoomed frame is drawn: the surround, from the last frame's picture.
static void zoom_backdrop(BYTE *app)
{
    BYTE *display = *(BYTE **)(app + APP_DISPLAY);
    if (!display || !camera_override || g_zoom >= 0.999f || !scene_stage_shown()) return;
    ambient_zoom_fill(*(void ***)(display + DISPLAY_BACK_SURFACE), &g_content);
}

void stage_parallax_thunk(void);
__asm__(
    ".globl _stage_parallax_thunk\n"
    "_stage_parallax_thunk:\n"
    "    movl %ebx, _g_stage\n"
    "    jmp _stage_parallax_hook@28\n");

// --- Screen width as seen by game layout code ------------------------------------
// These functions position splats, popups and the stage rect from GetWidth(); in the
// shifted scene they must keep thinking the screen is 800 wide.
static const DWORD layout_callers[][2] = {
    { 0x0040d970, 0x0040da50 }, // frenzy splat, centered
    { 0x00416020, 0x00416330 }, // right-aligned popup
    { 0x00422ce0, 0x004230f0 }, // popup text kept on screen
    { 0x004230f0, 0x00423190 }, // popup text kept on screen
    { 0x004302f0, 0x004306a0 }, // right-aligned popup
    { 0x004f1660, 0x004f191e }, // popup text kept on screen (cold part of 0x422ce0)
    { 0x004146b0, 0x00414b30 }, // Stage::Setup viewport rect
};

int __thiscall engine_get_width_hook(BYTE *engine)
{
    DWORD caller = (DWORD)__builtin_return_address(0);
    for (unsigned i = 0; i < sizeof layout_callers / sizeof *layout_callers; i++)
        if (caller >= layout_callers[i][0] && caller < layout_callers[i][1]) return UI_WIDTH;
    return *(int *)(engine + ENGINE_WIDTH);
}

// --- Mouse -------------------------------------------------------------------------

void __cdecl clamp_mouse(POINT *pt, BYTE *engine)
{
    int w = *(int *)(engine + ENGINE_WIDTH), h = *(int *)(engine + ENGINE_HEIGHT);
    pt->x = pt->x < 0 ? 0 : pt->x > w - 1 ? w - 1 : pt->x;
    pt->y = pt->y < 0 ? 0 : pt->y > h - 1 ? h - 1 : pt->y;
    // Relative mode only uses distances from its center in window pixels; shifting
    // would read as a constant 317-pixel move to the left on every update.
    BYTE *relative = *(BYTE **)(engine + ENGINE_RELATIVE_MOUSE);
    if (!relative || !relative[RELATIVE_MOUSE_ACTIVE]) pt->x -= g_shift;
    *(int *)(engine + ENGINE_MOUSE_X) = pt->x;
    *(int *)(engine + ENGINE_MOUSE_Y) = pt->y;
}

void clamp_mouse_thunk(void);
__asm__(
    ".globl _clamp_mouse_thunk\n"
    "_clamp_mouse_thunk:\n"
    "    pushal\n"
    "    pushl %edi\n"
    "    pushl %esi\n"
    "    call _clamp_mouse\n"
    "    addl $8, %esp\n"
    "    popal\n"
    "    ret\n");

int patches_is_game(void)
{
    const BYTE *p = (const BYTE *)(ADDR_CONFIG_WIDTH - 3);
    static const BYTE sig[] = { 0xc7, 0x46, 0x24, 0x20, 0x03, 0x00, 0x00 }; // mov [esi+0x24], 800
    return !IsBadReadPtr(p, sizeof sig) && memcmp(p, sig, sizeof sig) == 0;
}

void patches_apply(void)
{
    g_width = wide_width();
    g_shift = (g_width - UI_WIDTH) / 2;
    scene_init(g_width, g_shift);
    ambient_init(g_width, g_shift);
    patch_u32(ADDR_CONFIG_WIDTH, 800, g_width);
    patch_u32(ADDR_CONFIG_HEIGHT, 600, GAME_HEIGHT);
    int fps = g_fps = frame_rate();
    patch_u32(ADDR_CONFIG_FPS_1, 60, fps);
    patch_u32(ADDR_CONFIG_FPS_2, 60, fps);
    patch_u32(ADDR_ENGINE_SET_FRAME_RATE_SLOT, FN_ENGINE_SET_FRAME_RATE, (DWORD)engine_set_frame_rate_hook);
    mouse_init(ini_float("mouse_sensitivity", 1.0f));
    mice_init();
    static const BYTE recenter_start[5] = { 0x55, 0x8b, 0xec, 0x83, 0xec }; // push ebp; mov ebp,esp; sub esp,..
    patch_jmp(FN_RELATIVE_MOUSE_RECENTER, recenter_start, mouse_recenter_thunk);
    patch_call(ADDR_PLAYER_DRAG_CALL, FN_PLAYER_DRAG, player_drag_thunk);
    char ini_path[MAX_PATH], *slash = ini_path, player_colors[MAX_PLAYERS][32];
    const char *colors[MAX_PLAYERS] = { NULL };
    GetModuleFileNameA(NULL, ini_path, MAX_PATH);
    for (char *p = ini_path; *p; p++)
        if (*p == '\\') slash = p + 1;
    lstrcpyA(slash, "ff2wide.ini");
    for (int i = 1; i < MAX_PLAYERS; i++) {
        char key[24];
        sprintf(key, "coop_p%d_color", i + 1);
        GetPrivateProfileStringA("ff2wide", key, "", player_colors[i], sizeof player_colors[i], ini_path);
        colors[i] = player_colors[i];
    }
    coop_init(ini_int("coop", 0), ini_float("coop_pad_speed", 700), colors, patch_u32);
    tweaks_init(ini_int("golden_minnows", 1), ini_int("frenzy_waits_for_mermaid", 1), patch_u32);
    online_init(ini_int("online", 0), ini_int("online_port", 47600), ini_int("online_kbps", 6000),
                ini_int("online_fps", 60), g_width, GAME_HEIGHT, ini_int("online_upnp", 1));
    static const BYTE push_bpp_field[6] = { 0xff, 0xb6, 0xf8, 0x00, 0x00, 0x00 }; // push [esi+0xf8]
    static const BYTE push_32[6] = { 0x6a, 0x20, 0x90, 0x90, 0x90, 0x90 };        // push 32; nop x4
    if (patch_bytes(ADDR_DISPLAY_MODE_BPP, push_bpp_field, push_32, 6))
        log_printf("display mode: 32 bits per pixel");
    patch_u32(ADDR_DISPLAY_PRESENT_SLOT, FN_DISPLAY_PRESENT, (DWORD)display_present_hook);
    if (g_width == UI_WIDTH) return;
    log_printf("UI shifted right by %d", g_shift);

    static const BYTE clamp_mouse_start[5] = { 0x8b, 0x07, 0x8b, 0xcf, 0xff }; // mov eax,[edi]; mov ecx,edi; call..
    patch_call(ADDR_FRAME_DRAW_CALL, FN_SIGNAL_DRAW, frame_draw_thunk);
    patch_call(ADDR_STAGE_PARALLAX_CALL, FN_LAYERSET_PARALLAX, stage_parallax_thunk);
    patch_u32(ADDR_ENGINE_GETWIDTH_SLOT, FN_ENGINE_GETWIDTH, (DWORD)engine_get_width_hook);
    patch_jmp(FN_ENGINE_CLAMP_MOUSE, clamp_mouse_start, clamp_mouse_thunk);
    patch_u32(ADDR_LAYOUTPAN_APPLY_SLOT, FN_LAYOUTPAN_APPLY, (DWORD)layout_pan_apply_hook);
    patch_u32(ADDR_LAYOUTPAN_POS_SLOT, FN_LAYOUTPAN_POS, (DWORD)layout_pan_position_hook);
    patch_u32(ADDR_PARTICLE_APPLY_SLOT, FN_PARTICLE_APPLY, (DWORD)particle_apply_hook);
    patch_u32(ADDR_CLIPPANE_APPLY_SLOT, FN_CLIPPANE_APPLY, (DWORD)clip_pane_apply_transform);
    { // co-op camera: does nothing unless co-op has two or more fish in play
        static const BYTE layer_to_overlay_start[5] = { 0x55, 0x8b, 0xec, 0x8b, 0x01 }; // push ebp; mov ebp,esp; mov eax,[ecx]
        patch_u32(ADDR_STAGE_CAMERA_SLOT, FN_STAGE_CAMERA, (DWORD)stage_camera_hook);
        patch_jmp(FN_STAGE_LAYER_TO_OVERLAY, layer_to_overlay_start, layer_to_overlay_thunk);
    }
    static const BYTE runtime_check_start[5] = { 0x33, 0xc0, 0x50, 0x50, 0x50 }; // xor eax,eax; push eax x3
    patch_jmp(FN_RUNTIME_CHECK_FAILED, runtime_check_start, crashlog_runtime_check_failed);
}
