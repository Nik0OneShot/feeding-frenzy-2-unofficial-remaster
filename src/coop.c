// Co-op: up to MAX_PLAYERS fish. Player 1 steers with the mouse; the others join by pressing a
// button on a device of their own: a gamepad, a keyboard's WASD or arrow keys, a mouse of
// its own (up to LOCAL_PLAYERS on this PC), or an online guest's controls.
//
// PlayerSpawn::Spawn (0x4040f0) creates the PlayerFish and keeps it in a ref-counted holder
// (+0x8c); PlayerSpawn::Activate (0x490830) then sets it up and calls PlayerFish::EnterStage
// (0x406de0), which adds every PlayerFish to the stage's player vector (stage+0x340), from
// which collisions, eating and the actor lists pick it up. Running Spawn + Activate again
// therefore gives another fully live fish. As a stage starts, a fish is made (Spawn) for every
// other player; a player's fish enters (Activate) once a device has joined as that player,
// and from then on at every stage start. The holder is put back to the fish it tracks.
//
// The spawner looks after one fish, the "keeper": player 1's, until player 1 runs out of
// lives while others play on; then another player's fish takes its place (hand_over). The
// game handles the keeper's deaths, respawns and lives (its count, GameState +0x10, is the
// keeper's); everyone else's are handled here.
//
// The mouse drives player 1 by accumulating per-frame movement into player+0x184/+0x188
// (RelativeMouse delta signal, 0x427cb0) and by calling PlayerFish::OnButtonDown/Up
// (0x414d00 / 0x40c620; button 0 = dash, 1 = special). The other players get the same, from
// their devices.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "ambient.h"
#include "coop.h"
#include "../shared/label.h"
#include "online.h"
#include "../shared/proto.h"
#include "mice.h"
#include "mouse.h"

#define ADDR_PLAYER_SPAWN_SLOT 0x55819c  // PlayerSpawn interface vtable +0x10
#define FN_PLAYER_SPAWN        0x4040f0
#define SPAWN_FISH_HOLDER      0x8c      // smart pointer to the spawned PlayerFish
#define SPAWN_STAGE            0x28
#define STAGE_PLAYERS_BEGIN    0x344     // std::vector<PlayerFish*>
#define STAGE_PLAYERS_END      0x348
#define PLAYER_STAGE           0x78
#define FN_PLAYER_LEAVE        0x404e10  // PlayerFish::LeaveStage, see coop_leave_hook
typedef void (__thiscall *Leave_t)(BYTE *player);
#define PLAYER_MOVE_X          0x184     // int, accumulated movement in game pixels
#define PLAYER_MOVE_Y          0x188
#define FN_PLAYER_BUTTON_DOWN  0x414d00  // thiscall(PlayerFish*, int button), ret 4
#define FN_PLAYER_BUTTON_UP    0x40c620
#define ENGINE_RELATIVE_MOUSE  0x2a8     // RelativeMouse*; +0x50 = active (gameplay)

typedef void (__thiscall *Spawn_t)(BYTE *spawn, int *count);
typedef void (__thiscall *Button_t)(BYTE *player, int button);

static int enabled;
static float pad_speed = 700;  // game pixels per second at full stick / key
static BYTE *g_stage, *g_spawn;
static int creating;
static DWORD keeper_dying_tick;          // the keeper's death is playing, others swim on

typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } PadState;
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, void *);
static XInputGetState_t xinput_get_state;

// What steers a player, for the session: a kind and its number.
#define DEV_NONE               0
#define DEV_PAD                0x100     // XInput gamepad 0..3
#define DEV_KEYS               0x200     // keyboard * 2 + 0 (W A S D) or 1 (arrow keys)
#define DEV_MOUSE              0x300     // a mouse of its own (mice.c's id)
#define DEV_GUEST              0x400     // an online guest's controls (guest 0..)
#define DEV_KIND(d)            ((d) & 0xff00)
#define DEV_INDEX(d)           ((d) & 0xff)

typedef struct {
    int device;                // DEV_*; player 1 has none (the mouse, the game's own way)
    BYTE *fish;                // its PlayerFish this stage, made as the stage starts
    int entered;               // its fish has entered this stage
    int out;                   // its fish left after dying; back after respawn_timer
    float respawn_timer;
    int lives;                 // its own, counting the current one; -1: not yet. The keeper's
                               // are the game's count instead.
    int level;                 // its growth level
    int growth;                // its own growth counter (see Growth); -1: not known
    float tint[4];             // the color its fish is drawn with (player 1: none)
    int score;                 // its own points this game
    float carry_x, carry_y;    // movement not handed to the fish yet (fractions)
    float in_x, in_y;          // stick / key direction this frame, length <= 1
    int dash_down, special_down, mouse_held;
    BYTE *tinted_visual;       // its visual while tinted for a draw, with what it was
    float saved_color[4];
    int saved_mode;
} Player;
static Player pl[MAX_PLAYERS];
static int keeper;                       // the player whose fish the spawner holds

// This level's mode (the Options screen's Co-op button, see Modes): playing together, or a
// competitive one. A competitive round ends when someone wins (winner, a player index).
enum { MODE_TOGETHER, MODE_HUNGER, MODE_COUNT };
static int mode, mode_requested;
static int requested;                    // co-op switched on (the Options button, coop_request)
static int now_playing;                  // play is on (not menus, panels, between stages)
static int winner = -1;

static int competitive(void)
{
    return enabled && mode != MODE_TOGETHER;
}

static int growth_left = -1;             // what we last left in the game's growth counter
static int last_game_lives = -1000;      // to spot lives the game hands out
static void *death_jingle;               // a player's death jingle is playing (see death_show)
static float sorry_timer;
static int sorry_step;                   // a player's SORRY bubble: 1 shown, 2 popped
static void track_game_lives(void);
static void refresh_lives(void);
static void apply_requested(void);

// Intrusive ref count on engine objects lives in a virtual base: AddRef = its vtable[0].
static void add_ref(BYTE *object)
{
    BYTE *base = object + *(int *)(*(BYTE **)(object + 4) + 4) + 4;
    typedef void (__thiscall *AddRef_t)(BYTE *);
    ((AddRef_t)(*(void ***)base)[0])(base);
}

static int player_count(BYTE *stage)
{
    BYTE **begin = *(BYTE ***)(stage + STAGE_PLAYERS_BEGIN), **end = *(BYTE ***)(stage + STAGE_PLAYERS_END);
    return begin && end >= begin ? (int)(end - begin) : 0;
}

static int in_stage(BYTE *stage, BYTE *player)
{
    BYTE **begin = *(BYTE ***)(stage + STAGE_PLAYERS_BEGIN), **end = *(BYTE ***)(stage + STAGE_PLAYERS_END);
    for (BYTE **p = begin; begin && p < end; p++)
        if (*p == player) return 1;
    return 0;
}

// The game only watches the fish in the spawner: PlayerSpawn::Update (0x438d40) sees it die
// (PlayerFish vtable +0xe4: its state machine is empty), calls its LeaveStage (+0x1c) and
// clears the spawner's done flag; the stage update (0x447ef0) then takes a life, plays
// PlayerDiedState and later runs Activate again, which re-enters the same fish. The other
// players get the same here: leave, take one of their own lives, wait, re-Activate.
#define VTBL_PLAYER_IS_DEAD    0xe4
#define STAGE_GAME             0x68      // GameState*: +0xc = 1 for unlimited lives, +0x10 = lives
#define GAME_UNLIMITED_LIVES   0x0c
#define GAME_LIVES             0x10
#define STAGE_HUD              0x2e0
#define FN_HUD_SET_LIVES       0x402550  // stdcall(Hud*, int lives)
#define RESPAWN_DELAY          2.5f      // seconds, about as long as the game's own death
#define HUNGER_STRIKE_LIVES    3         // each, at every level's start; no extra ones
#define SPAWN_LEVEL            0x90      // int, growth level of the holder's fish
#define SPAWN_DEATHS           0x94      // int; 0 at the end = "perfect" stage (0x447ef0)
#define SPAWN_THRESHOLDS       0x6c      // vector begin, end at +0x70; 8 bytes per level

static int player_dead(BYTE *player)
{
    typedef BOOL (__thiscall *IsDead_t)(BYTE *);
    return (BYTE)((IsDead_t)(*(void ***)player)[VTBL_PLAYER_IS_DEAD / 4])(player);
}

static int *game_lives(BYTE *stage)
{
    BYTE *game = stage ? *(BYTE **)(stage + STAGE_GAME) : NULL;
    return game ? (int *)(game + GAME_LIVES) : NULL;
}

static int unlimited_lives(BYTE *stage)
{
    BYTE *game = stage ? *(BYTE **)(stage + STAGE_GAME) : NULL;
    return game && *(int *)(game + GAME_UNLIMITED_LIVES) == 1;
}

static BYTE *spawner_fish(void)
{
    return g_spawn ? *(BYTE **)(g_spawn + SPAWN_FISH_HOLDER) : NULL;
}

// The holder's pointer only: our reference on the fish put there swaps with the holder's on
// the one taken out.
static void set_spawner_fish(BYTE *fish)
{
    *(BYTE **)(g_spawn + SPAWN_FISH_HOLDER) = fish;
}

static int player_of(BYTE *fish)
{
    for (int i = 0; fish && i < MAX_PLAYERS; i++)
        if (pl[i].fish == fish) return i;
    return -1;
}

// Taking part in this stage: swimming, dying or waiting to come back.
static int present(int i)
{
    return pl[i].fish && pl[i].entered;
}

// In play right now: in the stage's player list and alive.
static int swimming(int i)
{
    BYTE *f = pl[i].fish;
    return f && pl[i].entered && !pl[i].out && g_stage && in_stage(g_stage, f) && !player_dead(f);
}

// Out of lives in a competitive round: out for good (no extra life brings them back), and
// crossed out on the HUD. The keeper never is: the game looks after its lives, and it hands
// the spawner over on its last one (hand_over).
static int eliminated(int i)
{
    return competitive() && present(i) && pl[i].out && pl[i].lives <= 0 && i != keeper;
}

// More than one player takes part (with player 1 alone the game is left as it is).
static int team(void)
{
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) n += present(i);
    return n >= 2;
}

// A spawn outside our own means a new player 1 fish (a new stage): the other players' fish
// are made anew alongside it when it enters.
void __thiscall coop_spawn_hook(BYTE *spawn, int *count)
{
    ((Spawn_t)FN_PLAYER_SPAWN)(spawn, count);
    if (creating) return;
    apply_requested();
    mode = enabled ? mode_requested : MODE_TOGETHER;
    winner = -1;
    // Other fish still in their old stage's player list would haunt the new one (bound to the
    // mouse, invisible, "SORRY!"), so they leave first.
    for (int i = 0; i < MAX_PLAYERS; i++) {
        BYTE *f = pl[i].fish;
        if (i == keeper || !present(i) || pl[i].out) continue;
        BYTE *old_stage = *(BYTE **)(f + PLAYER_STAGE);
        if (old_stage && in_stage(old_stage, f)) {
            ((Leave_t)FN_PLAYER_LEAVE)(f);
            log_printf("co-op: player %d was still in the last stage, taken out", i + 1);
        }
    }
    // The new fish in the spawner is player 1's: the game's count is player 1's again.
    int *lives = game_lives(*(BYTE **)(spawn + SPAWN_STAGE));
    if (keeper != 0 && lives && *lives > 0) {
        pl[keeper].lives = *lives;
        if (pl[0].lives > 0) *lives = pl[0].lives;
        last_game_lives = *lives;
    }
    keeper = 0;
    g_spawn = spawn;
    g_stage = NULL;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        pl[i].fish = NULL;
        pl[i].entered = pl[i].out = 0;
        pl[i].growth = -1;
        pl[i].tinted_visual = NULL;
    }
    growth_left = -1;
    death_jingle = NULL;
    sorry_step = 0;
}

// The other players' tints, on each one's FishVisual widget: color (RGBA, 0..1, +0x50) and
// color mode (+0x60). The game sets them itself every update (0x48fac0 and the flash
// states): mode 0 normally; mode 1 (additive) or 3 (blend toward a color by its alpha) with a
// flash color while the fish flashes, e.g. the spawn protection blink. So the tint is applied
// for the draw only and undone right after: in mode 0 it multiplies the fish (mode 2, as
// layouts' colorMode="multiply"); in a flash mode it tints the flash color.
#define PLAYER_VISUAL          0x124
#define VTBL_FISH_VISUAL       0x550ad4
#define VISUAL_COLOR           0x50
#define VISUAL_COLOR_MODE      0x60
#define COLOR_MODE_MULTIPLY    2
#define CHILDREN_BEGIN         0x9c      // a widget's children, begin and end
#define CHILDREN_END           0xa0

// Parts of the picture in a color mode of their own don't take the color the fish is drawn
// with: ink darkens the fish's animation that way (mode 2, gray fading back to white) and
// leaves it in that mode, so the tint was gone for good; poison (FishVisual 0x460b10) gives
// the animation (+0xdc) its own color and mode too. Those parts get the tint for the draw
// as well: a multiply is tinted; on the animation a blend also becomes a multiply leaning
// toward its color as on the fish itself, and an additive color brightens the tint (other
// parts keep their glows).
#define VISUAL_ANIMATION       0xdc
#define COLOR_MODE_ADD         1
#define MAX_TINTED_PARTS       160
static struct { BYTE *widget; float color[4]; int mode; } tinted_parts[MAX_TINTED_PARTS];
static int tinted_part_count;

static void tint_part(BYTE *c, const float *by, int any_mode)
{
    int *mode = (int *)(c + VISUAL_COLOR_MODE);
    if (*mode < COLOR_MODE_ADD || *mode > 3 || tinted_part_count >= MAX_TINTED_PARTS) return;
    if (!any_mode && *mode != COLOR_MODE_MULTIPLY) return;
    for (int i = 0; i < tinted_part_count; i++)
        if (tinted_parts[i].widget == c) return;
    float *color = (float *)(c + VISUAL_COLOR);
    tinted_parts[tinted_part_count].widget = c;
    tinted_parts[tinted_part_count].mode = *mode;
    memcpy(tinted_parts[tinted_part_count++].color, color, sizeof tinted_parts[0].color);
    if (*mode == COLOR_MODE_MULTIPLY) {
        for (int i = 0; i < 3; i++) color[i] *= by[i];
    } else if (*mode == 3) {
        float a = color[3] < 0 ? 0 : color[3] > 1 ? 1 : color[3];
        for (int i = 0; i < 3; i++) color[i] = by[i] * (1 - a) + color[i] * a;
    } else {
        for (int i = 0; i < 3; i++) {
            float lit = by[i] + (color[i] > 0 ? color[i] : 0);
            color[i] = lit > 1 ? 1 : lit;
        }
    }
    *mode = COLOR_MODE_MULTIPLY;
}

// Not every part of a picture is a container: some keep other things (floats) where a
// container's child list is, and one in a big fish's picture sent the walk off into nowhere.
// So a child list is only walked if it looks like one (readable, a sensible length), and a
// child only if it's one of the game's objects (its vtable in the game's code).
#define MAX_CHILDREN           32
#define GAME_IMAGE_START       0x401000
#define GAME_IMAGE_END         0x5b0000

static int game_object(const BYTE *p)
{
    return p && !((DWORD)p & 3) && !IsBadReadPtr(p, CHILDREN_END + 4) && *(const DWORD *)p >= GAME_IMAGE_START &&
           *(const DWORD *)p < GAME_IMAGE_END;
}

static void tint_parts(BYTE *w, const float *by, int depth)
{
    BYTE **child = *(BYTE ***)(w + CHILDREN_BEGIN), **end = *(BYTE ***)(w + CHILDREN_END);
    if (!child || end < child || end - child > MAX_CHILDREN || ((DWORD)child & 3) ||
        IsBadReadPtr(child, (end - child) * sizeof *child))
        return;
    for (; child < end; child++) {
        BYTE *c = *child;
        if (!game_object(c)) continue;
        tint_part(c, by, 0);
        if (depth < 4) tint_parts(c, by, depth + 1);
    }
}

// Flashes (the spawn protection blink is a ColorFlashModifier: additive glow, mode 1, with a
// dim gray pulsing up to ~0.24) draw the fish itself untinted, so tinting the flash color
// barely shows. For a tinted fish a flash becomes a pulse of its own tint instead: multiply
// by the tint brightened toward white, following the flash's strength.
#define FLASH_FULL             0.24f     // flash color at its brightest
// Multiply ignores the color's alpha, so a multiply color that already carries the tint is
// marked with this alpha, and isn't tinted a second time.
#define TINTED_ALPHA           0.987654f
#define COLOR_MODE_BLEND       3         // toward the color by its alpha

static void flash_to_tint(BYTE *visual, const float *tint)
{
    float *color = (float *)(visual + VISUAL_COLOR);
    float strength = color[0] > color[1] ? color[0] : color[1];
    if (color[2] > strength) strength = color[2];
    strength /= FLASH_FULL;
    if (strength > 1) strength = 1;
    if (strength < 0) strength = 0;
    for (int i = 0; i < 3; i++) color[i] = tint[i] + (1 - tint[i]) * 0.7f * strength;
    color[3] = TINTED_ALPHA;
    *(int *)(visual + VISUAL_COLOR_MODE) = COLOR_MODE_MULTIPLY;
}

// ColorFlashModifier::Apply(update, widget), via its interface vtable slot 0x55c1b4 (a thunk
// to 0x481870): sets the widget's color mode and pulsing color. Caught right after it runs,
// whenever it runs, so a tinted fish's flash is converted before it can be drawn.
#define ADDR_COLOR_FLASH_SLOT  0x55c1b4
#define FN_COLOR_FLASH_APPLY   0x4c3320
typedef void (__thiscall *FlashApply_t)(BYTE *modifier, void *update, BYTE *widget);

void __thiscall coop_color_flash_hook(BYTE *modifier, void *update, BYTE *widget)
{
    ((FlashApply_t)FN_COLOR_FLASH_APPLY)(modifier, update, widget);
    for (int i = 1; enabled && i < MAX_PLAYERS; i++)
        if (pl[i].fish && widget == *(BYTE **)(pl[i].fish + PLAYER_VISUAL)) {
            flash_to_tint(widget, pl[i].tint);
            break;
        }
}

static void tint_fish(Player *p)
{
    BYTE *visual = *(BYTE **)(p->fish + PLAYER_VISUAL);
    if (!visual || *(DWORD *)visual != VTBL_FISH_VISUAL) {
        static int logged;
        if (!logged++) log_printf("co-op: visual %p has vtable %08lx, not tinting", visual, visual ? *(DWORD *)visual : 0);
        return;
    }
    float *color = (float *)(visual + VISUAL_COLOR);
    int *mode = (int *)(visual + VISUAL_COLOR_MODE);
    memcpy(p->saved_color, color, sizeof p->saved_color);
    p->saved_mode = *mode;
    p->tinted_visual = visual;
    if (*mode == 0) {
        memcpy(color, p->tint, sizeof p->tint);
        *mode = COLOR_MODE_MULTIPLY;
    } else if (*mode == COLOR_MODE_MULTIPLY) {
        // The game's own multiply (e.g. lighting out of the water): tint on top of it.
        if (color[3] != TINTED_ALPHA)
            for (int i = 0; i < 3; i++) color[i] *= p->tint[i];
    } else if (*mode == COLOR_MODE_BLEND) {
        // The game blends the fish toward a color (out of the water, stage-wide flashes): as
        // a multiply that keeps the tint and leans toward that color by the same amount.
        float a = color[3] < 0 ? 0 : color[3] > 1 ? 1 : color[3];
        for (int i = 0; i < 3; i++) color[i] = p->tint[i] * (1 - a) + color[i] * a;
        *mode = COLOR_MODE_MULTIPLY;
    } else {
        flash_to_tint(visual, p->tint);
    }
    if (*mode != COLOR_MODE_MULTIPLY) return;
    BYTE *animation = *(BYTE **)(visual + VISUAL_ANIMATION);
    if (game_object(animation)) tint_part(animation, color, 1);
    tint_parts(visual, color, 0);
}

// PlayerFish's eater interface (at +0xf0) TryEat(prey), vtable slot 0x552de8: the stage's
// collision pass (0x440660) calls it for every pair of fish where one is big enough to eat
// the other. Players are on the same team, so they never eat each other.
#define ADDR_PLAYER_TRY_EAT_SLOT 0x552de8
#define FN_PLAYER_TRY_EAT        0x43dee0
#define VTBL_PLAYER_FISH         0x552ca4
typedef void (__thiscall *TryEat_t)(BYTE *eater, BYTE *prey);

// Clams (Oyster, vtable 0x5523bc, eater interface at +0xf0 like the players') eat a player
// with their own TryEat. The pass looks at each pair once, in list order: with the clam first
// and the player able to "eat" it by size, the player gets the turn (and can't eat a clam) and
// the clam never gets one. A fish that re-entered (a respawn) comes after the clams in the
// list, so it was safe from them. A player trying to eat a clam now gives the clam its turn
// too, under the pass's own size rule.
#define VTBL_OYSTER            0x5523bc
#define EATER_IFACE            0xf0
#define VTBL_EDIBLE_SIZE       0x74      // object vtable: the size it counts as when eaten
#define VTBL_EATER_SIZE        0x08      // eater interface vtable: the size it can eat
#define VTBL_EATER_TRY_EAT     0x0c
typedef int (__thiscall *Size_t)(BYTE *);

// Rainbow minnows (ShieldFish, vtable 0x553ac4) escort the fish that took the bonus; the one
// in front stands in for it (the fish itself can't be eaten while it has any, 0x446560), and
// enemies eat that one instead. A minnow only refuses its owner as an eater (0x50b261), so
// other players ate them, and its owner then had minnows on its list but none left to eat:
// enemies couldn't touch it any more. Players leave all minnows alone.
#define VTBL_SHIELD_FISH       0x553ac4

void __thiscall coop_try_eat_hook(BYTE *eater, BYTE *prey)
{
    if (prey && (*(DWORD *)prey == VTBL_PLAYER_FISH || *(DWORD *)prey == VTBL_SHIELD_FISH)) return;
    ((TryEat_t)FN_PLAYER_TRY_EAT)(eater, prey);
    if (!enabled || !prey || *(DWORD *)prey != VTBL_OYSTER) return;
    BYTE *player = eater - EATER_IFACE, *clam = prey + EATER_IFACE;
    if (((Size_t)(*(void ***)player)[VTBL_EDIBLE_SIZE / 4])(player) >=
        ((Size_t)(*(void ***)clam)[VTBL_EATER_SIZE / 4])(clam))
        return;
    ((TryEat_t)(*(void ***)clam)[VTBL_EATER_TRY_EAT / 4])(clam, player);
}

// The Fury bonus (PlayerFuryState) bypasses TryEat too: it gathers everything on screen that
// says it can be eaten by the fish (vtable +0xa8, 0x539721) and smaller than it, then dashes
// through them, eating each one. Players and rainbow minnows now refuse players there.
#define ADDR_PLAYER_EDIBLE_SLOT  0x552d4c
#define FN_PLAYER_EDIBLE         0x446560
#define ADDR_SHIELD_EDIBLE_SLOT  0x553b6c
#define FN_SHIELD_EDIBLE         0x50b261
typedef char (__thiscall *Edible_t)(BYTE *self, BYTE *eater);

char __thiscall coop_player_edible_hook(BYTE *self, BYTE *eater)
{
    if (eater && *(DWORD *)eater == VTBL_PLAYER_FISH) return 0;
    return ((Edible_t)FN_PLAYER_EDIBLE)(self, eater);
}

char __thiscall coop_shield_edible_hook(BYTE *self, BYTE *eater)
{
    if (eater && *(DWORD *)eater == VTBL_PLAYER_FISH) return 0;
    return ((Edible_t)FN_SHIELD_EDIBLE)(self, eater);
}

// The suck ability bypasses TryEat: the stage's collision pass (0x440660) hands every
// suckable thing near a sucking player to Suck (0x439030, esi = player, edi = item), which
// pulls it in and eats it. That call (at 0x4408d9) now skips player fish and rainbow minnows.
#define ADDR_SUCK_CALL         0x4408d9
#define FN_SUCK                0x439030
void coop_suck_thunk(void);
__asm__(
    ".globl _coop_suck_thunk\n"
    "_coop_suck_thunk:\n"
    "    cmpl $0x552ca4, (%edi)\n"  // the item is a PlayerFish: leave it alone
    "    je 1f\n"
    "    cmpl $0x553ac4, (%edi)\n"  // or a rainbow minnow (ShieldFish, see coop_try_eat_hook)
    "    je 1f\n"
    "    pushl $0x439030\n"         // else on to Suck, registers as they were
    "    ret\n"
    "1:  ret\n");

// The Stun bonus (0x53cb8a) stuns every fish on screen but the one that took it: each that
// can be stunned (vtable +0xc4, asked at 0x53cc26) gets the jellyfish stun (+0xd0, for a
// player PlayerFish::Stun), so it stunned the other players. That question now says no for
// player fish (esi = ecx = the fish, eax = its vtable, one argument).
#define ADDR_STUN_BONUS_ASK    0x53cc26  // ff 90 c4 00 00 00: call [eax + 0xc4]
void coop_stun_bonus_thunk(void);
__asm__(
    ".globl _coop_stun_bonus_thunk\n"
    "_coop_stun_bonus_thunk:\n"
    "    cmpl $0x552ca4, %eax\n"    // a PlayerFish: can't be stunned by it
    "    je 1f\n"
    "    jmp *0xc4(%eax)\n"         // else the fish's own answer, returning to the loop
    "1:  xorl %eax, %eax\n"
    "    ret $4\n");

// In-game key handler (0x499730) pauses on P, Esc and Space. Space is a player's dash, so
// in co-op its check (cmp eax, 0x20 at 0x4f45d3) is turned into a second Esc check.
#define ADDR_PAUSE_SPACE_CMP   0x4f45d3
#define PAUSE_SPACE_CMP        0x0f20f883  // 83 f8 20 0f
#define PAUSE_ESC_CMP          0x0f1bf883  // 83 f8 1b 0f

void coop_before_draw(void)
{
    for (int i = 1; enabled && g_stage && i < MAX_PLAYERS; i++)
        if (present(i) && in_stage(g_stage, pl[i].fish)) tint_fish(&pl[i]);
}

void coop_after_draw(void)
{
    while (tinted_part_count > 0) {
        tinted_part_count--;
        memcpy(tinted_parts[tinted_part_count].widget + VISUAL_COLOR, tinted_parts[tinted_part_count].color,
               sizeof tinted_parts[0].color);
        *(int *)(tinted_parts[tinted_part_count].widget + VISUAL_COLOR_MODE) = tinted_parts[tinted_part_count].mode;
    }
    for (int i = MAX_PLAYERS - 1; i >= 1; i--) {
        Player *p = &pl[i];
        if (!p->tinted_visual) continue;
        memcpy(p->tinted_visual + VISUAL_COLOR, p->saved_color, sizeof p->saved_color);
        *(int *)(p->tinted_visual + VISUAL_COLOR_MODE) = p->saved_mode;
        p->tinted_visual = NULL;
    }
}

// The other players' steering. The mouse pushes a fish: each frame's movement becomes a
// force, capped, so turning against your speed takes a while, which suits flicking a mouse
// but makes keys and sticks feel sluggish. So with stick / key input a player's velocity is
// steered straight toward (direction x top speed), right after the game's water drag
// (PlayerFish::Physics 0x43b2e0 calls the drag, then caps the speed unless dashing). During
// a dash the speed is kept and only its direction turns toward the input.
#define PLAYER_VELOCITY        0xb0      // float x, y
#define PLAYER_STOPPED         0xc9
#define PLAYER_MAX_SPEED       0x144     // float x, y; times +0x260 and the speed option
#define PLAYER_SPEED_SCALE     0x260
#define PLAYER_DASH_TIME       0x1d8     // float, > 0 while dashing
#define PLAYER_FREE_Y          0x19c     // byte, 0 = up/down movement locked
#define PLAYER_IN_WATER        0x198     // float, 1 = fully under water
#define STEER_TIME             0.08f     // seconds to get ~2/3 of the way to the target speed
#define DASH_STEER_TIME        0.06f

// A stung fish (jellyfish, PlayerFish::Stun 0x496360) is put in PlayerStunnedState, which
// ignores its controls; the steering below is a player's controls, so it waits too. The
// fish's state manager is at +0x154 (slot 0x20: the current state).
#define PLAYER_STATES          0x154
#define VTBL_PLAYER_STUNNED    0x553114
#define VTBL_PLAYER_STUNNED_IFACE 0x55318c
#define STATES_VTBL_CURRENT    0x20

static int player_stunned(BYTE *player)
{
    typedef BYTE *(__thiscall *Current_t)(BYTE *);
    BYTE *states = player + PLAYER_STATES;
    BYTE *state = ((Current_t)(*(void ***)states)[STATES_VTBL_CURRENT / 4])(states);
    // the state itself, or its engine::Interface part (+0x38, vtable 0x55318c)
    return state && (*(DWORD *)state == VTBL_PLAYER_STUNNED || *(DWORD *)state == VTBL_PLAYER_STUNNED_IFACE);
}

// A poison minnow (PlayerFish 0x53ea78 -> 0x53c595) poisons the fish that ate it, on its
// FishVisual: +0x2f0 set, and a phase (+0x304) into a list of phase times (std::vector at
// +0x2f4: begin +0x2f8, end +0x2fc). In phases 0 to 2 PlayerFish's mouse force (0x43aa40) is
// turned around and weakened, x -0.7; the other players' sticks and keys are too.
#define VISUAL_POISONED        0x2f0
#define VISUAL_POISON_BEGIN    0x2f8
#define VISUAL_POISON_END      0x2fc
#define VISUAL_POISON_PHASE    0x304
#define POISON_STEER           -0.7f

static int player_poisoned(BYTE *player)
{
    BYTE *visual = *(BYTE **)(player + PLAYER_VISUAL);
    if (!visual || *(DWORD *)visual != VTBL_FISH_VISUAL || !visual[VISUAL_POISONED]) return 0;
    BYTE *begin = *(BYTE **)(visual + VISUAL_POISON_BEGIN), *end = *(BYTE **)(visual + VISUAL_POISON_END);
    int phase = *(int *)(visual + VISUAL_POISON_PHASE);
    return begin && (unsigned)phase < (unsigned)(end - begin) / 4 && phase - 1 < 2;
}

// Out of the water the game's physics (gravity) carry the fish; a jump out takes a dash,
// like player 1's. Sticks and keys still steer it there, more softly: sideways toward the
// usual top speed while held, and up or down by a push that eases the fall or speeds it but
// is far below gravity (2000 pixels/s^2 out of the water, 0x43b2e0), so the fish can't float.
#define AIR_STEER_TIME         0.3f      // seconds to get ~2/3 of the way to the sideways speed
#define AIR_PUSH               450.0f    // up/down, pixels/s^2

void coop_steer(BYTE *player, float dt, float speed_option)
{
    int who = enabled ? player_of(player) : -1;
    if (who < 1 || !pl[who].entered || pl[who].out) return;
    Player *p = &pl[who];
    if ((p->in_x == 0 && p->in_y == 0) || player_stunned(player)) return; // let go: glide on, like the mouse
    float ix = p->in_x, iy = p->in_y;
    if (player_poisoned(player)) ix *= POISON_STEER, iy *= POISON_STEER;
    float *v = (float *)(player + PLAYER_VELOCITY);
    float *max = (float *)(player + PLAYER_MAX_SPEED);
    if (*(float *)(player + PLAYER_IN_WATER) < 0.999f) {
        float scale = *(float *)(player + PLAYER_SPEED_SCALE) * speed_option;
        if (ix != 0) v[0] += (ix * max[0] * scale - v[0]) * (1 - expf(-dt / AIR_STEER_TIME));
        v[1] += iy * AIR_PUSH * dt;
        player[PLAYER_STOPPED] = 0;
        return;
    }
    float x = ix, y = player[PLAYER_FREE_Y] ? iy : 0;
    if (x == 0 && y == 0) return;
    if (*(float *)(player + PLAYER_DASH_TIME) > 0) {
        float speed = sqrtf(v[0] * v[0] + v[1] * v[1]), len = sqrtf(x * x + y * y);
        float k = 1 - expf(-dt / DASH_STEER_TIME);
        float nx = v[0] + (x / len * speed - v[0]) * k, ny = v[1] + (y / len * speed - v[1]) * k;
        float n = sqrtf(nx * nx + ny * ny);
        if (n < 0.001f) nx = x / len, ny = y / len, n = 1;
        v[0] = nx / n * speed;
        v[1] = ny / n * speed;
    } else {
        float scale = *(float *)(player + PLAYER_SPEED_SCALE) * speed_option;
        float k = 1 - expf(-dt / STEER_TIME);
        v[0] += (x * max[0] * scale - v[0]) * k;
        v[1] += (y * max[1] * scale - v[1]) * k;
    }
    player[PLAYER_STOPPED] = 0;
}

// PlayerFish::EnterStage (0x406de0, vtable slot 0x552cbc): adds the fish to the stage's
// player list and starts its spawn (splash-in) state.
#define ADDR_PLAYER_ENTER_SLOT 0x552cbc
#define FN_PLAYER_ENTER        0x406de0
typedef void (__thiscall *Enter_t)(BYTE *player, BYTE *stage, int arg);

// PlayerSpawn::Activate (0x490830, PlayerSpawn vtable +0x1c): sets up the fish in the holder
// (size, level, position, owner spawner at +0x180, ...) and then calls its EnterStage(stage, 7).
// Entering a fish without this leaves those fields empty and its spawn state crashes.
#define FN_SPAWN_ACTIVATE      0x490830
#define SPAWN_DONE             0x98      // byte, set by Activate once its fish has entered
typedef BYTE *(__thiscall *Activate_t)(BYTE *spawn);

// The game binds the mouse to the first fish in the stage's player list when play
// (re)starts (RunState::Enter 0x48d440 and 0x4ea724 call BindMouse, 0x497040,
// stdcall(PlayerFish*)). After player 1 respawns, another player is first in that list.
#define FN_BIND_MOUSE          0x497040
#define ADDR_BIND_MOUSE_CALL_1 0x48d490
#define ADDR_BIND_MOUSE_CALL_2 0x4ea72d
typedef void (__stdcall *BindMouse_t)(BYTE *player);

// Team Frenzy. Each fish keeps its own Frenzy chain (+0x290 count, +0x294 timer, +0x298
// timer length, +0x29c score multiplier) and redraws the HUD's Frenzy meter from it
// (0x4605d0) whenever it changes, so with several fish the meter and multiplier jumped
// between the chains. PlayerFish::AddFrenzy (0x47c550, vtable slot 0x552dd4, called on each
// meal) now starts from a teammate's chain and copies the result to everyone: one shared
// chain. All the copies then time out identically.
#define ADDR_PLAYER_ADD_FRENZY_SLOT 0x552dd4
#define FN_PLAYER_ADD_FRENZY   0x47c550
#define PLAYER_FRENZY          0x290     // 16 bytes, see above
#define FN_HUD_SHOW_FRENZY     0x4605d0  // thiscall(PlayerFish*, Hud*)
typedef void (__thiscall *AddFrenzy_t)(BYTE *player, int count, float time);
typedef void (__thiscall *ShowFrenzy_t)(BYTE *player, BYTE *hud);

// A swimming teammate of player i (its chain is the team's), or -1.
static int frenzy_partner(int i)
{
    for (int j = 0; j < MAX_PLAYERS; j++)
        if (j != i && swimming(j)) return j;
    return -1;
}

void __thiscall coop_add_frenzy_hook(BYTE *player, int count, float time)
{
    int who = enabled && g_stage && team() ? player_of(player) : -1;
    int partner = who >= 0 ? frenzy_partner(who) : -1;
    if (partner >= 0) memcpy(player + PLAYER_FRENZY, pl[partner].fish + PLAYER_FRENZY, 16);
    ((AddFrenzy_t)FN_PLAYER_ADD_FRENZY)(player, count, time);
    for (int j = 0; who >= 0 && j < MAX_PLAYERS; j++)
        if (j != who && present(j) && in_stage(g_stage, pl[j].fish))
            memcpy(pl[j].fish + PLAYER_FRENZY, player + PLAYER_FRENZY, 16);
}

// Entering resets a fish's chain and the meter; it joins the team's chain instead.
static void frenzy_join(int i)
{
    int partner = frenzy_partner(i);
    if (partner < 0) return;
    memcpy(pl[i].fish + PLAYER_FRENZY, pl[partner].fish + PLAYER_FRENZY, 16);
    ((ShowFrenzy_t)FN_HUD_SHOW_FRENZY)(pl[partner].fish, *(BYTE **)(g_stage + STAGE_HUD));
}

// Set up and enter a player's fish through the spawner, exactly as Activate does for the
// keeper: the fish goes in the holder for the call, and the holder and the done flag are put
// back afterwards.
static void activate_player(int i)
{
    Player *p = &pl[i];
    BYTE *held = spawner_fish();
    BYTE done = g_spawn[SPAWN_DONE];
    // Activate sets the fish up at the spawner's size (the keeper's). In Hunger Strike everyone
    // has their own: back at it after being eaten, or the smallest when joining.
    int *level = (int *)(g_spawn + SPAWN_LEVEL), keeper_level = *level;
    int own = competitive() && mode == MODE_HUNGER && i != keeper ? (p->growth >= 0 ? p->level : 0) : keeper_level;
    creating = 1;
    set_spawner_fish(p->fish);
    g_spawn[SPAWN_DONE] = 0;
    *level = own;
    ((Activate_t)FN_SPAWN_ACTIVATE)(g_spawn);
    *level = keeper_level;
    set_spawner_fish(held);
    g_spawn[SPAWN_DONE] = done;
    creating = 0;
    p->entered = 1;
    p->out = 0;
    p->level = own;
    p->carry_x = p->carry_y = 0;
    p->dash_down = 1; // a button still held from joining doesn't dash
    p->special_down = 0;
    if (competitive() && mode == MODE_HUNGER && p->growth < 0) p->lives = HUNGER_STRIKE_LIVES; // joining
    if (p->lives < 0) {
        int *lives = game_lives(g_stage);
        p->lives = lives && *lives > 0 ? *lives : 1;
    }
    if (i == 0) ((BindMouse_t)FN_BIND_MOUSE)(p->fish);
    if (g_stage) frenzy_join(i);
}

void __thiscall coop_enter_hook(BYTE *player, BYTE *stage, int arg)
{
    ((Enter_t)FN_PLAYER_ENTER)(player, stage, arg);
    if (!enabled || creating || !g_spawn || !stage) return;
    if (spawner_fish() != player) return;
    if (g_spawn[SPAWN_DONE]) return; // not inside the spawner's Activate: it would do nothing
    g_stage = stage;
    int k = player_of(player);
    if (k >= 0) {
        // The keeper (re)entering: its respawn (the others are in the stage or waiting for
        // their own), or the next stage without new fish (after a bonus stage), where the
        // others were taken out with it (coop_leave_hook) and come back with it.
        keeper = k;
        for (int j = 0; j < MAX_PLAYERS; j++) {
            if (j == k || !present(j) || pl[j].out) continue;
            if (in_stage(stage, pl[j].fish)) {
                memcpy(pl[j].fish + PLAYER_FRENZY, player + PLAYER_FRENZY, 16); // a death ends the chain
            } else {
                activate_player(j);
                log_printf("co-op: player %d came along to the next stage, stage has %d players", j + 1,
                           player_count(stage));
            }
        }
        return;
    }
    if (player_count(stage) != 1) return;

    // A new stage: player 1's new fish entered; one is made for each other player (we're
    // inside this same call for player 1; it marks the spawner done only after EnterStage
    // returns). Each Spawn drops the holder's reference on the fish it held, so that fish
    // gets one of ours first; the references end up with us, and player 1's with the holder.
    pl[0].fish = player;
    pl[0].entered = 1;
    keeper = 0;
    creating = 1;
    // as many as can join: this PC's players, and the guests' while hosting
    int made = online_hosting() ? MAX_PLAYERS : LOCAL_PLAYERS;
    for (int i = 1; i < made; i++) {
        BYTE *held = spawner_fish();
        int count = 0;
        add_ref(held);
        ((Spawn_t)FN_PLAYER_SPAWN)(g_spawn, &count);
        BYTE *fish = spawner_fish();
        pl[i].fish = fish != held ? fish : NULL;
    }
    set_spawner_fish(player);
    creating = 0;
    // Hunger Strike starts every level with HUNGER_STRIKE_LIVES each (the game's count is
    // player 1's here)
    int *lives = game_lives(stage), hunger = competitive() && mode == MODE_HUNGER;
    if (hunger && lives) *lives = last_game_lives = HUNGER_STRIKE_LIVES;
    int joined = 0;
    for (int i = 1; i < MAX_PLAYERS; i++) {
        if (!pl[i].device || !pl[i].fish) continue;
        if (hunger) pl[i].lives = HUNGER_STRIKE_LIVES;
        else if (pl[i].lives == 0) pl[i].lives = 1; // ran out last stage: back with one
        activate_player(i);
        joined++;
    }
    track_game_lives();
    refresh_lives();
    log_printf("co-op: new stage, %d other players in, stage has %d players", joined, player_count(stage));
}

// Growth. PlayerSpawn::Update (0x438d40, vtable slot 0x5581ac, thiscall with 2 arguments)
// grows the fish in its holder one level (+0x90) whenever the score passes that level's
// threshold: new size, level, "playerGrow" sound and the HUD. The others follow the
// keeper's level: whenever the keeper is ahead, the same update runs with the other fish in
// the holder and the level set to its own, so it grows exactly the same way.
#define ADDR_SPAWN_UPDATE_SLOT 0x5581ac
#define FN_SPAWN_UPDATE        0x438d40
typedef void (__thiscall *SpawnUpdate_t)(BYTE *spawn, void *a, void *b);

static void death_show(void);

// The keeper died on its last life while someone else still plays. The game only knows the
// fish in the spawner: it would end the game. Instead the keeper leaves the stage the way
// the spawner would have taken it out, and the other player's fish takes its place in the
// spawner, with its lives as the game's count: from now on the game looks after that one.
// Everyone is back at the next stage, where the game makes the fish anew.
static void hand_over(BYTE *spawn, int from, int to)
{
    ((Leave_t)FN_PLAYER_LEAVE)(pl[from].fish);
    ++*(int *)(spawn + SPAWN_DEATHS);
    pl[from].out = 1;
    pl[from].lives = 0;
    pl[from].respawn_timer = 1e30f;
    set_spawner_fish(pl[to].fish);
    keeper = to;
    int *lives = game_lives(g_stage);
    if (lives) {
        *lives = pl[to].lives > 0 ? pl[to].lives : 1;
        last_game_lives = *lives;
    }
    death_show();
    refresh_lives();
    log_printf("co-op: player %d is out of lives, the game looks after player %d now (%d lives)", from + 1, to + 1,
               lives ? *lives : 0);
}

// --- Hunger Strike --------------------------------------------------------------------
// Everyone races their own growth bar: each grows from their own counter (see Growth), and
// the first to fill the last size wins. The keeper grows the game's way, from the game's
// counter, which holds the keeper's own growth here; the others the same way, with the
// counter set to theirs for the update. A player other than the keeper who wins takes the
// spawner, with their level and growth, so the game ends the stage for them as for anyone.
static int *growth_counter(void);
static void announce_winner(int i);

static int level_count(void)
{
    int *begin = *(int **)(g_spawn + SPAWN_THRESHOLDS), *end = *(int **)(g_spawn + SPAWN_THRESHOLDS + 4);
    return begin ? (int)(end - begin) / 2 : 0;
}

// The growth that completes size `level` (and grows to the next, or ends the stage).
static int level_goal(int level)
{
    int *begin = *(int **)(g_spawn + SPAWN_THRESHOLDS);
    return level >= 0 && level < level_count() ? begin[level * 2 + 1] : 0x7fffffff;
}

static void hunger_strike_growth(BYTE *spawn, void *a, void *b)
{
    int *level = (int *)(spawn + SPAWN_LEVEL), *counter = growth_counter(), keeper_level = *level, last = level_count() - 1;
    BYTE *held = spawner_fish();
    if (!counter || last < 0) return;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &pl[i];
        if (i == keeper || !swimming(i) || p->growth < 0) continue;
        int grew = 0;
        set_spawner_fish(p->fish);
        while (p->level < last && p->growth >= level_goal(p->level)) {
            *level = p->level;
            *counter = p->growth;
            ((SpawnUpdate_t)FN_SPAWN_UPDATE)(spawn, a, b);
            if (*level != p->level + 1 || !spawn[SPAWN_DONE]) break;
            p->level++;
            grew = 1;
        }
        set_spawner_fish(held);
        *level = keeper_level;
        *counter = pl[keeper].growth;
        spawn[SPAWN_DONE] = 1;
        if (grew) log_printf("hunger strike: player %d grew to size %d", i + 1, p->level + 1);
    }
    if (winner >= 0) return;
    // the last one standing wins, whatever their size: the stage is ended for them as for a
    // full bar (their size's counted as the last)
    int taking_part = 0, standing = 0, last_standing = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!present(i)) continue;
        taking_part++;
        if (!eliminated(i)) standing++, last_standing = i;
    }
    if (taking_part >= 2 && standing == 1) {
        announce_winner(last_standing);
        log_printf("hunger strike: player %d is the last one standing", last_standing + 1);
        if (last_standing != keeper) {
            set_spawner_fish(pl[last_standing].fish); // our references on the two swap with the spawner's
            keeper = last_standing;
        }
        *level = last;
        *counter = pl[keeper].growth = level_goal(last);
        return;
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        int lvl = i == keeper ? keeper_level : pl[i].level;
        if (!present(i) || pl[i].growth < 0 || lvl < last || pl[i].growth < level_goal(last)) continue;
        announce_winner(i);
        if (i != keeper) {
            // the spawner's fish becomes the winner's, with its size: the stage ends as usual
            set_spawner_fish(pl[i].fish); // our references on the two swap with the spawner's
            keeper = i;
            *level = pl[i].level;
            *counter = pl[i].growth;
        }
        break;
    }
}

static void spawn_update(BYTE *spawn, void *a, void *b)
{
    BYTE *held = *(BYTE **)(spawn + SPAWN_FISH_HOLDER);
    if (enabled && !creating && spawn == g_spawn && g_stage && team() && spawn[SPAWN_DONE] && held &&
        player_dead(held) && !unlimited_lives(g_stage)) {
        // The keeper died. The stage takes a life and ends the game when none are left
        // (0x447ef0: the count is lives including the current one). On its last life, while
        // another player still plays, that one takes over.
        int *lives = game_lives(g_stage);
        int to = -1;
        for (int j = 0; lives && *lives <= 1 && j < MAX_PLAYERS && to < 0; j++)
            if (j != keeper && swimming(j)) to = j;
        for (int j = 0; lives && *lives <= 1 && j < MAX_PLAYERS && to < 0; j++) {
            if (j == keeper || !present(j) || !pl[j].out || pl[j].lives <= 0) continue;
            // Waiting to respawn with lives left: brought in now.
            activate_player(j);
            log_printf("co-op: player %d respawned early to take over", j + 1);
            if (swimming(j)) to = j;
        }
        if (to >= 0) {
            hand_over(spawn, keeper, to);
            held = pl[to].fish;
        }
    }
    ((SpawnUpdate_t)FN_SPAWN_UPDATE)(spawn, a, b);
    if (spawn == g_spawn && held == pl[keeper].fish) pl[keeper].level = *(int *)(spawn + SPAWN_LEVEL);
    if (!enabled || creating || spawn != g_spawn || !g_stage || !spawn[SPAWN_DONE] || held != pl[keeper].fish) return;
    if (competitive() && mode == MODE_HUNGER) {
        hunger_strike_growth(spawn, a, b);
        return;
    }
    int *level = (int *)(spawn + SPAWN_LEVEL);
    int keeper_level = *level;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &pl[i];
        if (i == keeper || p->level >= keeper_level || !swimming(i)) continue;
        set_spawner_fish(p->fish);
        while (p->level < keeper_level) {
            *level = p->level;
            ((SpawnUpdate_t)FN_SPAWN_UPDATE)(spawn, a, b);
            if (*level != p->level + 1 || !spawn[SPAWN_DONE]) break;
            p->level++;
        }
        set_spawner_fish(held);
        *level = keeper_level;
        spawn[SPAWN_DONE] = 1;
        log_printf("co-op: player %d grew to level %d (the keeper, player %d: %d)", i + 1, p->level, keeper + 1,
                   keeper_level);
    }
}

// A fish leaving the stage during the spawner's update is a death (or a keeper handing
// over); any other time it's the stage ending (coop_leave_hook).
static int spawn_updating;

void __thiscall coop_spawn_update_hook(BYTE *spawn, void *a, void *b)
{
    spawn_updating++;
    spawn_update(spawn, a, b);
    spawn_updating--;
}

// PlayerFish::LeaveStage (0x404e10, vtable slot 0x552cc0, thiscall, no arguments) is the only
// way a fish leaves the stage's player list. At the end of a stage the game calls it for the
// keeper only, so the others stayed behind in the list into the next stage, half alive (the
// mouse got bound to one, the stage thought the player had died). So when the keeper leaves
// alive, the others leave with it, and come back when it next enters.
#define ADDR_PLAYER_LEAVE_SLOT 0x552cc0

void __thiscall coop_leave_hook(BYTE *player)
{
    BYTE *stage = *(BYTE **)(player + PLAYER_STAGE);
    ((Leave_t)FN_PLAYER_LEAVE)(player);
    // Not "is it dead": at the end of a bonus stage the keeper's state is already cleared
    // when it leaves, which looked like a death and left the others behind in the stage.
    if (!enabled || creating || spawn_updating || !g_spawn || player != spawner_fish() || player_of(player) < 0) return;
    int left = 0;
    for (int j = 0; j < MAX_PLAYERS; j++) {
        if (j == keeper || !present(j)) continue;
        if (!pl[j].out && stage && in_stage(stage, pl[j].fish)) {
            ((Leave_t)FN_PLAYER_LEAVE)(pl[j].fish);
            left++;
        }
        pl[j].out = 0; // all back with the keeper
        if (pl[j].lives == 0) pl[j].lives = 1;
    }
    g_stage = NULL;
    log_printf("co-op: the stage is over, %d other players left with player %d", left, keeper + 1);
}

// --- Lives -------------------------------------------------------------------------
// Each player has their own: the keeper uses the game's count (GameState +0x10), the others
// their own, starting from the game's count when they join. Lives the game gives (extra life
// every so many points, 1-up bubbles) go to everyone. A continue after a game over refills
// everyone. In a competitive round a player out of lives is out for good (eliminated), and
// crossed out on the HUD. The HUD's life count (Hud +0x198, a TextItem) shows "3/X/2", each
// number but player 1's in that player's tint (HudSetLives, 0x402550, stdcall(Hud*, int
// lives), is replaced; smaller with more numbers; the text is set whole, text_assign). The
// numbers are right-aligned, so drawing the end of the text alone on top lands exactly on
// that part: from each player's number on, in that player's tint, then the rest after it
// plain again (the TextItem gets a private vtable with its own Draw).
#define FN_HUD_SET_LIVES_START 0x402550
#define HUD_LIVES_TEXT         0x198
#define VTBL_TEXT_ITEM         0x55c32c
#define FN_TEXT_ITEM_DRAW      0x4471b0  // vtable +0xa0, thiscall(TextItem*, DrawContext*)
#define VTBL_DRAW              0xa0
#define FN_STRING_FROM_CSTR    0x44be60  // std::string(const char *), thiscall
#define FN_STRING_FREE         0x448860  // thiscall(std::string*, 1, 0)
#define DRAWCTX_STATE_DEQUE    0x38
#define STATE_COLOR            0x18      // float r, g, b, a
#define STATE_FLAGS            0x34      // color mode bits: mask [0x5a6360], shift [0x5a6370]
#define WIDGET_SCALE_X         0x3c
#define WIDGET_SCALE_Y         0x40
// The text's size by how many numbers it has, so it stays about as wide as its plug.
static const float lives_scale[MAX_PLAYERS + 1] = { 1, 1, 1, 0.8f, 0.8f, 0.65f, 0.65f, 0.55f, 0.55f, 0.5f, 0.5f };
typedef void (__thiscall *Draw_t)(BYTE *widget, BYTE *ctx);
static BYTE *lives_text;
static void **lives_vtable;
static int shown_count, shown[MAX_PLAYERS], shown_who[MAX_PLAYERS];

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

// The numbers from shown[from] on, joined by "/", after a "/" with slash; an eliminated
// player's (-1) is crossed out, an X.
static void lives_string(char *out, int from, int slash)
{
    int o = 0;
    if (slash) out[o++] = '/';
    for (int k = from; k < shown_count; k++)
        o += shown[k] < 0 ? sprintf(out + o, k + 1 < shown_count ? "X/" : "X")
                          : sprintf(out + o, k + 1 < shown_count ? "%d/" : "%d", shown[k]);
    out[o] = 0;
}

static void text_assign(BYTE *text, const char *value);

static void set_lives_from(BYTE *text, int from, int slash)
{
    char value[80];
    lives_string(value, from, slash);
    text_assign(text, value);
}

// The draw state's position, scale, rotation, opacity and color, and its flags: drawing text
// moves the position along, so each pass starts from where the first one did.
#define STATE_PLAIN_SIZE       0x28
#define STATE_FLAGS_SIZE       0x08      // +0x30, +0x34

static const char *text_string(BYTE *text)
{
    return *(DWORD *)(text + 0xb0) < 0x10 ? (const char *)(text + 0x9c) : *(const char **)(text + 0x9c);
}

void __thiscall lives_text_draw(BYTE *text, BYTE *ctx)
{
    BYTE *state = (BYTE *)draw_state(ctx);
    // During play something else sets this text to the bare count every frame (not
    // HudSetLives; the log showed '3' here during play and '3/3' while paused), so put ours
    // back whenever it was changed.
    if (text == lives_text && shown_count > 1) {
        char want[80];
        lives_string(want, 0, 0);
        if (strcmp(text_string(text), want) != 0) set_lives_from(text, 0, 0);
    }
    BYTE saved[STATE_PLAIN_SIZE], saved_flags[STATE_FLAGS_SIZE];
    if (state) {
        memcpy(saved, state, sizeof saved);
        memcpy(saved_flags, state + 0x30, sizeof saved_flags);
    }
    ((Draw_t)FN_TEXT_ITEM_DRAW)(text, ctx);
    if (text != lives_text || shown_count < 2 || !state || (BYTE *)draw_state(ctx) != state) return;
    DWORD flags = *(DWORD *)(state + STATE_FLAGS), mask = *(DWORD *)0x5a6360;
    BYTE shift = *(BYTE *)0x5a6370;
    for (int k = 1; k < shown_count; k++) {
        memcpy(state, saved, sizeof saved);
        memcpy(state + 0x30, saved_flags, sizeof saved_flags);
        set_lives_from(text, k, 0);
        for (int i = 0; i < 3; i++) ((float *)(state + STATE_COLOR))[i] *= pl[shown_who[k]].tint[i];
        *(DWORD *)(state + STATE_FLAGS) = (flags & ~mask) | (COLOR_MODE_MULTIPLY << shift & mask);
        ((Draw_t)FN_TEXT_ITEM_DRAW)(text, ctx);
        if (k == shown_count - 1) break;
        memcpy(state, saved, sizeof saved);
        memcpy(state + 0x30, saved_flags, sizeof saved_flags);
        set_lives_from(text, k + 1, 1);
        ((Draw_t)FN_TEXT_ITEM_DRAW)(text, ctx);
    }
    memcpy(state, saved, sizeof saved);
    memcpy(state + 0x30, saved_flags, sizeof saved_flags);
    set_lives_from(text, 0, 0);
}

static void show_lives(BYTE *hud, int game_count)
{
    BYTE *text = hud ? *(BYTE **)(hud + HUD_LIVES_TEXT) : NULL;
    if (!text) return;
    int values[MAX_PLAYERS], who[MAX_PLAYERS], n = 0, together = enabled && team();
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i != 0 && !(together && present(i))) continue;
        int v = i == keeper ? game_count : pl[i].lives;
        values[n] = eliminated(i) ? -1 : v < 0 ? 0 : v > 99 ? 99 : v;
        who[n++] = i;
    }
    // The stage update (Stage vtable +0x14, 0x486950) sets the count every frame during play;
    // only rewrite the text when something changed.
    if (text == lives_text && n == shown_count && !memcmp(values, shown, n * sizeof *values) &&
        !memcmp(who, shown_who, n * sizeof *who) && *(void ***)text == lives_vtable)
        return;
    shown_count = n;
    memcpy(shown, values, sizeof values);
    memcpy(shown_who, who, sizeof who);
    set_lives_from(text, 0, 0);
    if (*(DWORD *)text == VTBL_TEXT_ITEM || *(void ***)text == lives_vtable) {
        float scale = lives_scale[n];
        *(float *)(text + WIDGET_SCALE_X) = *(float *)(text + WIDGET_SCALE_Y) = scale;
    }
    if (*(DWORD *)text == VTBL_TEXT_ITEM) {
        if (!lives_vtable) {
            // Keep the slot before the vtable too: MSVC's RTTI locator, read by dynamic_cast.
            void **block = VirtualAlloc(NULL, 0x204, MEM_COMMIT, PAGE_READWRITE);
            if (!block) return;
            memcpy(block, (void **)VTBL_TEXT_ITEM - 1, 0x204);
            lives_vtable = block + 1;
            lives_vtable[VTBL_DRAW / 4] = (void *)lives_text_draw;
        }
        *(void ***)text = lives_vtable;
    }
    lives_text = text;
}

void __stdcall coop_hud_set_lives(BYTE *hud, int lives)
{
    show_lives(hud, lives);
}

static void refresh_lives(void)
{
    int *lives = game_lives(g_stage);
    if (lives) show_lives(*(BYTE **)(g_stage + STAGE_HUD), *lives);
}

// Lives the game handed out since last time go to everyone else as well; a continue (from
// below zero) refills everyone. A player who was out of lives comes back with them.
static void track_game_lives(void)
{
    int *lives = game_lives(g_stage);
    if (!lives) return;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (i != keeper && present(i) && pl[i].lives < 0) pl[i].lives = *lives > 0 ? *lives : 1;
    if (last_game_lives != -1000 && *lives > last_game_lives && competitive() && mode == MODE_HUNGER) {
        // no extra lives in Hunger Strike: the game's taken straight back
        log_printf("hunger strike: an extra life for player %d, taken back", keeper + 1);
        *lives = last_game_lives;
        refresh_lives();
    }
    if (last_game_lives != -1000 && *lives > last_game_lives) {
        int gained = *lives - last_game_lives;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (i == keeper || pl[i].lives < 0 || eliminated(i)) continue;
            int was_out = present(i) && pl[i].out && pl[i].lives <= 0;
            pl[i].lives = last_game_lives < 0 ? *lives : pl[i].lives + gained;
            if (was_out && pl[i].lives > 0) {
                pl[i].respawn_timer = RESPAWN_DELAY;
                log_printf("co-op: player %d got an extra life and comes back", i + 1);
            }
        }
        refresh_lives();
    }
    last_game_lives = *lives;
}

// --- Growth ------------------------------------------------------------------------
// The game grows the fish by one counter (GameState +0x40): meals add to it (the eater
// interface's slot 0x14, 0x42a6d0, sums up what the fish just ate), and the spawner grows the
// fish when it passes the next threshold (PlayerSpawn +0x6c: {?, threshold} per level). A
// respawn puts it back to the start of the current size (Stage slot 0x18, 0x490940): the
// growth bar drops.
// Each player now has their own counter, fed by their own meals; the game's counter holds
// the highest, so the bar shows whoever is furthest along and everyone grows as soon as one
// reaches the next size. Then nobody is behind the start of the new size. A death drops only
// that player back to the start of their size.
#define GAME_GROWTH            0x40
#define STAGE_BONUS            0x392     // bonus stage: growth works differently, left alone
#define ADDR_EAT_UPDATE_SLOT   0x552df0
#define FN_EAT_UPDATE          0x42a6d0
typedef void (__thiscall *EatUpdate_t)(BYTE *eater);

static int *growth_counter(void)
{
    if (!enabled || !g_stage || !g_spawn || g_stage[STAGE_BONUS] || !team()) return NULL;
    BYTE *game = *(BYTE **)(g_stage + STAGE_GAME);
    return game ? (int *)(game + GAME_GROWTH) : NULL;
}

// Where size `level` starts: the threshold that was passed to reach it.
static int size_start(int level)
{
    int *begin = *(int **)(g_spawn + SPAWN_THRESHOLDS), *end = *(int **)(g_spawn + SPAWN_THRESHOLDS + 4);
    if (level < 1 || !begin || level - 1 >= (int)(end - begin) / 2) return 0;
    return begin[(level - 1) * 2 + 1];
}

static int hunger_strike(void)
{
    return competitive() && mode == MODE_HUNGER;
}

// What the game's counter holds: the furthest along, so everyone grows as soon as one gets
// there; in Hunger Strike the keeper's own (everyone grows from their own there).
static int growth_shown(void)
{
    int most = 0;
    if (hunger_strike()) return pl[keeper].growth > 0 ? pl[keeper].growth : 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (present(i) && pl[i].growth > most) most = pl[i].growth;
    return most;
}

// Takes in what the game did to its counter since we last set it, then sets it again.
static void sync_growth(void)
{
    int *counter = growth_counter();
    if (!counter) {
        for (int i = 0; i < MAX_PLAYERS; i++) pl[i].growth = -1;
        growth_left = -1;
        return;
    }
    if (growth_left < 0) {
        for (int i = 0; i < MAX_PLAYERS; i++) pl[i].growth = present(i) ? *counter : -1;
    } else if (*counter < growth_left) {
        // a respawn's drop: the fish the spawner looks after
        pl[keeper].growth = *counter;
    } else if (*counter > growth_left) {
        // anything else the game handed out (golden fish, ...) goes to everyone; in Hunger
        // Strike to the keeper, whose counter it is
        for (int i = 0; i < MAX_PLAYERS; i++)
            if (present(i) && pl[i].growth < *counter && (!hunger_strike() || i == keeper)) pl[i].growth = *counter;
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!present(i)) continue;
        // in Hunger Strike everyone's at their own size, else everyone's at the keeper's
        int start = size_start(hunger_strike() && i != keeper ? pl[i].level : *(int *)(g_spawn + SPAWN_LEVEL));
        if (pl[i].growth < 0) pl[i].growth = hunger_strike() ? start : *counter; // just joined
        if (pl[i].growth < start) pl[i].growth = start;
    }
    *counter = growth_left = growth_shown();
}

void __thiscall coop_eat_update_hook(BYTE *eater)
{
    sync_growth();
    int *counter = growth_counter();
    int who = counter ? player_of(eater - EATER_IFACE) : -1;
    if (who < 0 || !present(who) || pl[who].growth < 0) {
        ((EatUpdate_t)FN_EAT_UPDATE)(eater);
        return;
    }
    *counter = pl[who].growth;
    ((EatUpdate_t)FN_EAT_UPDATE)(eater);
    pl[who].growth = *counter;
    *counter = growth_left = growth_shown();
}

// The HUD's meters (GrowMeter, Draw at vtable slot 0xa0 = 0x45faf0): a rectangle in its color
// (+0x50), filled +0xa8 of +0xa0 wide (the meter's total is +0x98), +0xa4 tall, scaled by
// +0x3c/+0x40. Two use it: the growth bar (Hud +0x174, filled with the game's counter: the
// player furthest along) and the special meter (Hud +0x180, total 1: the ability's charge of
// the fish in the spawner, PlayerFish +0x20c, set by 0x455a50). Inside each, every other
// player's own as a thin bar in their tint, on a dark line so it shows on the yellow.
#define ADDR_GROW_METER_DRAW_SLOT 0x556b1c
#define FN_GROW_METER_DRAW     0x45faf0
#define METER_TOTAL            0x98
#define METER_WIDTH            0xa0
#define METER_HEIGHT           0xa4
#define VTBL_CTX_FILL_RECT     0x24      // DrawContext: thiscall(ctx, float w, float h)
#define HUD_GROW_METER         0x174
#define HUD_SPECIAL_METER      0x180
#define PLAYER_ABILITY_CHARGE  0x20c     // float, 0..1
typedef void (__thiscall *FillRect_t)(BYTE *ctx, float w, float h);

static void meter_bar(BYTE *ctx, float *state, const float *at, float y, float w, float h, const float *color)
{
    state[0] = at[0];
    state[1] = at[1] + y * state[3];
    memcpy(state + STATE_COLOR / 4, color, 3 * sizeof(float));
    ((FillRect_t)(*(void ***)ctx)[VTBL_CTX_FILL_RECT / 4])(ctx, w, h);
}

// The growth bar itself shows player 1's own growth, though the game's counter it's set from
// holds the furthest along (so everyone grows together): only player 1's meals move it, and
// the thin bars are everyone else's. The meter's value (+0x9c, and its filled width +0xa8, as
// SetValue 0x45efd0 sets them) is swapped for the draw.
#define FN_METER_SET_VALUE     0x45efd0  // thiscall(meter, float value)
#define METER_VALUE            0x9c
#define METER_FILLED           0xa8
typedef void (__thiscall *SetValue_t)(BYTE *meter, float value);

void __thiscall coop_grow_meter_draw(BYTE *meter, BYTE *ctx)
{
    BYTE *hud = enabled && g_stage && team() ? *(BYTE **)(g_stage + STAGE_HUD) : NULL;
    int growth_bar = hud && meter == *(BYTE **)(hud + HUD_GROW_METER);
    if (growth_bar && growth_counter() && pl[0].growth >= 0) {
        float value = *(float *)(meter + METER_VALUE), filled = *(float *)(meter + METER_FILLED);
        ((SetValue_t)FN_METER_SET_VALUE)(meter, (float)pl[0].growth);
        ((Draw_t)FN_GROW_METER_DRAW)(meter, ctx);
        *(float *)(meter + METER_VALUE) = value;
        *(float *)(meter + METER_FILLED) = filled;
    } else {
        ((Draw_t)FN_GROW_METER_DRAW)(meter, ctx);
    }
    float *state = draw_state(ctx);
    float total = *(float *)(meter + METER_TOTAL);
    if (!hud || !state || total <= 0) return;
    if (!growth_bar && meter != *(BYTE **)(hud + HUD_SPECIAL_METER)) return;
    if (growth_bar && !growth_counter()) return;
    float values[MAX_PLAYERS];
    int who[MAX_PLAYERS], n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (growth_bar ? i == 0 : i == keeper) continue; // the bar itself shows that one
        if (growth_bar ? !present(i) || pl[i].growth < 0 : !swimming(i)) continue;
        values[n] = growth_bar ? (float)pl[i].growth : *(float *)(pl[i].fish + PLAYER_ABILITY_CHARGE);
        who[n++] = i;
    }
    if (!n) return;
    BYTE saved[STATE_PLAIN_SIZE];
    memcpy(saved, state, sizeof saved);
    float sx = *(float *)(meter + WIDGET_SCALE_X), sy = *(float *)(meter + WIDGET_SCALE_Y);
    float width = *(float *)(meter + METER_WIDTH) * sx, height = *(float *)(meter + METER_HEIGHT) * sy;
    // stacked down the meter, thinner the more there are; their dark lines first, under all
    float thin = n == 1 ? height * 0.2f : height * 0.7f / n - 1, at[2] = { state[0], state[1] };
    if (n > 1 && thin > height * 0.16f) thin = height * 0.16f;
    if (thin < (n == 1 ? 2 : 1)) thin = n == 1 ? 2 : 1;
    static const float dark[3] = { 0.12f, 0.08f, 0.04f };
    float w[MAX_PLAYERS], y[MAX_PLAYERS];
    for (int k = 0; k < n; k++) {
        float part = values[k] / total;
        w[k] = width * (part < 0 ? 0 : part > 1 ? 1 : part);
        y[k] = n == 1 ? height * 0.6f : height * (0.2f + 0.75f * k / n);
        if (w[k] >= 1) meter_bar(ctx, state, at, y[k] - 1, w[k] + 1, thin + 2, dark);
    }
    for (int k = 0; k < n; k++)
        if (w[k] >= 1) meter_bar(ctx, state, at, y[k], w[k], thin, pl[who[k]].tint);
    memcpy(state, saved, sizeof saved);
}

// --- A player's death, as the game shows the keeper's ---------------------------------
// PlayerDiedState (enter 0x4034b0) plays the "playerDie" music cue, a short jingle (theme
// lookup 0x489670, stdcall(theme at Stage +0x2d4, std::string *name), then 0x48d420 with esi =
// the music player at [App 0x5ac624] +0x38, edi = the track), and shows "SORRY" (0x498d20,
// stdcall(stage, std::string *string name "playerDied")). The jingle replaces the level's
// music, which the run state starts again when the keeper is back (0x48d440, the name at
// Stage +0x1f0): with the keeper still swimming, the level's music is started again here
// once the jingle is over. The text goes in the stage's bubble text (Stage +0x2e8), which is
// only shown while it's on the stage's overlay (+0x2e4; slot 0x108 adds a child, 0xf0 takes it
// off). The death state's update puts it there with 0x4990f0 (esi = stage, stdcall(0.0,
// 1.0): adds it and has its letters pop in over a second), later pops it (0x4959a0, esi =
// stage: the burst and its "bubbleTextBurst" sound); the next state takes it off (0x4a3b00).
#define FN_THEME_LOOKUP        0x489670
#define FN_MUSIC_PLAY          0x48d420
#define FN_STAGE_ANNOUNCE      0x498d20
#define ADDR_APP               0x5ac624
#define APP_MUSIC_PLAYER       0x38
#define MUSIC_PLAYER_TRACK     0x18
#define STAGE_THEME            0x2d4
#define STAGE_MUSIC_NAME       0x1f0
#define DEATH_JINGLE_TIME      3.0f      // seconds until the level's music comes back
#define STAGE_OVERLAY          0x2e4
#define STAGE_MESSAGE          0x2e8
#define VTBL_REMOVE_CHILD      0xf0
#define FN_MESSAGE_SHOW        0x4990f0
#define WIDGET_CHILDREN        0x9c      // begin, end at +0xa0
#define FN_MESSAGE_BURST       0x4959a0
#define SORRY_SHOW_TIME        1.6f      // seconds before the bubble pops
#define SORRY_POP_TIME         0.7f      // and then until it's taken off
typedef void (__thiscall *Child_t)(BYTE *container, BYTE *child);
typedef void *(__stdcall *ThemeLookup_t)(void *theme, void *name);
typedef void (__stdcall *Announce_t)(BYTE *stage, void *name);
typedef void (__thiscall *StrCtor_t)(void *, const char *);
typedef void (__thiscall *StrFree_t)(void *, int, int);
static float jingle_timer;

static void *music_player(void)
{
    BYTE *app = *(BYTE **)ADDR_APP;
    return app ? *(void **)(app + APP_MUSIC_PLAYER) : NULL;
}

static void play_music(void *track)
{
    static const DWORD fn = FN_MUSIC_PLAY;
    void *player = music_player();
    if (!player || !track) return;
    __asm__ volatile("call *%2" : : "S"(player), "D"(track), "m"(fn) : "eax", "ecx", "edx", "memory", "cc");
}

static void *theme_track(const char *name)
{
    DWORD str[8];
    ((StrCtor_t)FN_STRING_FROM_CSTR)(str, name);
    void *track = ((ThemeLookup_t)FN_THEME_LOOKUP)(*(void **)(g_stage + STAGE_THEME), str);
    ((StrFree_t)FN_STRING_FREE)(str, 1, 0);
    return track;
}

static int message_shown(void)
{
    BYTE *overlay = *(BYTE **)(g_stage + STAGE_OVERLAY), *message = *(BYTE **)(g_stage + STAGE_MESSAGE);
    BYTE **child = *(BYTE ***)(overlay + WIDGET_CHILDREN), **end = *(BYTE ***)(overlay + WIDGET_CHILDREN + 4);
    for (; child && child < end; child++)
        if (*child == message) return 1;
    return 0;
}

static void message_hide(void)
{
    BYTE *overlay = *(BYTE **)(g_stage + STAGE_OVERLAY), *message = *(BYTE **)(g_stage + STAGE_MESSAGE);
    if (overlay && message && message_shown())
        ((Child_t)(*(void ***)overlay)[VTBL_REMOVE_CHILD / 4])(overlay, message);
}

static void message_show(void)
{
    static const DWORD show = FN_MESSAGE_SHOW;
    float from = 0, span = 1;
    DWORD a, b;
    memcpy(&a, &from, 4);
    memcpy(&b, &span, 4);
    message_hide();
    __asm__ volatile("pushl %[b]\n\tpushl %[a]\n\tcall *%[fn]"
                     : : "S"(g_stage), [a] "r"(a), [b] "r"(b), [fn] "m"(show) : "eax", "ecx", "edx", "memory", "cc");
}

static void death_show(void)
{
    DWORD str[8];
    ((StrCtor_t)FN_STRING_FROM_CSTR)(str, "playerDied");
    ((Announce_t)FN_STAGE_ANNOUNCE)(g_stage, str);
    ((StrFree_t)FN_STRING_FREE)(str, 1, 0);
    message_show();
    sorry_step = 1;
    sorry_timer = SORRY_SHOW_TIME;
    death_jingle = theme_track("playerDie");
    play_music(death_jingle);
    jingle_timer = DEATH_JINGLE_TIME;
}

// The keeper is swimming: no death state of the game's is playing.
static int keeper_swimming(void)
{
    BYTE *fish = spawner_fish();
    return g_stage && fish && in_stage(g_stage, fish) && !player_dead(fish);
}

// The SORRY bubble pops, then goes, unless the keeper died meanwhile: its death state then
// shows and takes off the same bubble itself.
static void sorry_over(float dt)
{
    static const DWORD burst = FN_MESSAGE_BURST;
    if (!sorry_step || (sorry_timer -= dt) > 0) return;
    if (!keeper_swimming()) {
        sorry_step = 0;
        return;
    }
    if (sorry_step == 1) {
        __asm__ volatile("call *%1" : : "S"(g_stage), "m"(burst) : "eax", "ecx", "edx", "memory", "cc");
        sorry_step = 2;
        sorry_timer = SORRY_POP_TIME;
    } else {
        message_hide();
        sorry_step = 0;
    }
}

// Once the jingle is over: the level's music again, unless something else took over the
// music meanwhile (the keeper's own death plays the same jingle, and its run state restarts
// the music itself).
static void jingle_over(float dt)
{
    if (!death_jingle || (jingle_timer -= dt) > 0) return;
    BYTE *player = music_player();
    if (player && *(void **)(player + MUSIC_PLAYER_TRACK) == death_jingle && keeper_swimming())
        play_music(((ThemeLookup_t)FN_THEME_LOOKUP)(*(void **)(g_stage + STAGE_THEME), g_stage + STAGE_MUSIC_NAME));
    death_jingle = NULL;
}

// The mouse is player 1's: bound to player 1's fish while it's in the stage, else to none.
void __stdcall coop_bind_mouse_hook(BYTE *player)
{
    if (enabled && player && player_of(player) > 0) {
        if (!pl[0].fish || !g_stage || !in_stage(g_stage, pl[0].fish)) return;
        player = pl[0].fish;
    }
    ((BindMouse_t)FN_BIND_MOUSE)(player);
}

// A player other than the keeper died: it leaves, pays a life, and comes back after a while
// (or at the next stage, or with an extra life, when none are left).
static void player_died(int i)
{
    Player *p = &pl[i];
    ((Leave_t)FN_PLAYER_LEAVE)(p->fish);
    p->out = 1;
    death_show();
    if (growth_counter() && p->growth >= 0) {
        p->growth = size_start(p->level); // back to the start of its size, as a respawn does
        sync_growth();
    }
    if (g_spawn) ++*(int *)(g_spawn + SPAWN_DEATHS); // no "perfect" stage for this team
    // Like the game's count, a player's includes the current life: at 0 it's out.
    if (!unlimited_lives(g_stage) && p->lives > 0) p->lives--;
    p->respawn_timer = unlimited_lives(g_stage) || p->lives > 0 ? RESPAWN_DELAY : 1e30f;
    refresh_lives();
    log_printf("co-op: player %d died, %d lives left", i + 1, p->lives);
}

// PlayerFish's state machine (+0x154, vtable +0x20 = current state) is in PlayerSuckingState.
#define VTBL_PLAYER_SUCKING_STATE 0x55307c
static int player_sucking(BYTE *player)
{
    typedef BYTE *(__thiscall *Current_t)(BYTE *);
    BYTE *machine = player + PLAYER_STATES;
    BYTE *state = ((Current_t)(*(void ***)machine)[0x20 / 4])(machine);
    return state && *(DWORD *)state == VTBL_PLAYER_SUCKING_STATE;
}

// --- The players' colors (the Options screen) -----------------------------------------
// coop_p2_color .. coop_p10_color in ff2wide.ini; players 2 to 4's are each cycled through
// these presets by a button the patch adds to the Options screen (options.xml,
// coop_options_action_hook). Any other color set in the ini shows as "Custom".
static const struct { const char *name; BYTE r, g, b; } colors[] = {
    { "Orange", 255, 140, 90 },  { "Red", 255, 85, 85 },    { "Pink", 255, 120, 210 },
    { "Purple", 185, 120, 255 }, { "Blue", 100, 150, 255 }, { "Cyan", 80, 220, 255 },
    { "Green", 110, 240, 110 },  { "Yellow", 255, 225, 80 }, { "Gray", 150, 150, 150 },
};
#define COLOR_COUNT ((int)(sizeof colors / sizeof *colors))
// orange, green, blue, then pink, yellow, purple, cyan, red, gray
static const int default_colors[MAX_PLAYERS] = { -1, 0, 6, 4, 2, 7, 3, 5, 1, 8 };

static void set_tint(int i, int r, int g, int b)
{
    pl[i].tint[0] = r / 255.0f;
    pl[i].tint[1] = g / 255.0f;
    pl[i].tint[2] = b / 255.0f;
    pl[i].tint[3] = 1;
}

static void ini_write(const char *key, const char *value)
{
    char path[MAX_PATH], *slash = path;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    for (char *p = path; *p; p++)
        if (*p == '\\') slash = p + 1;
    lstrcpyA(slash, "ff2wide.ini");
    WritePrivateProfileStringA("ff2wide", key, value, path);
}

static int color_index(int player)
{
    const float *tint = pl[player].tint;
    for (int i = 0; i < COLOR_COUNT; i++)
        if (fabsf(colors[i].r / 255.0f - tint[0]) < 0.01f && fabsf(colors[i].g / 255.0f - tint[1]) < 0.01f &&
            fabsf(colors[i].b / 255.0f - tint[2]) < 0.01f)
            return i;
    return -1;
}

const char *coop_color_name(int player)
{
    if (player < 2 || player > MAX_PLAYERS) return "";
    int i = color_index(player - 1);
    return i >= 0 ? colors[i].name : "Custom";
}

// The color buttons' actions ("p2Color" .. "p4Color"). Buttons report to the popup's state
// (PopupState vtable slot 0x55a910, 0x414e50: "optionsDone", "pauseResume", ...), checkboxes
// to the options screen (GenericOptionScreen slot 0x55ca48, 0x520768); both thiscall(this,
// const std::string *action). Both are watched for these; the rest goes on as before.
#define ADDR_OPTIONS_ACTION_SLOT 0x55ca48
#define FN_OPTIONS_ACTION      0x520768
#define ADDR_POPUP_ACTION_SLOT 0x55a910
#define FN_POPUP_ACTION        0x414e50
typedef void (__thiscall *OptionsAction_t)(BYTE *screen, BYTE *action);

static const char *std_string(const BYTE *str)
{
    return *(const DWORD *)(str + 0x18) < 0x10 ? (const char *)(str + 4) : *(const char *const *)(str + 4);
}

static int color_action(BYTE *action)
{
    const char *a = action ? std_string(action) : "";
    int player;
    char rest[8];
    if (sscanf(a, "p%dColo%7s", &player, rest) != 2 || strcmp(rest, "r") || player < 2 || player > MAX_PLAYERS)
        return 0;
    int i = (color_index(player - 1) + 1) % COLOR_COUNT;
    set_tint(player - 1, colors[i].r, colors[i].g, colors[i].b);
    char key[24], value[16];
    sprintf(key, "coop_p%d_color", player);
    sprintf(value, "%d,%d,%d", colors[i].r, colors[i].g, colors[i].b);
    ini_write(key, value);
    log_printf("co-op: player %d color: %s", player, colors[i].name);
    return 1;
}

// --- Modes --------------------------------------------------------------------------------
// The Options screen's Co-op button (options.xml, in the Co-op checkbox's place) cycles Off,
// Co-op (together) and the competitive modes; coop and coop_mode in ff2wide.ini. A mode takes
// effect when the next level starts, like co-op itself; Off at once (coop_request).
static const char *const mode_keys[MODE_COUNT] = { "together", "hunger" };
static const char *const mode_names[MODE_COUNT] = { "Co-op", "Hunger Strike" };

const char *coop_mode_name(void)
{
    return requested ? mode_names[mode_requested] : "Off";
}

static int mode_action(BYTE *action)
{
    if (!action || strcmp(std_string(action), "coopMode")) return 0;
    if (!requested) {
        mode_requested = MODE_TOGETHER;
        coop_request(1);
    } else if (mode_requested + 1 < MODE_COUNT) {
        mode_requested++;
    } else {
        mode_requested = MODE_TOGETHER;
        coop_request(0);
    }
    ini_write("coop_mode", mode_keys[mode_requested]);
    log_printf("mode: %s from the next level", coop_mode_name());
    return 1;
}

void __thiscall coop_options_action_hook(BYTE *screen, BYTE *action)
{
    if (!color_action(action) && !mode_action(action)) ((OptionsAction_t)FN_OPTIONS_ACTION)(screen, action);
}

void __thiscall coop_popup_action_hook(BYTE *state, BYTE *action)
{
    if (!color_action(action) && !mode_action(action)) ((OptionsAction_t)FN_POPUP_ACTION)(state, action);
}

// A competitive round's winner: "Player 2 wins!" across the screen in their color for a few
// seconds (drawn into the frame like the online "You!" label, coop_draw_banner).
#define BANNER_TIME            5.0f
#define BANNER_TEXT_PX         64
static float banner_timer;
static Label banner;
static int banner_for = -1;

static void announce_winner(int i)
{
    winner = i;
    banner_timer = BANNER_TIME;
    log_printf("%s: player %d wins", mode_names[mode], i + 1);
}

// A player's color as 0..255 RGB: player 1 in its score's gold, the others in their tints.
static void player_rgb(int i, int *rgb)
{
    static const int gold[3] = { 255, 230, 131 };
    for (int c = 0; c < 3; c++) rgb[c] = i == 0 ? gold[c] : (int)(pl[i].tint[c] * 255);
}

// A player out of lives waits for the next level or an extra life: "Player 2 is waiting on
// lives..." in their color under the HUD while they do, so it's clear why their fish isn't
// coming back.
#define WAITING_TEXT_PX        22
#define WAITING_TOP            150       // the first line's bottom, back-buffer pixels
#define WAITING_LINE           28
static Label waiting_label[MAX_PLAYERS];
static int waiting_rgb[MAX_PLAYERS][3];
static char waiting_said[MAX_PLAYERS][48];

static int waiting_on_lives(int i)
{
    return enabled && g_stage && now_playing && team() && present(i) && pl[i].out && i != keeper &&
           pl[i].lives <= 0 && !unlimited_lives(g_stage);
}

// Its line: waiting, or in a competitive round, out for good.
static void waiting_text(int i, char *out)
{
    sprintf(out, competitive() ? "Player %d is out!" : "Player %d is waiting on lives...", i + 1);
}

void coop_draw_overlays(void **back_surface, int width, int height)
{
    int show_banner = winner >= 0 && banner_timer > 0, waiting = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) waiting += waiting_on_lives(i);
    if ((!show_banner && !waiting) || !back_surface) return;
    if (show_banner && banner_for != winner) {
        char text[32];
        int rgb[3];
        player_rgb(winner, rgb);
        free(banner.bgra);
        sprintf(text, "Player %d wins!", winner + 1);
        label_make_text(&banner, text, BANNER_TEXT_PX, rgb[0], rgb[1], rgb[2]);
        banner_for = winner;
    }
    int pitch;
    BYTE *bits = ambient_lock(back_surface, &pitch);
    if (!bits) return;
    for (int i = 0, line = 0; i < MAX_PLAYERS; i++) {
        if (!waiting_on_lives(i)) continue;
        int rgb[3];
        player_rgb(i, rgb);
        char text[48];
        waiting_text(i, text);
        if (!waiting_label[i].bgra || memcmp(rgb, waiting_rgb[i], sizeof rgb) || strcmp(text, waiting_said[i])) {
            free(waiting_label[i].bgra);
            label_make_text(&waiting_label[i], text, WAITING_TEXT_PX, rgb[0], rgb[1], rgb[2]);
            memcpy(waiting_rgb[i], rgb, sizeof rgb);
            strcpy(waiting_said[i], text);
        }
        label_blit(&waiting_label[i], bits, pitch, width, height, width / 2, WAITING_TOP + WAITING_LINE * line++);
    }
    if (show_banner) label_blit(&banner, bits, pitch, width, height, width / 2, height * 2 / 5 + banner.h / 2);
    ambient_unlock(back_surface);
}

// --- Every mission open in the competitive modes, the save untouched ----------------------
// The level map opens missions up to the save's furthest stage: the current profile
// ([App +0xb4]) keeps a record per game mode (+0x40, 0x20 each, two of them), its furthest
// stage at +0x18 (0x478d90 reads it so, clamped to the last one). While a competitive mode is
// chosen, every record's is set to the last mission, the real records kept here; switched
// back, they're put back. The profile save (0x4819c0) writes each record through one call
// (0x481b9e, stdcall 0x473570(record)), which is wrapped: the real record is swapped in for
// the write and back out after, so the save only ever gets the real progress.
#define APP_PROFILE            0xb4
#define PROFILE_RECORDS        0x40
#define RECORD_SIZE            0x20
#define RECORD_COUNT           2
#define RECORD_MAX_STAGE       0x18
#define ALL_STAGES             59
#define ADDR_SAVE_RECORD_CALL  0x481b9e
#define FN_SAVE_RECORD         0x473570
static BYTE *opened_profile;
static BYTE real_records[RECORD_COUNT * RECORD_SIZE];

static int competitive_requested(void)
{
    return requested && mode_requested != MODE_TOGETHER;
}

static void open_missions(void)
{
    BYTE *app = *(BYTE **)ADDR_APP, *profile = app ? *(BYTE **)(app + APP_PROFILE) : NULL;
    if (opened_profile && (opened_profile != profile || !competitive_requested())) {
        if (opened_profile == profile) memcpy(profile + PROFILE_RECORDS, real_records, sizeof real_records);
        log_printf("missions: back to the save's");
        opened_profile = NULL;
    }
    if (!opened_profile && profile && competitive_requested()) {
        memcpy(real_records, profile + PROFILE_RECORDS, sizeof real_records);
        for (int r = 0; r < RECORD_COUNT; r++)
            *(int *)(profile + PROFILE_RECORDS + r * RECORD_SIZE + RECORD_MAX_STAGE) = ALL_STAGES;
        opened_profile = profile;
        log_printf("missions: all open for the competitive modes");
    }
}

// Exchanges a record with its real copy while missions are opened (twice puts it back).
void __cdecl coop_record_swap(BYTE *record)
{
    if (!opened_profile) return;
    int r = (int)(record - (opened_profile + PROFILE_RECORDS));
    if (r < 0 || r % RECORD_SIZE || r / RECORD_SIZE >= RECORD_COUNT) return;
    BYTE live[RECORD_SIZE];
    memcpy(live, record, RECORD_SIZE);
    memcpy(record, real_records + r, RECORD_SIZE);
    memcpy(real_records + r, live, RECORD_SIZE);
}

void coop_save_record_thunk(void);
__asm__(
    ".globl _coop_save_record_thunk\n"
    "_coop_save_record_thunk:\n"
    "    pushal\n"
    "    pushl 36(%esp)\n"          // the record: the real one in
    "    call _coop_record_swap\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    pushl 4(%esp)\n"           // written (stdcall, takes its argument)
    "    movl $0x473570, %eax\n"
    "    call *%eax\n"
    "    pushal\n"
    "    pushl 36(%esp)\n"          // and the live one back
    "    call _coop_record_swap\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    ret $4\n");

// --- Joining and controls ---------------------------------------------------------------
// During play, a device nobody has yet that presses one of its buttons joins as the next
// free player: a gamepad (A, B, X, Y or Start), a keyboard's left keys (W A S D, Space to
// dash, E or Left Shift for the special) or its right keys (arrows, Right Ctrl to dash, Enter
// or Right Shift for the special), a mouse of its own (a click; mice.c), up to LOCAL_PLAYERS
// on this PC; or an online guest (as it connects). It keeps that player for the session.
#define PAD_JOIN_BUTTONS       0xf010    // A B X Y, Start
#define PAD_RETRY_MS           2000      // an empty gamepad slot is asked again after this
static const int left_keys[] = { 'W', 'A', 'S', 'D', VK_SPACE, 'E', VK_LSHIFT };
static const int right_keys[] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RCONTROL, VK_RETURN, VK_RSHIFT };
static WORD pad_held[4];
static DWORD pad_retry[4];               // XInput on an empty slot is slow: asked now and then

static int read_pad(int i, PadState *pad)
{
    struct { DWORD packet; PadState pad; } state;
    if (!xinput_get_state || GetTickCount() - pad_retry[i] < PAD_RETRY_MS) return 0;
    if (xinput_get_state(i, &state) != ERROR_SUCCESS) {
        pad_retry[i] = GetTickCount();
        return 0;
    }
    pad_retry[i] = GetTickCount() - PAD_RETRY_MS;
    *pad = state.pad;
    return 1;
}

static void device_name(int device, char *out)
{
    int i = DEV_INDEX(device);
    switch (DEV_KIND(device)) {
    case DEV_PAD: sprintf(out, "gamepad %d", i + 1); break;
    case DEV_KEYS: sprintf(out, "keyboard %d's %s keys", i / 2 + 1, i % 2 ? "arrow" : "WASD"); break;
    case DEV_MOUSE: sprintf(out, "a mouse of its own"); break;
    case DEV_GUEST: sprintf(out, "online guest %d's controls", i + 1); break;
    default: sprintf(out, "nothing");
    }
}

static int free_player(void)
{
    for (int i = 1; i < MAX_PLAYERS; i++)
        if (!pl[i].device) return i;
    return -1;
}

// Players on this PC's own devices, player 1 included.
static int local_players(void)
{
    int n = 1;
    for (int i = 1; i < MAX_PLAYERS; i++) n += pl[i].device && DEV_KIND(pl[i].device) != DEV_GUEST;
    return n;
}

static void join(int device)
{
    for (int i = 1; i < MAX_PLAYERS; i++)
        if (pl[i].device == device) return;
    int i = free_player();
    if (i < 0 || (DEV_KIND(device) != DEV_GUEST && local_players() >= LOCAL_PLAYERS)) return;
    pl[i].device = device;
    char name[48];
    device_name(device, name);
    log_printf("co-op: player %d joins with %s", i + 1, name);
}

static int any_key(int keyboard, const int *keys, int count)
{
    for (int i = 0; i < count; i++)
        if (keys_held(keyboard, keys[i])) return 1;
    return 0;
}

static void find_joiners(void)
{
    for (int i = 0; i < 4; i++) {
        PadState pad;
        WORD held = read_pad(i, &pad) ? pad.buttons : 0;
        if (held & ~pad_held[i] & PAD_JOIN_BUTTONS) join(DEV_PAD | i);
        pad_held[i] = held;
    }
    float gx, gy;
    int gb;
    for (int g = 0; g < ONLINE_MAX_GUESTS; g++)
        if (online_guest(g, &gx, &gy, &gb)) join(DEV_GUEST | g);
    if (free_player() < 0 || local_players() >= LOCAL_PLAYERS) return;
    for (int k = 0; k < keys_keyboards(); k++) {
        if (any_key(k, left_keys, sizeof left_keys / sizeof *left_keys)) join(DEV_KEYS | k * 2);
        if (any_key(k, right_keys, sizeof right_keys / sizeof *right_keys)) join(DEV_KEYS | (k * 2 + 1));
    }
    int mouse = mice_new_click();
    if (mouse >= 0) join(DEV_MOUSE | mouse);
}

// A player's controls this frame: stick / key direction, its buttons, and mouse movement.
static void read_device(int device, float *x, float *y, int *dash, int *special, int *mouse_dx, int *mouse_dy)
{
    *x = *y = 0;
    *dash = *special = 0;
    *mouse_dx = *mouse_dy = 0;
    int i = DEV_INDEX(device);
    switch (DEV_KIND(device)) {
    case DEV_PAD: {
        PadState pad;
        if (!read_pad(i, &pad)) break;
        float lx = pad.lx / 32767.0f, ly = -pad.ly / 32767.0f;
        float len = sqrtf(lx * lx + ly * ly);
        const float dead = 0.24f;
        if (len > dead) {
            float s = (len > 1 ? 1 : len - dead) / (1 - dead) / len;
            *x = lx * s;
            *y = ly * s;
        }
        if (pad.buttons & 0x1000) *dash = 1;    // A
        if (pad.buttons & 0x2000) *special = 1; // B
        break;
    }
    case DEV_KEYS: {
        // No Shift for the dash: pressing it 5 times brings up Windows' Sticky Keys prompt,
        // which pauses the game. Left Shift for the special is fine: it's held, not tapped.
        int k = i / 2;
        if (i % 2 == 0) {
            if (keys_held(k, 'A')) *x -= 1;
            if (keys_held(k, 'D')) *x += 1;
            if (keys_held(k, 'W')) *y -= 1;
            if (keys_held(k, 'S')) *y += 1;
            if (keys_held(k, VK_SPACE)) *dash = 1;
            if (keys_held(k, 'E') || keys_held(k, VK_LSHIFT)) *special = 1;
        } else {
            if (keys_held(k, VK_LEFT)) *x -= 1;
            if (keys_held(k, VK_RIGHT)) *x += 1;
            if (keys_held(k, VK_UP)) *y -= 1;
            if (keys_held(k, VK_DOWN)) *y += 1;
            if (keys_held(k, VK_RCONTROL)) *dash = 1;
            if (keys_held(k, VK_RETURN) || keys_held(k, VK_RSHIFT)) *special = 1;
        }
        break;
    }
    case DEV_MOUSE: {
        int buttons = 0;
        if (!mice_take(i, mouse_dx, mouse_dy, &buttons)) break;
        if (buttons & MICE_LEFT) *dash = 1;
        if (buttons & MICE_RIGHT) *special = 1;
        break;
    }
    case DEV_GUEST: {
        // An online guest (online.c) steers with the same controls on its own PC.
        int buttons;
        if (!online_guest(i, x, y, &buttons)) break;
        if (buttons & NET_DASH) *dash = 1;
        if (buttons & NET_SPECIAL) *special = 1;
        online_guest_mouse(i, mouse_dx, mouse_dy);
        break;
    }
    }
    float len = sqrtf(*x * *x + *y * *y);
    if (len > 1) *x /= len, *y /= len;
}

// A player's controls handed to its fish: movement as the mouse's is, and the buttons.
static void steer_player(int i, float dt)
{
    Player *p = &pl[i];
    float x, y;
    int dash, special, mouse_dx, mouse_dy;
    read_device(p->device, &x, &y, &dash, &special, &mouse_dx, &mouse_dy);
    p->in_x = x;
    p->in_y = y;
    p->carry_x += x * pad_speed * dt + mouse_dx * mouse_get_sensitivity();
    p->carry_y += y * pad_speed * dt + mouse_dy * mouse_get_sensitivity();
    int dx = (int)p->carry_x, dy = (int)p->carry_y;
    p->carry_x -= dx;
    p->carry_y -= dy;
    *(int *)(p->fish + PLAYER_MOVE_X) += dx;
    *(int *)(p->fish + PLAYER_MOVE_Y) += dy;

    if (dash && !p->dash_down) ((Button_t)FN_PLAYER_BUTTON_DOWN)(p->fish, 0);
    if (special != p->special_down)
        ((Button_t)(special ? FN_PLAYER_BUTTON_DOWN : FN_PLAYER_BUTTON_UP))(p->fish, 1);
    else if (special && !player_sucking(p->fish))
        // Held: the suck starts once the ability is ready. The game does this in the fish's
        // update only for the fish the mouse is bound to (+0x194), never the others.
        ((Button_t)FN_PLAYER_BUTTON_DOWN)(p->fish, 1);
    p->dash_down = dash;
    p->special_down = special;
}

// Play is on (not menus, panels, between stages; now_playing): the "You!" labels show only then.

// Just above a fish, in the play layer's coordinates. which: the player's number (1 = player
// 1, this PC's). The fish grows with its level, so the label sits higher.
#define VTBL_PLAYER_GET_POSITION 0x54
#define LABEL_GAP              26.0f
#define LABEL_GAP_PER_LEVEL    18.0f
int coop_label_spot(int which, float *spot)
{
    int i = which - 1;
    if (!enabled || i < 0 || i >= MAX_PLAYERS || !g_spawn || !g_stage || !now_playing || !team() || !swimming(i))
        return 0;
    typedef float *(__thiscall *GetPos_t)(BYTE *, float *);
    float buf[4];
    float *pos = ((GetPos_t)(*(void ***)pl[i].fish)[VTBL_PLAYER_GET_POSITION / 4])(pl[i].fish, buf);
    int level = i == keeper ? *(int *)(g_spawn + SPAWN_LEVEL) : pl[i].level;
    spot[0] = pos[0];
    spot[1] = pos[1] - (LABEL_GAP + LABEL_GAP_PER_LEVEL * level);
    return 1;
}

int coop_guest_player(int guest)
{
    for (int i = 1; i < MAX_PLAYERS; i++)
        if (pl[i].device == (DEV_GUEST | guest)) return i + 1;
    return 0;
}

// --- Taking players out mid-level (co-op or online play switched off) -------------------
#define TAKE_OUT_ALL           1         // co-op off: everyone but player 1
#define TAKE_OUT_GUESTS        2         // online play off: the guests
static int take_out_pending;

static int taken_out(int i, int all)
{
    return i > 0 && pl[i].device && (all || DEV_KIND(pl[i].device) == DEV_GUEST);
}

// The keeper is being taken out: player 1 takes the spawner back (its fish entered again if
// it's out, with at least one life), and the game's count is player 1's again. Not while the
// spawner is busy with the keeper's death or respawn: then 0, to try again next frame.
static int give_back_to_player1(void)
{
    if (!pl[0].fish || !g_spawn[SPAWN_DONE]) return 0;
    if (!swimming(0)) {
        if (pl[0].lives <= 0) pl[0].lives = 1;
        activate_player(0);
    }
    int *lives = game_lives(g_stage);
    if (lives) {
        pl[keeper].lives = *lives;
        *lives = pl[0].lives > 0 ? pl[0].lives : 1;
        last_game_lives = *lives;
    }
    set_spawner_fish(pl[0].fish); // our references on the two swap with the spawner's
    log_printf("co-op: player 1 takes over from player %d", keeper + 1);
    keeper = 0;
    return 1;
}

static void take_out_players(void)
{
    if (!take_out_pending) return;
    int all = take_out_pending & TAKE_OUT_ALL;
    if (g_stage && g_spawn && keeper != 0 && taken_out(keeper, all) && !give_back_to_player1()) return;
    for (int i = 1; i < MAX_PLAYERS; i++) {
        if (!taken_out(i, all)) continue;
        if (g_stage && present(i)) {
            if (!pl[i].out && in_stage(g_stage, pl[i].fish)) ((Leave_t)FN_PLAYER_LEAVE)(pl[i].fish);
            log_printf("co-op: player %d taken out (%s switched off)", i + 1, all ? "co-op" : "online play");
        }
        pl[i].entered = pl[i].out = 0;
        pl[i].growth = -1;
        if (DEV_KIND(pl[i].device) == DEV_GUEST) pl[i].device = DEV_NONE; // their place is free again
    }
    take_out_pending = 0;
    if (all) apply_requested();
    refresh_lives();
}

void coop_frame(BYTE *engine)
{
    static LARGE_INTEGER freq, last;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    float dt = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / freq.QuadPart : 0;
    last = now;
    if (dt > 0.1f) dt = 0.1f;

    for (int i = 0; i < MAX_PLAYERS; i++) pl[i].in_x = pl[i].in_y = 0;
    BYTE *relative = *(BYTE **)(engine + ENGINE_RELATIVE_MOUSE);
    // In play (not menus, panels, between stages), or the keeper's death is playing while
    // others swim on.
    int playing = (relative && relative[0x50]) || GetTickCount() - keeper_dying_tick < 200;
    take_out_players();
    open_missions();
    if (banner_timer > 0) banner_timer -= dt;
    mice_set_play(enabled && g_stage && playing);
    now_playing = playing;
    if (!enabled || !g_stage || !g_spawn) return;
    track_game_lives();
    sync_growth();
    jingle_over(dt);
    sorry_over(dt);
    if (!playing) return;
    find_joiners();

    // With other players' mice, player 1's clicks come from its own mouse (the shared ones are
    // held back, mice.c).
    if (mice_shared_out()) {
        int b = mice_buttons(MICE_PLAYER1), was = pl[0].mouse_held;
        if (swimming(0)) {
            if ((b & MICE_LEFT) && !(was & MICE_LEFT)) ((Button_t)FN_PLAYER_BUTTON_DOWN)(pl[0].fish, 0);
            if ((b & MICE_RIGHT) != (was & MICE_RIGHT))
                ((Button_t)((b & MICE_RIGHT) ? FN_PLAYER_BUTTON_DOWN : FN_PLAYER_BUTTON_UP))(pl[0].fish, 1);
        }
        pl[0].mouse_held = b;
    }

    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &pl[i];
        if (!p->fish) continue;
        if (!p->entered) {
            // Just joined: in once the spawner isn't busy with the keeper's respawn.
            if (i > 0 && p->device && g_spawn[SPAWN_DONE]) {
                activate_player(i);
                refresh_lives();
                log_printf("co-op: player %d swims in, stage has %d players", i + 1, player_count(g_stage));
            }
            continue;
        }
        if (i != keeper) {
            // Everyone but the keeper: deaths and respawns are ours.
            if (p->out) {
                if ((p->respawn_timer -= dt) <= 0 && g_spawn[SPAWN_DONE]) {
                    activate_player(i);
                    log_printf("co-op: player %d respawned, stage has %d players", i + 1, player_count(g_stage));
                }
                continue;
            }
            if (!in_stage(g_stage, p->fish)) {
                log_printf("co-op: player %d left the stage", i + 1);
                p->out = 1;
                p->respawn_timer = 1e30f;
                continue;
            }
            if (player_dead(p->fish)) {
                player_died(i);
                continue;
            }
        }
        if (i > 0 && in_stage(g_stage, p->fish) && !player_dead(p->fish)) steer_player(i, dt);
    }
}

// --- The others play on while the keeper's death plays --------------------------------
// The stage's RunState update (0x43acb0) runs the stage's per-frame steps (Stage vtable
// +0x48 .. +0x6c); PlayerDiedState's (0x434cb0, slot 0x558790, thiscall(state, update))
// leaves out spawning (+0x4c), collisions and eating (+0x50), the players' own updates
// (+0x54) and the camera (+0x60): the level holds still for the death. With others still
// swimming, those run too, and their controls stay live (coop_frame).
#define ADDR_DIED_STATE_UPDATE_SLOT 0x558790
#define FN_DIED_STATE_UPDATE   0x434cb0
#define STATE_STAGE            0x0c
static const int run_steps[] = { 0x4c, 0x50, 0x54, 0x60 };
typedef void (__thiscall *StateUpdate_t)(BYTE *state, BYTE *update);
typedef void (__thiscall *StageStep_t)(BYTE *stage, BYTE *update);

// Meanwhile the death state swings the camera to where the keeper comes back: the middle of
// the play area below the water line (where PlayerFish::EnterStage, 0x406de0, puts a fish,
// from 0x44c6f0). With others swimming, the co-op camera keeps them in view instead and
// frames that spot along with them (coop_player_positions), zooming out if they're far
// apart; the keeper still comes back there.
#define STAGE_PLAY_AREA        0x88      // float x0, y0, x1, y1, play layer coordinates
#define STAGE_LAYER_SET        0x2d8
#define LAYERSET_WATER_LEVEL   0xdc      // int, the water line (0 without a surface)
static int respawn_framing;
static float respawn_point[2];

void __thiscall coop_died_state_update_hook(BYTE *state, BYTE *update)
{
    ((StateUpdate_t)FN_DIED_STATE_UPDATE)(state, update);
    BYTE *stage = *(BYTE **)(state + STATE_STAGE);
    if (!enabled || !stage || stage != g_stage || !team()) return;
    int others = 0;
    for (int j = 0; j < MAX_PLAYERS; j++)
        if (j != keeper && swimming(j)) others++;
    if (!others) return;
    float *area = (float *)(stage + STAGE_PLAY_AREA);
    BYTE *layer_set = *(BYTE **)(stage + STAGE_LAYER_SET);
    float top = layer_set ? (float)*(int *)(layer_set + LAYERSET_WATER_LEVEL) : area[1];
    respawn_point[0] = (area[0] + area[2]) / 2;
    respawn_point[1] = (top + area[3]) / 2;
    respawn_framing = 1;
    for (int i = 0; i < (int)(sizeof run_steps / sizeof *run_steps); i++)
        ((StageStep_t)(*(void ***)stage)[run_steps[i] / 4])(stage, update);
    respawn_framing = 0;
    keeper_dying_tick = GetTickCount();
}

int coop_respawn_framing(void)
{
    return respawn_framing;
}

// --- AI fish react to the nearest player --------------------------------------------
// Stage::GetPlayer (0x4916a0, eax = stage, returns the first fish in the player list) is
// what AIFish::Decide (0x4288b0, vtable +0x110 of the fish classes below, thiscall) uses to
// pick whom to chase or avoid. While a fish decides, GetPlayer returns the live player
// nearest to it instead, so big fish hunt everyone and small fish notice everyone.
#define FN_STAGE_GET_PLAYER    0x4916a0
#define FN_AIFISH_DECIDE       0x4288b0
#define VTBL_ACTOR_GET_POSITION 0x54
static const DWORD ai_decide_slots[] = {
    0x54e6cc + 0x110, // AIFish
    0x54f4d4 + 0x110, // Butterfly
    0x54f72c + 0x110, // CuttleFish
    0x55117c + 0x110, // GlowFish
    0x551354 + 0x110, // GoldenFish
    0x553464 + 0x110, // PoisonFish
};
typedef void (__thiscall *Decide_t)(BYTE *fish);
typedef float *(__thiscall *ActorPos_t)(BYTE *, float *);
static BYTE *deciding_fish;

void __thiscall coop_ai_decide_hook(BYTE *fish)
{
    BYTE *outer = deciding_fish;
    deciding_fish = fish;
    ((Decide_t)FN_AIFISH_DECIDE)(fish);
    deciding_fish = outer;
}

BYTE *__cdecl coop_get_player(BYTE *stage)
{
    BYTE **begin = *(BYTE ***)(stage + STAGE_PLAYERS_BEGIN), **end = *(BYTE ***)(stage + STAGE_PLAYERS_END);
    if (!begin || end <= begin) return NULL;
    if (!enabled || !deciding_fish || end - begin < 2) return begin[0];
    float buf[4], *pos = ((ActorPos_t)(*(void ***)deciding_fish)[VTBL_ACTOR_GET_POSITION / 4])(deciding_fish, buf);
    float fx = pos[0], fy = pos[1], best_d = 0;
    BYTE *best = NULL;
    for (BYTE **p = begin; p < end; p++) {
        if (!*p || player_dead(*p)) continue;
        pos = ((ActorPos_t)(*(void ***)*p)[VTBL_ACTOR_GET_POSITION / 4])(*p, buf);
        float d = (pos[0] - fx) * (pos[0] - fx) + (pos[1] - fy) * (pos[1] - fy);
        if (!best || d < best_d) best = *p, best_d = d;
    }
    return best ? best : begin[0];
}

void coop_get_player_thunk(void);
__asm__(
    ".globl _coop_get_player_thunk\n"
    "_coop_get_player_thunk:\n"
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    pushl %eax\n"              // stage
    "    call _coop_get_player\n"
    "    addl $4, %esp\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    ret\n");

// The positions in the level of everyone in play (PlayerFish vtable +0x54, what the camera
// follows), and while the keeper's death plays, the spot it comes back at instead of it; how
// many, when that's two or more, else 0. For the co-op camera.
int coop_player_positions(float (*pos)[2], int max)
{
    if (!enabled || !g_spawn || !g_stage || !team()) return 0;
    typedef float *(__thiscall *GetPos_t)(BYTE *, float *);
    int n = 0;
    if (respawn_framing && n < max) { // the keeper is about to come back (coop_died_state_update_hook)
        pos[n][0] = respawn_point[0];
        pos[n++][1] = respawn_point[1];
    }
    for (int i = 0; i < MAX_PLAYERS && n < max; i++) {
        if ((respawn_framing && i == keeper) || !swimming(i)) continue;
        float buf[4];
        float *p = ((GetPos_t)(*(void ***)pl[i].fish)[VTBL_PLAYER_GET_POSITION / 4])(pl[i].fish, buf);
        pos[n][0] = p[0];
        pos[n++][1] = p[1];
    }
    return n >= 2 ? n : 0;
}

// Switching co-op on (the options screen's checkbox, scene.c) takes effect when the next
// level starts (the spawn hook): the other players' fish are made then. Switching it off
// takes the other fish out at once (in the next frame, take_out_players), and so does online
// play for the guests' fish. The hooks stay installed either way and do nothing while co-op
// is off; only the Space-pauses check is swapped.
static int (*patch_u32_fn)(DWORD, DWORD, DWORD);

static void apply_requested(void)
{
    if (enabled == requested) return;
    enabled = requested;
    if (enabled) patch_u32_fn(ADDR_PAUSE_SPACE_CMP, PAUSE_SPACE_CMP, PAUSE_ESC_CMP);
    else patch_u32_fn(ADDR_PAUSE_SPACE_CMP, PAUSE_ESC_CMP, PAUSE_SPACE_CMP);
    log_printf("co-op %s", enabled ? "on" : "off");
}

int coop_requested(void)
{
    return requested;
}

void coop_request(int on)
{
    requested = on != 0;
    ini_write("coop", requested ? "1" : "0");
    if (!requested && enabled) take_out_pending |= TAKE_OUT_ALL;
    if (requested) take_out_pending &= ~TAKE_OUT_ALL; // on again before it happened
    log_printf(requested ? "co-op on from the next level" : "co-op off");
}

void coop_online_closed(void)
{
    take_out_pending |= TAKE_OUT_GUESTS;
}

// --- Whose score is whose ---------------------------------------------------------------
// Points eaten show as a "+N" splat (0x427250, thiscall(const float *where, Hud*, int points,
// int), called at the four sites below), next to whoever ate: each splat counts toward the
// nearest player's own total, and the other players' are tinted their color. The splat is
// made by ScoreSplat's constructor (0x47a3c0, stdcall(memory), called at two sites inside).
#define FN_SCORE_SPLAT         0x427250
#define FN_SCORE_SPLAT_NEW     0x47a3c0
static const DWORD splat_calls[] = { 0x44c69e, 0x53fbe6, 0x53fd90, 0x5403ac };
static const DWORD splat_new_calls[] = { 0x427291, 0x4f0a8e };
typedef void (__thiscall *ScoreSplat_t)(const float *where, BYTE *hud, int points, int extra);
typedef BYTE *(__stdcall *ScoreSplatNew_t)(BYTE *memory);
static BYTE *new_splat;
extern void __cdecl layer_to_overlay(BYTE *stage, int layer, float x, float y, float *out);

BYTE *__stdcall coop_score_splat_new_hook(BYTE *memory)
{
    return new_splat = ((ScoreSplatNew_t)FN_SCORE_SPLAT_NEW)(memory);
}

// The player nearest the splat, measured both in the play layer and on the overlay (where
// splats are placed), or -1 out of co-op.
static int splat_player(const float *where)
{
    if (!enabled || !g_spawn || !g_stage || !where || !team()) return -1;
    int best = -1;
    float best_d = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        BYTE *p = pl[i].fish;
        if (!present(i) || !in_stage(g_stage, p)) continue;
        float buf[4], over[2];
        float *pos = ((ActorPos_t)(*(void ***)p)[VTBL_ACTOR_GET_POSITION / 4])(p, buf);
        layer_to_overlay(g_stage, 0, pos[0], pos[1], over);
        float d1 = (pos[0] - where[0]) * (pos[0] - where[0]) + (pos[1] - where[1]) * (pos[1] - where[1]);
        float d2 = (over[0] - where[0]) * (over[0] - where[0]) + (over[1] - where[1]) * (over[1] - where[1]);
        float d = d1 < d2 ? d1 : d2;
        if (best < 0 || d < best_d) best = i, best_d = d;
    }
    return best;
}

void __thiscall coop_score_splat_hook(const float *where, BYTE *hud, int points, int extra)
{
    int who = splat_player(where);
    new_splat = NULL;
    ((ScoreSplat_t)FN_SCORE_SPLAT)(where, hud, points, extra);
    if (who >= 0) {
        pl[who].score += points;
        if (who > 0 && points > 0 && new_splat)
            for (int i = 0; i < 3; i++) ((float *)(new_splat + VISUAL_COLOR))[i] *= pl[who].tint[i];
    }
    new_splat = NULL;
}

static void format_points(char *out, const char *prefix, int n)
{
    char digits[16];
    sprintf(digits, "%d", n < 0 ? -n : n);
    int len = (int)strlen(digits), o = sprintf(out, "%s%s", prefix, n < 0 ? "-" : "");
    for (int i = 0; i < len; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = digits[i];
    }
    out[o] = 0;
}

// A TextItem's text, as TextItem::SetNumber (0x408920) sets it: clear the laid-out glyphs
// (0x422b80, esi = them, at +0xb4), assign the string (+0x98), lay it out again (0x450cc0,
// stdcall(glyphs, string)) and refresh (vtable +0xa8).
#define TEXT_STRING            0x98
#define TEXT_GLYPHS            0xb4
#define FN_TEXT_CLEAR_GLYPHS   0x422b80
#define FN_STRING_ASSIGN       0x448ee0  // thiscall(std::string*, const std::string*, pos, count)
#define FN_TEXT_LAYOUT         0x450cc0
#define TEXT_VTBL_REFRESH      0xa8

void coop_set_text(BYTE *text, const char *value)
{
    if (!text || *(DWORD *)text != VTBL_TEXT_ITEM || strcmp(text_string(text), value) == 0) return;
    text_assign(text, value);
}

// The same for any TextItem (the lives counter has a vtable of its own).
static void text_assign(BYTE *text, const char *value)
{
    typedef void (__thiscall *Assign_t)(BYTE *, const void *, int, int);
    typedef void (__stdcall *Layout_t)(BYTE *, BYTE *);
    typedef void (__thiscall *Refresh_t)(BYTE *);
    DWORD str[8];
    static const DWORD clear = FN_TEXT_CLEAR_GLYPHS;
    __asm__ volatile("call *%1" : : "S"(text + TEXT_GLYPHS), "m"(clear) : "eax", "ecx", "edx", "memory", "cc");
    ((StrCtor_t)FN_STRING_FROM_CSTR)(str, value);
    ((Assign_t)FN_STRING_ASSIGN)(text + TEXT_STRING, str, 0, -1);
    ((StrFree_t)FN_STRING_FREE)(str, 1, 0);
    ((Layout_t)FN_TEXT_LAYOUT)(text + TEXT_GLYPHS, text + TEXT_STRING);
    ((Refresh_t)(*(void ***)text)[TEXT_VTBL_REFRESH / 4])(text);
}

// The text inside a button (comboButton: an image and a TextItem).
BYTE *coop_button_text(BYTE *button)
{
    if (!button) return NULL;
    BYTE **child = *(BYTE ***)(button + CHILDREN_BEGIN), **end = *(BYTE ***)(button + CHILDREN_END);
    for (; child && child < end; child++)
        if (*child && **(DWORD **)child == VTBL_TEXT_ITEM) return *child;
    return NULL;
}

// The HUD's per-player totals (hudlayout.xml: p1ScoreText .. p4ScoreText), blank for players
// not in this stage and out of co-op. A new game starts the totals over: the shared score
// (scoreText) went down.
void coop_show_scores(BYTE *total, BYTE **texts)
{
    static int last_total = -1;
    if (total && *(DWORD *)total == VTBL_TEXT_ITEM) {
        int n = 0;
        for (const char *c = text_string(total); *c; c++)
            if (*c >= '0' && *c <= '9') n = n * 10 + (*c - '0');
        if (n < last_total)
            for (int i = 0; i < MAX_PLAYERS; i++) pl[i].score = 0;
        last_total = n;
    }
    int together = enabled && g_stage && team();
    for (int i = 0; i < MAX_PLAYERS; i++) {
        char text[32], prefix[8];
        int shown = together && present(i);
        sprintf(prefix, "P%d ", i + 1);
        if (shown) format_points(text, prefix, pl[i].score);
        coop_set_text(texts[i], shown ? text : "");
        if (texts[i] && i > 0) memcpy(texts[i] + VISUAL_COLOR, pl[i].tint, 3 * sizeof(float));
    }
}

// Stage::Reset (0x486950, Stage vtable slot 0x5585d8) runs as a level is left without
// finishing it (e.g. quitting with "close but don't save"). With fish still in the stage's
// player list it reads the first player spawner's level (0x4f37cf), but by then the
// spawners are gone; in one player the list is empty too, but the other players' fish were
// still in it, so the game's container check closed the game. They leave first, as the
// keeper has.
#define ADDR_STAGE_RESET_SLOT  0x5585d8
#define FN_STAGE_RESET         0x486950
#define STAGE_SPAWNERS_BEGIN   0x294     // std::vector<PlayerSpawn*>, end at +0x298
#define STAGE_SPAWNERS_END     0x298
typedef void (__thiscall *StageReset_t)(BYTE *stage);

void __thiscall coop_stage_reset_hook(BYTE *stage)
{
    if (*(BYTE **)(stage + STAGE_SPAWNERS_BEGIN) == *(BYTE **)(stage + STAGE_SPAWNERS_END)) {
        for (int i = 0; i < MAX_PLAYERS; i++)
            if (pl[i].fish && in_stage(stage, pl[i].fish)) {
                ((Leave_t)FN_PLAYER_LEAVE)(pl[i].fish);
                log_printf("co-op: level left unfinished, player %d taken out first", i + 1);
            }
        keeper = 0;
    }
    ((StageReset_t)FN_STAGE_RESET)(stage);
}

int coop_init(int enable, const char *mode_key, float speed, const char *const *player_colors,
              int (*patch_u32)(DWORD, DWORD, DWORD))
{
    patch_u32_fn = patch_u32;
    for (int i = 0; i < MODE_COUNT; i++)
        if (mode_key && !lstrcmpiA(mode_key, mode_keys[i])) mode_requested = i;
    patch_u32(ADDR_SAVE_RECORD_CALL + 1, FN_SAVE_RECORD - (ADDR_SAVE_RECORD_CALL + 5),
              (DWORD)coop_save_record_thunk - (ADDR_SAVE_RECORD_CALL + 5));
    patch_u32(ADDR_OPTIONS_ACTION_SLOT, FN_OPTIONS_ACTION, (DWORD)coop_options_action_hook);
    patch_u32(ADDR_POPUP_ACTION_SLOT, FN_POPUP_ACTION, (DWORD)coop_popup_action_hook);
    for (int i = 0; i < (int)(sizeof splat_calls / sizeof *splat_calls); i++)
        patch_u32(splat_calls[i] + 1, FN_SCORE_SPLAT - (splat_calls[i] + 5),
                  (DWORD)coop_score_splat_hook - (splat_calls[i] + 5));
    for (int i = 0; i < (int)(sizeof splat_new_calls / sizeof *splat_new_calls); i++)
        patch_u32(splat_new_calls[i] + 1, FN_SCORE_SPLAT_NEW - (splat_new_calls[i] + 5),
                  (DWORD)coop_score_splat_new_hook - (splat_new_calls[i] + 5));
    requested = enable != 0;
    if (speed > 0) pad_speed = speed;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        pl[i].lives = -1;
        pl[i].growth = -1;
        int r, g, b, c = default_colors[i];
        if (i == 0) set_tint(0, 255, 255, 255);
        else if (player_colors && player_colors[i] && sscanf(player_colors[i], "%d,%d,%d", &r, &g, &b) == 3)
            set_tint(i, r, g, b);
        else
            set_tint(i, colors[c].r, colors[c].g, colors[c].b);
        if (i) log_printf("co-op: player %d color %s", i + 1, coop_color_name(i + 1));
    }
    patch_u32(ADDR_PLAYER_ENTER_SLOT, FN_PLAYER_ENTER, (DWORD)coop_enter_hook);
    patch_u32(ADDR_STAGE_RESET_SLOT, FN_STAGE_RESET, (DWORD)coop_stage_reset_hook);
    patch_u32(ADDR_PLAYER_TRY_EAT_SLOT, FN_PLAYER_TRY_EAT, (DWORD)coop_try_eat_hook);
    patch_u32(ADDR_PLAYER_EDIBLE_SLOT, FN_PLAYER_EDIBLE, (DWORD)coop_player_edible_hook);
    patch_u32(ADDR_SHIELD_EDIBLE_SLOT, FN_SHIELD_EDIBLE, (DWORD)coop_shield_edible_hook);
    patch_u32(ADDR_EAT_UPDATE_SLOT, FN_EAT_UPDATE, (DWORD)coop_eat_update_hook);
    patch_u32(ADDR_GROW_METER_DRAW_SLOT, FN_GROW_METER_DRAW, (DWORD)coop_grow_meter_draw);
    patch_u32(ADDR_SUCK_CALL + 1, FN_SUCK - (ADDR_SUCK_CALL + 5), (DWORD)coop_suck_thunk - (ADDR_SUCK_CALL + 5));
    apply_requested();
    patch_u32(ADDR_COLOR_FLASH_SLOT, FN_COLOR_FLASH_APPLY, (DWORD)coop_color_flash_hook);
    patch_u32(ADDR_SPAWN_UPDATE_SLOT, FN_SPAWN_UPDATE, (DWORD)coop_spawn_update_hook);
    patch_u32(ADDR_PLAYER_LEAVE_SLOT, FN_PLAYER_LEAVE, (DWORD)coop_leave_hook);
    patch_u32(ADDR_PLAYER_ADD_FRENZY_SLOT, FN_PLAYER_ADD_FRENZY, (DWORD)coop_add_frenzy_hook);
    patch_u32(ADDR_BIND_MOUSE_CALL_1 + 1, FN_BIND_MOUSE - (ADDR_BIND_MOUSE_CALL_1 + 5),
              (DWORD)coop_bind_mouse_hook - (ADDR_BIND_MOUSE_CALL_1 + 5));
    patch_u32(ADDR_BIND_MOUSE_CALL_2 + 1, FN_BIND_MOUSE - (ADDR_BIND_MOUSE_CALL_2 + 5),
              (DWORD)coop_bind_mouse_hook - (ADDR_BIND_MOUSE_CALL_2 + 5));
    // HudSetLives starts 6a 1c b8 8a 12 (push 0x1c; mov eax, ...): becomes `jmp ours`, its
    // rel32 written first, then the opcode byte.
    DWORD rel = (DWORD)coop_hud_set_lives - (FN_HUD_SET_LIVES_START + 5);
    if (patch_u32(FN_HUD_SET_LIVES_START + 1, 0x128ab81c, rel))
        patch_u32(FN_HUD_SET_LIVES_START, 0x6a | rel << 8, 0xe9 | rel << 8);
    // The Stun bonus's question becomes `call ours; nop`: rel32, then the nop over the
    // displacement's last byte, then the opcode.
    rel = (DWORD)coop_stun_bonus_thunk - (ADDR_STUN_BONUS_ASK + 5);
    if (patch_u32(ADDR_STUN_BONUS_ASK + 1, 0x0000c490, rel) &&
        patch_u32(ADDR_STUN_BONUS_ASK + 5, 0x74c08400, 0x74c08490))
        patch_u32(ADDR_STUN_BONUS_ASK, 0xff | rel << 8, 0xe8 | rel << 8);
    // GetPlayer starts 56 8b f0 81 c6 (push esi; mov esi, eax; add esi, ...): same trick.
    rel = (DWORD)coop_get_player_thunk - (FN_STAGE_GET_PLAYER + 5);
    if (patch_u32(FN_STAGE_GET_PLAYER + 1, 0xc681f08b, rel))
        patch_u32(FN_STAGE_GET_PLAYER, 0x56 | rel << 8, 0xe9 | rel << 8);
    for (int i = 0; i < (int)(sizeof ai_decide_slots / sizeof *ai_decide_slots); i++)
        patch_u32(ai_decide_slots[i], FN_AIFISH_DECIDE, (DWORD)coop_ai_decide_hook);
    patch_u32(ADDR_DIED_STATE_UPDATE_SLOT, FN_DIED_STATE_UPDATE, (DWORD)coop_died_state_update_hook);
    HMODULE xinput = LoadLibraryA("xinput1_4.dll");
    if (!xinput) xinput = LoadLibraryA("xinput9_1_0.dll");
    if (xinput) xinput_get_state = (XInputGetState_t)(void *)GetProcAddress(xinput, "XInputGetState");
    log_printf("co-op %s: up to %d players, others join with %s, speed %.0f", enabled ? "on" : "off", MAX_PLAYERS,
               xinput_get_state ? "gamepads, keyboards or mice" : "keyboards or mice (no XInput)", pad_speed);
    return patch_u32(ADDR_PLAYER_SPAWN_SLOT, FN_PLAYER_SPAWN, (DWORD)coop_spawn_hook);
}
