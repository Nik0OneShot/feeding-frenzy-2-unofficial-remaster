// Per-frame tweaks to the widget tree, applied just before the scene is drawn and undone
// right after, so game logic (hit testing, animation) never sees them:
//  - full-screen 800x600 backgrounds that are the same painting as the main menu backdrop
//    are scaled to cover the whole width, matching the scaled menu backdrop behind them;
//  - the HUD is hidden while a stage start / stage complete panel covers it, because the
//    panel's faded edges would otherwise show the HUD's ends.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "scene.h"
#include "coop.h"
#include "online.h"

// Widgets (engine::Interface).
#define WIDGET_NAME       0x10   // std::string (MSVC 2005: buffer/pointer at +4, size +0x14, capacity +0x18)
#define WIDGET_X          0x2c   // float position
#define WIDGET_Y          0x30
#define WIDGET_SCALE_X    0x3c   // float scale applied when drawing (0x443c20)
#define WIDGET_SCALE_Y    0x40
#define WIDGET_OPACITY    0x48   // float, clamped to 0..1 and multiplied into the draw alpha
#define WIDGET_FLAGS      0x70   // bit 0: visible
#define WIDGET_HIDE_FLAGS 0x74   // bit 0: hidden; Container::Draw skips the child
#define CONTAINER_BEGIN   0x9c   // std::vector<Widget*> children
#define CONTAINER_END     0xa0
#define VTBL_DRAW         0xa0   // Widget::Draw(DrawContext*) vtable offset
#define FN_CONTAINER_DRAW 0x443850
#define FN_COMPOSITE_DRAW 0x4465d0

// The frame's draw signal (engine+0x14) holds a list of connections; the one of type
// sigslot::_connection1<engine::Scene, DrawContext*> targets the Scene, the root container
// whose Draw renders every screen.
#define SIGNAL_LIST           0x08
#define CONNECTION_TARGET     0x04
#define VTBL_SCENE_CONNECTION 0x560884

#define UI_WIDTH  800
#define UI_HEIGHT 600
#define MAX_TWEAKS 8

static int g_width, g_shift;

typedef struct {
    BYTE *widget;
    float x, y, sx, sy, opacity;
    DWORD hide;
} Saved;

static Saved saved[MAX_TWEAKS];
static int saved_count;

static const char *widget_name(BYTE *w)
{
    DWORD size = *(DWORD *)(w + WIDGET_NAME + 0x14), cap = *(DWORD *)(w + WIDGET_NAME + 0x18);
    if (size == 0 || size > 256) return "";
    return cap < 16 ? (const char *)(w + WIDGET_NAME + 4) : *(const char **)(w + WIDGET_NAME + 4);
}

static int is_container(BYTE *w)
{
    DWORD draw = (*(DWORD **)w)[VTBL_DRAW / 4];
    // Some classes (e.g. engine::Scene, 0x45dc90) draw through a `jmp Container::Draw` thunk.
    if (*(BYTE *)draw == 0xe9) draw = draw + 5 + *(DWORD *)(draw + 1);
    return draw == FN_CONTAINER_DRAW || draw == FN_COMPOSITE_DRAW;
}

static int is_shown(BYTE *w)
{
    return (*(DWORD *)(w + WIDGET_FLAGS) & 1) && !(*(DWORD *)(w + WIDGET_HIDE_FLAGS) & 1);
}

static BYTE **children(BYTE *w, BYTE ***end)
{
    *end = *(BYTE ***)(w + CONTAINER_END);
    return *(BYTE ***)(w + CONTAINER_BEGIN);
}

// Depth-first search for a widget with this name below `root` (shown widgets only).
static BYTE *find(BYTE *root, const char *name, int depth)
{
    if (!root || depth > 12 || !is_container(root)) return NULL;
    BYTE **end, **c = children(root, &end);
    if (!c || end < c || end - c > 4096) return NULL;
    for (; c < end; c++) {
        if (!*c || !is_shown(*c)) continue;
        if (strcmp(widget_name(*c), name) == 0) return *c;
        BYTE *hit = find(*c, name, depth + 1);
        if (hit) return hit;
    }
    return NULL;
}

static Saved *save(BYTE *w)
{
    if (saved_count == MAX_TWEAKS) return NULL;
    Saved *s = &saved[saved_count++];
    s->widget = w;
    s->x = *(float *)(w + WIDGET_X);
    s->y = *(float *)(w + WIDGET_Y);
    s->sx = *(float *)(w + WIDGET_SCALE_X);
    s->sy = *(float *)(w + WIDGET_SCALE_Y);
    s->opacity = *(float *)(w + WIDGET_OPACITY);
    s->hide = *(DWORD *)(w + WIDGET_HIDE_FLAGS);
    return s;
}

static float opacity(BYTE *w)
{
    float a = *(float *)(w + WIDGET_OPACITY);
    return a < 0 ? 0 : a > 1 ? 1 : a;
}

// How opaque the shown widget with this name is on screen: the product of the opacities
// from `root` down to it. 0 if there is none.
static float find_opacity(BYTE *root, const char *name, float above, int depth)
{
    if (!root || depth > 12 || !is_container(root)) return 0;
    BYTE **end, **c = children(root, &end);
    if (!c || end < c || end - c > 4096) return 0;
    for (; c < end; c++) {
        if (!*c || !is_shown(*c)) continue;
        float a = above * opacity(*c);
        if (strcmp(widget_name(*c), name) == 0) return a;
        float hit = find_opacity(*c, name, a, depth + 1);
        if (hit > 0) return hit;
    }
    return 0;
}

// Scale a top-left-anchored 800x600 image so it covers the whole screen, centered, the
// same way the main menu's backdrop layers are scaled.
static void cover_screen(BYTE *w)
{
    if (!w || !save(w)) return;
    float s = (float)g_width / UI_WIDTH;
    *(float *)(w + WIDGET_SCALE_X) = s;
    *(float *)(w + WIDGET_SCALE_Y) = s;
    *(float *)(w + WIDGET_X) = (float)-g_shift;
    *(float *)(w + WIDGET_Y) = (UI_HEIGHT - UI_HEIGHT * s) / 2;
}

static BYTE *scene_root(BYTE *engine)
{
    BYTE *signal = engine + 0x14;
    BYTE **head = *(BYTE ***)(signal + SIGNAL_LIST);
    if (!head) return NULL;
    // std::list nodes: next, prev, connection*
    for (BYTE **node = (BYTE **)*head; node && node != (BYTE **)head; node = (BYTE **)node[0]) {
        BYTE *connection = node[2];
        if (connection && *(DWORD *)connection == VTBL_SCENE_CONNECTION)
            return *(BYTE **)(connection + CONNECTION_TARGET);
    }
    return NULL;
}

// Writes the widget tree to ff2wide.log whenever the set of top-level screens changes,
// so screen and widget names can be checked against what is actually on screen.
static void dump(BYTE *w, int depth, int *lines)
{
    if (!w || depth > 5 || *lines > 400) return;
    char indent[16];
    memset(indent, ' ', sizeof indent);
    indent[depth * 2 < 15 ? depth * 2 : 15] = 0;
    log_printf("%s%s \"%s\" vtbl=%08lx shown=%d pos=(%.0f,%.0f) scale=(%.2f,%.2f)", indent,
               is_container(w) ? "+" : "-", widget_name(w), *(DWORD *)w, is_shown(w),
               *(float *)(w + WIDGET_X), *(float *)(w + WIDGET_Y),
               *(float *)(w + WIDGET_SCALE_X), *(float *)(w + WIDGET_SCALE_Y));
    (*lines)++;
    if (!is_container(w)) return;
    BYTE **end, **c = children(w, &end);
    if (!c || end < c || end - c > 4096) return;
    for (; c < end; c++) dump(*c, depth + 1, lines);
}

static void dump_on_change(BYTE *root)
{
    static DWORD last;
    DWORD sig = 0;
    BYTE **end, **c = children(root, &end);
    if (!c || end < c || end - c > 4096) return;
    for (; c < end; c++) {
        if (!*c || !is_shown(*c)) continue;
        for (const char *n = widget_name(*c); *n; n++) sig = sig * 31 + (BYTE)*n;
        sig = sig * 31 + (DWORD)*c;
    }
    if (sig == last) return;
    last = sig;
    int lines = 0;
    log_printf("--- screens changed:");
    dump(root, 0, &lines);
}

void scene_init(int width, int shift)
{
    g_width = width;
    g_shift = shift;
}

static int stage_shown; // a level is on screen this frame
static float complete_opacity; // how far a stage complete panel is faded in

float scene_complete_panel_opacity(void)
{
    return complete_opacity;
}

int scene_stage_shown(void)
{
    return stage_shown;
}

void scene_before_draw(BYTE *engine)
{
    saved_count = 0;
    BYTE *root = scene_root(engine);
    static BYTE *last_root = (BYTE *)1;
    if (root != last_root) {
        log_printf("--- scene root %p", root);
        last_root = root;
    }
    stage_shown = 0;
    complete_opacity = 0;
    if (!root) return;
    dump_on_change(root);
    stage_shown = find(root, "StageContainer", 0) != NULL;

    // Options screen: the "Co-op:" button FF2 Remaster adds (options.xml) shows the mode (coop.c
    // cycles it), and the Online checkbox beside it shows the setting when the screen opens
    // and stores any change (CheckBox vtable +0x160 IsChecked, +0x164 SetChecked, as the
    // options screen itself uses them, 0x402d60 / 0x520768); the address friends type in
    // goes below.
    coop_set_text(coop_button_text(find(root, "coopModeBtn", 0)), coop_mode_name());
    typedef BYTE (__thiscall *IsChecked_t)(BYTE *);
    typedef void (__thiscall *SetChecked_t)(BYTE *, int);
    static BYTE *online_check;
    BYTE *check = find(root, "onlineOptionCheck", 0);
    if (check && check != online_check) {
        ((SetChecked_t)(*(void ***)check)[0x164 / 4])(check, online_requested());
    } else if (check) {
        int on = ((IsChecked_t)(*(void ***)check)[0x160 / 4])(check) != 0;
        if (on != online_requested()) online_request(on);
    }
    online_check = check;
    BYTE *address = find(root, "onlineAddressText", 0);
    if (address) {
        char text[96];
        online_address_text(text, sizeof text);
        coop_set_text(address, text);
    }
    // and the players' color buttons below it show their colors (coop.c cycles them)
    char name[24];
    for (int i = 2; i <= MAX_PLAYERS; i++) {
        sprintf(name, "p%dColorBtn", i);
        BYTE *text = coop_button_text(find(root, name, 0));
        if (!text) continue;
        char label[32];
        sprintf(label, "P%d %s", i, coop_color_name(i));
        coop_set_text(text, label);
    }
    // The HUD's per-player scores (hudlayout.xml), by the shared one.
    if (stage_shown) {
        BYTE *texts[MAX_PLAYERS];
        for (int i = 0; i < MAX_PLAYERS; i++) {
            sprintf(name, "p%dScoreText", i + 1);
            texts[i] = find(root, name, 0);
        }
        coop_show_scores(find(root, "scoreText", 0), texts);
    }

    // Splash screen (splash/splashScreen.xml names its background "splashBg"), and the menu
    // painting FF2 Remaster adds behind the level map (gamemapbase.xml, "ambientBg").
    cover_screen(find(root, "splashBg", 0));
    cover_screen(find(root, "ambientBg", 0));
    // Generic loading screen: "LoadScreen" with a load bar cap. The game map screen is
    // also called "LoadScreen" but its background must stay aligned with its level pins.
    BYTE *load = find(root, "LoadScreen", 0);
    if (load && find(load, "loadBarCap", 0)) cover_screen(find(load, "bg", 0));

    // Fade the HUD out as much as a stage start / complete panel is faded in, so it's
    // revealed smoothly as the panel fades away (originally it simply sat underneath).
    float panel = find_opacity(root, "Stage Start Base", 1, 0);
    complete_opacity = find_opacity(root, "Stage 1 Complete", 1, 0);
    float a = find_opacity(root, "StageCompleteBonus", 1, 0);
    if (a > complete_opacity) complete_opacity = a;
    if (complete_opacity > panel) panel = complete_opacity;
    static float logged = -1;
    if (panel - logged > 0.2f || logged - panel > 0.2f || (panel == 0) != (logged == 0)) {
        log_printf("panel opacity %.2f", panel);
        logged = panel;
    }
    if (panel > 0) {
        BYTE *hud = find(root, "Game Hud", 0);
        if (hud && save(hud)) *(float *)(hud + WIDGET_OPACITY) *= 1 - panel;
    }
}

void scene_after_draw(void)
{
    while (saved_count > 0) {
        Saved *s = &saved[--saved_count];
        *(float *)(s->widget + WIDGET_X) = s->x;
        *(float *)(s->widget + WIDGET_Y) = s->y;
        *(float *)(s->widget + WIDGET_SCALE_X) = s->sx;
        *(float *)(s->widget + WIDGET_SCALE_Y) = s->sy;
        *(float *)(s->widget + WIDGET_OPACITY) = s->opacity;
        *(DWORD *)(s->widget + WIDGET_HIDE_FLAGS) = s->hide;
    }
}
