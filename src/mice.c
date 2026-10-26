// Several mice and keyboards: more players on one PC.
//
// Windows moves the one cursor with every mouse and sends their clicks as the same
// messages, so the game can't tell them apart. Raw Input can: a thread with a message-only
// window receives every mouse's own movement and buttons (RIDEV_INPUTSINK, so also while
// the game has the focus). Roles:
//  - the mouse last used in the menus is player 1's;
//  - another mouse clicked during play joins the game as a player (coop.c claims it with
//    mice_new_click); it keeps that player for the session.
// While some other mouse belongs to a player and a level is being played:
//  - player 1 steers with its own mouse's movement (mouse.c asks mice_take);
//  - the shared click messages are kept from the game (a WH_MOUSE hook on the game's
//    thread), and each player's clicks are handed out per mouse instead (coop.c).
// In the menus all mice work as usual.
//
// Keyboards the same way: each keyboard's keys are tracked apart, so each one's WASD and
// arrow keys can be two players of their own (coop.c). Until raw input has seen a keyboard,
// the keys are read from Windows (all keyboards together, as one).
#include <windows.h>
#include "log.h"
#include "mice.h"

#define MAX_MICE 8

typedef struct {
    HANDLE device;
    LONG dx, dy;        // movement since last taken, in mouse counts
    int buttons;        // MICE_LEFT | MICE_RIGHT currently held
    int claimed;        // belongs to a player (not player 1)
    int clicked;        // clicked during play while nobody's
} Mouse;

static CRITICAL_SECTION lock;
static Mouse mice[MAX_MICE];
static int mouse_count;
static int player1_mouse = -1;
static int claimed_count;
static volatile LONG in_play;
static HHOOK click_hook;
static HANDLE keyboards[MICE_MAX_KEYBOARDS];
static volatile LONG keyboard_count;
static volatile BYTE keys[MICE_MAX_KEYBOARDS][256];

static int mouse_index(HANDLE d)
{
    for (int i = 0; i < mouse_count; i++)
        if (mice[i].device == d) return i;
    if (mouse_count == MAX_MICE) return -1;
    mice[mouse_count] = (Mouse){ .device = d };
    return mouse_count++;
}

static void on_keyboard(RAWINPUT *in)
{
    RAWKEYBOARD *k = &in->data.keyboard;
    int vk = k->VKey, up = (k->Flags & RI_KEY_BREAK) != 0, e0 = (k->Flags & RI_KEY_E0) != 0;
    if (vk == VK_SHIFT) vk = k->MakeCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    else if (vk == VK_CONTROL) vk = e0 ? VK_RCONTROL : VK_LCONTROL;
    else if (vk == VK_MENU) vk = e0 ? VK_RMENU : VK_LMENU;
    if (vk <= 0 || vk >= 256) return;
    HANDLE d = in->header.hDevice;
    EnterCriticalSection(&lock);
    int i = 0;
    while (i < keyboard_count && keyboards[i] != d) i++;
    if (i == keyboard_count && !up && i < MICE_MAX_KEYBOARDS) {
        keyboards[i] = d;
        keyboard_count = i + 1;
        log_printf("keyboards: keyboard %d is %p", i + 1, d);
    }
    if (i < keyboard_count) keys[i][vk] = !up;
    LeaveCriticalSection(&lock);
}

static void on_input(HRAWINPUT handle)
{
    BYTE buf[64];
    UINT size = sizeof buf;
    if (GetRawInputData(handle, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    RAWINPUT *in = (RAWINPUT *)buf;
    if (in->header.dwType == RIM_TYPEKEYBOARD && in->header.hDevice) {
        on_keyboard(in);
        return;
    }
    if (in->header.dwType != RIM_TYPEMOUSE || !in->header.hDevice) return;
    RAWMOUSE *m = &in->data.mouse;
    LONG dx = 0, dy = 0;
    if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE)) dx = m->lLastX, dy = m->lLastY;
    USHORT b = m->usButtonFlags;
    if (!dx && !dy && !(b & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_LEFT_BUTTON_UP |
                             RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_UP)))
        return;

    EnterCriticalSection(&lock);
    int i = mouse_index(in->header.hDevice);
    if (i >= 0) {
        Mouse *r = &mice[i];
        if (!in_play) {
            if (!r->claimed && player1_mouse != i) {
                player1_mouse = i;
                log_printf("mice: player 1's mouse is %p", r->device);
            }
        } else {
            if (player1_mouse < 0 && !r->claimed) player1_mouse = i;
            r->dx += dx;
            r->dy += dy;
            if (b & RI_MOUSE_LEFT_BUTTON_DOWN) r->buttons |= MICE_LEFT;
            if (b & RI_MOUSE_LEFT_BUTTON_UP) r->buttons &= ~MICE_LEFT;
            if (b & RI_MOUSE_RIGHT_BUTTON_DOWN) r->buttons |= MICE_RIGHT;
            if (b & RI_MOUSE_RIGHT_BUTTON_UP) r->buttons &= ~MICE_RIGHT;
            if (b & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN)) r->clicked = 1;
        }
    }
    LeaveCriticalSection(&lock);
}

static LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INPUT) on_input((HRAWINPUT)lp);
    return DefWindowProcA(window, msg, wp, lp);
}

static DWORD WINAPI input_thread(void *unused)
{
    (void)unused;
    WNDCLASSA wc = { .lpfnWndProc = window_proc, .hInstance = GetModuleHandleA(NULL), .lpszClassName = "ff2wide_mice" };
    RegisterClassA(&wc);
    HWND window = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    RAWINPUTDEVICE rid[2] = {
        { .usUsagePage = 1, .usUsage = 2, .dwFlags = RIDEV_INPUTSINK, .hwndTarget = window }, // mice
        { .usUsagePage = 1, .usUsage = 6, .dwFlags = RIDEV_INPUTSINK, .hwndTarget = window }, // keyboards
    };
    if (!window || !RegisterRawInputDevices(rid, 2, sizeof rid[0])) {
        log_printf("mice: raw input unavailable (%lu)", GetLastError());
        return 0;
    }
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) DispatchMessageA(&msg);
    return 0;
}

void mice_init(void)
{
    InitializeCriticalSection(&lock);
    HANDLE thread = CreateThread(NULL, 0, input_thread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
}

int mice_shared_out(void)
{
    return in_play && claimed_count > 0;
}

int mice_new_click(void)
{
    if (!in_play) return -1;
    int found = -1;
    EnterCriticalSection(&lock);
    for (int i = 0; i < mouse_count && found < 0; i++) {
        Mouse *r = &mice[i];
        if (r->claimed || i == player1_mouse || !r->clicked) continue;
        r->claimed = 1;
        r->dx = r->dy = 0;
        claimed_count++;
        found = i;
        log_printf("mice: mouse %p joins as a player", r->device);
    }
    for (int i = 0; i < mouse_count; i++) mice[i].clicked = 0;
    LeaveCriticalSection(&lock);
    return found;
}

int keys_held(int keyboard, int vk)
{
    if (vk <= 0 || vk >= 256) return 0;
    if (!keyboard_count) return keyboard == 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
    return in_play && keyboard >= 0 && keyboard < keyboard_count && keys[keyboard][vk];
}

int keys_keyboards(void)
{
    return keyboard_count ? keyboard_count : 1;
}

// Clicks on the game's own thread, before its window sees them: dropped while mice are
// shared out during play.
static LRESULT CALLBACK click_proc(int code, WPARAM msg, LPARAM lp)
{
    if (code >= 0 && mice_shared_out() &&
        (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK ||
         msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_RBUTTONDBLCLK))
        return 1;
    return CallNextHookEx(click_hook, code, msg, lp);
}

void mice_set_play(int playing)
{
    if (!click_hook) {
        click_hook = SetWindowsHookExA(WH_MOUSE, click_proc, NULL, GetCurrentThreadId());
        if (!click_hook) log_printf("mice: click hook failed (%lu)", GetLastError());
    }
    if ((LONG)(playing != 0) == in_play) return;
    EnterCriticalSection(&lock);
    in_play = playing != 0;
    // Movement, clicks and keys from before (or from the menus) don't carry into play.
    for (int i = 0; i < mouse_count; i++) {
        mice[i].dx = mice[i].dy = 0;
        mice[i].buttons = mice[i].clicked = 0;
    }
    for (int k = 0; k < MICE_MAX_KEYBOARDS; k++)
        for (int i = 0; i < 256; i++) keys[k][i] = 0;
    LeaveCriticalSection(&lock);
}

static Mouse *mouse_for(int id)
{
    if (id == MICE_PLAYER1) return mice_shared_out() && player1_mouse >= 0 ? &mice[player1_mouse] : NULL;
    return in_play && id >= 0 && id < mouse_count && mice[id].claimed ? &mice[id] : NULL;
}

int mice_buttons(int id)
{
    Mouse *r = mouse_for(id);
    return r ? r->buttons : 0;
}

int mice_take(int id, int *dx, int *dy, int *buttons)
{
    EnterCriticalSection(&lock);
    Mouse *r = mouse_for(id);
    if (r) {
        *dx = r->dx;
        *dy = r->dy;
        if (buttons) *buttons = r->buttons;
        r->dx = r->dy = 0;
    }
    LeaveCriticalSection(&lock);
    return r != NULL;
}
