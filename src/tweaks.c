// Gameplay changes beyond the picture, each with its setting in ff2wide.ini:
//  - golden_minnows: from the first level with one on, a golden minnow in every level (the
//    stage data, changed by make_saf_mods.py / Setup), coming in where it can be seen;
//  - frenzy_waits_for_mermaid: after a perfect stage, the Frenzy chain holds until the
//    mermaid comes.
#include <windows.h>
#include "log.h"
#include "tweaks.h"
#include "coop.h"

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

// --- Golden minnows: the Remaster's or the original's --------------------------------------
// The game data has both: the levels' own golden minnow spawners, and from the first level
// with one on, the Remaster's two (a sure one and a rare one, make_saf_mods.py / Setup),
// told apart by poolSize="1" (+0xa8; fish spawners don't use it otherwise, and it's 0 when
// not given). FishSpawn's update (0x43a180, vtable slot 0x55787c, thiscall(spawn, update,
// flag)) is what spawns; for a golden minnow spawner (behavior "GoldenFish", the std::string
// at +0x60) it runs only for the set the golden_minnows setting picks.
// Every level also has Lives Attack's extra hazards (jellyfish, poison minnows, squid), told
// apart by supply="9999" (+0xac): they spawn only in Lives Attack, where they're the only
// enemies: the level's own (any fish, mines, pelicans, its own hazards) don't come in there;
// clams, pearls, bonuses, golden minnows and the like still do. A spawner's kind is the
// ActorClassInfo it was made from (+0x3c, for its class="..."), told apart by vtable. The
// jellyfish's spawner, a NonPlayerSpawn, has the same update (vtable slot 0x558044) and
// fields.
#define ADDR_FISH_SPAWN_UPDATE_SLOT 0x55787c
#define ADDR_NONPLAYER_SPAWN_UPDATE_SLOT 0x558044
#define FN_FISH_SPAWN_UPDATE   0x43a180
#define SPAWN_SUPPLY           0xac
#define LIVES_ATTACK_SUPPLY    9999
#define SPAWN_CLASS            0x3c
static const DWORD enemy_classes[] = {
    0x54e1c0, // AIFish
    0x54e1e0, // PufferFish
    0x54e200, // Mine
    0x54e468, // JellyFish
    0x54e488, // PoisonFish
    0x54e4c8, // Barracuda
    0x54e4e8, // AlienFish
    0x54e528, // CuttleFish
    0x54e548, // GlowFish
    0x54e568, // Pelican
};
#define SPAWN_BEHAVIOR         0x60      // std::string
#define SPAWN_POOL_SIZE        0xa8
#define REMASTER_POOL_SIZE     1
typedef void (__thiscall *SpawnUpdate_t)(BYTE *spawn, void *update, DWORD flag);
static int remaster_golden = 1;

static const char *std_string(const BYTE *s)
{
    return *(const DWORD *)(s + 0x18) < 0x10 ? (const char *)(s + 4) : *(const char *const *)(s + 4);
}

static int spawns_enemies(const BYTE *spawn)
{
    const BYTE *info = *(const BYTE *const *)(spawn + SPAWN_CLASS);
    for (int i = 0; info && i < (int)(sizeof enemy_classes / sizeof *enemy_classes); i++)
        if (*(const DWORD *)info == enemy_classes[i]) return 1;
    return 0;
}

void __thiscall fish_spawn_update_hook(BYTE *spawn, void *update, DWORD flag)
{
    if (!lstrcmpA(std_string(spawn + SPAWN_BEHAVIOR), "GoldenFish") &&
        (*(int *)(spawn + SPAWN_POOL_SIZE) == REMASTER_POOL_SIZE) != remaster_golden)
        return;
    int lives_attacks = *(int *)(spawn + SPAWN_SUPPLY) == LIVES_ATTACK_SUPPLY;
    if (coop_lives_attack() ? !lives_attacks && spawns_enemies(spawn) : lives_attacks) return;
    ((SpawnUpdate_t)FN_FISH_SPAWN_UPDATE)(spawn, update, flag);
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

void tweaks_init(int golden_minnows, int frenzy_waits, int (*patch_u32)(DWORD, DWORD, DWORD))
{
    remaster_golden = golden_minnows != 0;
    log_printf("golden minnows: %s; Frenzy %s for the mermaid", remaster_golden ? "the Remaster's" : "the original's",
               frenzy_waits ? "waits" : "doesn't wait");
    patch_u32(ADDR_FISH_SPAWN_UPDATE_SLOT, FN_FISH_SPAWN_UPDATE, (DWORD)fish_spawn_update_hook);
    patch_u32(ADDR_NONPLAYER_SPAWN_UPDATE_SLOT, FN_FISH_SPAWN_UPDATE, (DWORD)fish_spawn_update_hook);
    if (frenzy_waits)
        patch_u32(ADDR_MERMAID_STATE_UPDATE_SLOT, FN_MERMAID_STATE_UPDATE, (DWORD)mermaid_state_update_hook);
    if (remaster_golden) patch_u32(ADDR_GOLDEN_ENTER_SLOT, FN_GOLDEN_ENTER, (DWORD)golden_enter_hook);
}
