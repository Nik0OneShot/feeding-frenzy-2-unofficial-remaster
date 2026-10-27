// Gameplay changes beyond the picture:
//  - after a perfect stage, the Frenzy chain holds until the mermaid comes;
//  - golden minnows come in where they can be seen (the stage data, changed by
//    make_saf_mods.py / Setup, has one come in every level).
#include <windows.h>
#include "log.h"
#include "tweaks.h"

#define STAGE_PLAYERS_BEGIN    0x344     // std::vector<PlayerFish*>
#define STAGE_PLAYERS_END      0x348
#define STAGE_VIEW_RECT        0x6c      // float viewport rect x0, y0, x1, y1
#define STAGE_LAYER_SET        0x2d8
#define LAYERSET_CAMERA        0xfc      // float camera x, y: the view's top left in the level

// --- The Frenzy chain holds for the mermaid -------------------------------------------
// A perfect stage (no deaths; the stage update 0x447ef0) switches the stage to MermaidState
// (vtable 0x55871c, update slot 0x558728 = 0x541447). Play goes on, the players' updates
// counting their Frenzy timers down (PlayerFish +0x294, seconds), until no fish are left on
// screen; then the mermaid swims in (the state's +0x1c) and counts the Frenzy bonus. Fish
// take longer to leave a wide screen, so the timer ran out first. While the state waits for
// the mermaid, any drop in a player's timer is undone; eating still adds to the chain.
#define ADDR_MERMAID_STATE_UPDATE_SLOT 0x558728
#define FN_MERMAID_STATE_UPDATE 0x541447
#define STATE_STAGE            0x0c
#define MERMAID_STATE_MERMAID  0x1c      // the mermaid, once it's in
#define PLAYER_FRENZY_TIMER    0x294     // float
#define MAX_HELD               16
typedef void (__thiscall *StateUpdate_t)(BYTE *state, void *update);

void __thiscall mermaid_state_update_hook(BYTE *state, void *update)
{
    BYTE *stage = *(BYTE **)(state + STATE_STAGE), *fish[MAX_HELD];
    float timer[MAX_HELD];
    int n = 0;
    if (stage && !*(void **)(state + MERMAID_STATE_MERMAID)) {
        BYTE **p = *(BYTE ***)(stage + STAGE_PLAYERS_BEGIN), **end = *(BYTE ***)(stage + STAGE_PLAYERS_END);
        for (; p && p < end && n < MAX_HELD; p++)
            if (*p) fish[n] = *p, timer[n++] = *(float *)(*p + PLAYER_FRENZY_TIMER);
    }
    ((StateUpdate_t)FN_MERMAID_STATE_UPDATE)(state, update);
    for (int i = 0; i < n; i++) {
        float *t = (float *)(fish[i] + PLAYER_FRENZY_TIMER);
        if (*t < timer[i]) *t = timer[i];
    }
}

// --- Golden minnows come in on screen --------------------------------------------------
// GoldenFish::EnterStage (0x53fe72, vtable slot 0x55136c, thiscall(fish, stage, arg)) starts
// its trail and enters it where its spawner put it: at the level's side, which in a level
// wider than the view can be far off screen, so it swam away unseen. A golden fish farther
// off than just past the view's edge is moved to just past the nearest one (it then swims in
// from there, heading in as it was), and below or above the view, into it.
#define ADDR_GOLDEN_ENTER_SLOT 0x55136c
#define FN_GOLDEN_ENTER        0x53fe72
#define VTBL_ACTOR_SET_POS     0x28      // thiscall(actor, float x, float y)
#define VTBL_ACTOR_GET_POS     0x2c      // thiscall(actor) -> float *
#define FISH_START_POS         0x190     // float x, y: where it entered, set by EnterStage
#define GOLDEN_OUTSIDE_X       30.0f     // how far past the view's side it comes in from
#define GOLDEN_MARGIN_Y        50.0f     // how far inside the view's top and bottom
typedef void (__thiscall *Enter_t)(BYTE *fish, BYTE *stage, int arg);
typedef float *(__thiscall *GetPos_t)(BYTE *);
typedef void (__thiscall *SetPos_t)(BYTE *, float, float);

static float clamp_inside(float v, float lo, float hi)
{
    return hi < lo ? (lo + hi) / 2 : v < lo ? lo : v > hi ? hi : v;
}

void __thiscall golden_enter_hook(BYTE *fish, BYTE *stage, int arg)
{
    ((Enter_t)FN_GOLDEN_ENTER)(fish, stage, arg);
    BYTE *layer_set = stage ? *(BYTE **)(stage + STAGE_LAYER_SET) : NULL;
    if (!layer_set) return;
    float *view = (float *)(stage + STAGE_VIEW_RECT), *camera = (float *)(layer_set + LAYERSET_CAMERA);
    float *at = ((GetPos_t)(*(void ***)fish)[VTBL_ACTOR_GET_POS / 4])(fish);
    float x0 = camera[0] - GOLDEN_OUTSIDE_X, x1 = camera[0] + (view[2] - view[0]) + GOLDEN_OUTSIDE_X;
    float y0 = camera[1] + GOLDEN_MARGIN_Y, y1 = camera[1] + (view[3] - view[1]) - GOLDEN_MARGIN_Y;
    float x = clamp_inside(at[0], x0, x1), y = clamp_inside(at[1], y0, y1);
    if (x == at[0] && y == at[1]) return;
    log_printf("golden minnow moved from %.0f,%.0f into view at %.0f,%.0f", at[0], at[1], x, y);
    ((SetPos_t)(*(void ***)fish)[VTBL_ACTOR_SET_POS / 4])(fish, x, y);
    ((float *)(fish + FISH_START_POS))[0] = x;
    ((float *)(fish + FISH_START_POS))[1] = y;
}

void tweaks_init(int (*patch_u32)(DWORD, DWORD, DWORD))
{
    patch_u32(ADDR_MERMAID_STATE_UPDATE_SLOT, FN_MERMAID_STATE_UPDATE, (DWORD)mermaid_state_update_hook);
    patch_u32(ADDR_GOLDEN_ENTER_SLOT, FN_GOLDEN_ENTER, (DWORD)golden_enter_hook);
}
