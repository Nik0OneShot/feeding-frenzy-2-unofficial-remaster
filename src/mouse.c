// Gameplay mouse steering from the real mouse.
//
// During play the game hides the cursor and, every frame, RelativeMouse::Recenter
// (0x42b8c0, esi = RelativeMouse) sends (cursor - center) in game pixels to the player and
// warps the cursor back to the center. Under cnc-ddraw the cursor position is scaled from
// screen pixels down to game pixels (3440 -> 1434), so hand movement counted 2.4x less than
// on original hardware, and with integer deltas at high frame rates small movements rounded
// to zero every frame and were lost.
//
// The replacement reads the real cursor in screen pixels (calling user32 directly, past
// cnc-ddraw's hooks), parks it at the monitor's center, converts the movement to game pixels
// (1:1 like the original at mouse_sensitivity=1) and carries fractions over to the next frame.
#include <windows.h>
#include <math.h>
#include "log.h"
#include "mouse.h"
#include "coop.h"
#include "mice.h"

#define MOUSE_DELTA_SIGNAL 0x0c      // RelativeMouse+0x0c: signal1<const Vector2<int>&>
#define FN_SIGNAL_EMIT     0x427cd0  // eax = signal, [esp+4] = const Vector2<int>*, ret 4
#define RESET_AFTER_MS     150       // a pause this long means the mode was (re)entered

static BOOL (WINAPI *real_get_cursor_pos)(POINT *);
static BOOL (WINAPI *real_set_cursor_pos)(int, int);
static float sensitivity = 1;
static float carry_x, carry_y;
static DWORD last_call;

void mouse_init(float mouse_sensitivity)
{
    HMODULE user32 = GetModuleHandleA("user32.dll");
    real_get_cursor_pos = (void *)GetProcAddress(user32, "GetCursorPos");
    real_set_cursor_pos = (void *)GetProcAddress(user32, "SetCursorPos");
    sensitivity = mouse_sensitivity;
    log_printf("mouse sensitivity %.2f", sensitivity);
}

float mouse_get_sensitivity(void)
{
    return sensitivity;
}

static void emit(BYTE *relative_mouse, int dx, int dy)
{
    int delta[2] = { dx, dy };
    BYTE *signal = relative_mouse + MOUSE_DELTA_SIGNAL;
    __asm__ __volatile__(
        "pushl %1\n\t"
        "call *%2\n\t"
        : "+a"(signal)
        : "r"(delta), "r"(FN_SIGNAL_EMIT)
        : "ecx", "edx", "memory", "cc");
}

static POINT screen_center(void)
{
    POINT c = { GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2 };
    HWND window = GetForegroundWindow();
    MONITORINFO info = { .cbSize = sizeof info };
    if (window && GetMonitorInfoA(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &info)) {
        c.x = (info.rcMonitor.left + info.rcMonitor.right) / 2;
        c.y = (info.rcMonitor.top + info.rcMonitor.bottom) / 2;
    }
    return c;
}

void __cdecl mouse_recenter(BYTE *relative_mouse)
{
    if (!real_get_cursor_pos || !real_set_cursor_pos) return;
    POINT center = screen_center(), p;
    DWORD now = GetTickCount();
    int reset = now - last_call > RESET_AFTER_MS;
    last_call = now;
    if (!real_get_cursor_pos(&p)) return;
    // With other players' mice, all move the cursor: player 1 steers with its own mouse's
    // movement.
    int own_x, own_y;
    int own = mice_take(MICE_PLAYER1, &own_x, &own_y, NULL);
    if (reset) {
        carry_x = carry_y = 0;
    } else {
        carry_x += (own ? own_x : p.x - center.x) * sensitivity;
        carry_y += (own ? own_y : p.y - center.y) * sensitivity;
        int dx = (int)carry_x, dy = (int)carry_y; // toward zero; the rest carries over
        carry_x -= dx;
        carry_y -= dy;
        if (dx || dy) emit(relative_mouse, dx, dy);
    }
    if (p.x != center.x || p.y != center.y) real_set_cursor_pos(center.x, center.y);
}

// --- Player water drag -----------------------------------------------------------------
// PlayerFish::ApplyDrag (0x43ae90, edi = PlayerFish) slows the fish by
//   drag = speed^2 * fish.drag * inWater * option + dashDrag
// once per frame, with no elapsed time in it (the forces and movement around it do use it),
// so at 165 fps drag was applied 2.75x as often per second and dashes died early. This is
// the same calculation scaled by (elapsed seconds * 60): identical at 60 fps, the same per
// second at any rate.

#define PLAYER_VELOCITY   0xb0   // float x, y
#define PLAYER_STOPPED    0xc9   // bool: drag stopped the fish this frame
#define PLAYER_DASH_DRAG  0xd0   // float, set when a dash starts
#define PLAYER_IN_WATER   0x198  // float 0..1
#define PLAYER_FISH_INFO  0x138  // FishInfo*; +0xac = drag
#define FISHINFO_DRAG     0xac
#define GAME_GLOBAL       0x5ac624 // FeedingFrenzyTwo*; +0xb8 = options, vtable +0x44 = float
#define GAME_OPTIONS      0xb8

void __cdecl player_drag(BYTE *player, float dt)
{
    BYTE *options = *(BYTE **)(*(BYTE **)GAME_GLOBAL + GAME_OPTIONS);
    typedef float (__thiscall *GetFloat_t)(BYTE *);
    float option = ((GetFloat_t)(*(void ***)options)[0x44 / 4])(options);
    float *v = (float *)(player + PLAYER_VELOCITY);
    float speed2 = v[0] * v[0] + v[1] * v[1];
    float drag = speed2 * *(float *)(*(BYTE **)(player + PLAYER_FISH_INFO) + FISHINFO_DRAG)
                 * (*(float *)(player + PLAYER_IN_WATER) * option) + *(float *)(player + PLAYER_DASH_DRAG);
    drag *= dt * 60;
    float speed = sqrtf(speed2);
    if (speed < drag) {
        player[PLAYER_STOPPED] = 1;
        v[0] = v[1] = 0;
    } else {
        player[PLAYER_STOPPED] = 0;
        if (speed > 0) {
            v[0] -= v[0] / speed * drag;
            v[1] -= v[1] / speed * drag;
        }
    }
    coop_steer(player, dt, option);
}

// Replaces `call PlayerFish::ApplyDrag` in PlayerFish::Physics (0x43b2e0), whose frame has
// the update info at [ebp+8] (elapsed seconds at +8); edi = PlayerFish.
void player_drag_thunk(void);
__asm__(
    ".globl _player_drag_thunk\n"
    "_player_drag_thunk:\n"
    "    pushal\n"
    "    movl 8(%ebp), %eax\n"
    "    pushl 8(%eax)\n"
    "    pushl %edi\n"
    "    call _player_drag\n"
    "    addl $8, %esp\n"
    "    popal\n"
    "    ret\n");

// Replaces RelativeMouse::Recenter (esi = RelativeMouse, plain ret).
void mouse_recenter_thunk(void);
__asm__(
    ".globl _mouse_recenter_thunk\n"
    "_mouse_recenter_thunk:\n"
    "    pushal\n"
    "    pushl %esi\n"
    "    call _mouse_recenter\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    ret\n");
