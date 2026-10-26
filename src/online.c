// Online co-op host: connects the game to ff2online.dll (patch/online), which streams the
// game to guests' ff2guest.exe and brings each guest's input back as a player's (coop.c).
// The DLL is loaded only with online=1 in ff2wide.ini.
#include <windows.h>
#include "../shared/label.h"
#include "../shared/online_api.h"
#include "ambient.h"
#include "coop.h"
#include "log.h"
#include "online.h"

static OnlineWantsFrame_t wants_frame;
static OnlineVideo_t video;
static OnlineGetInput_t get_input;
static OnlineInput latest[ONLINE_MAX_GUESTS];
static int was_connected[ONLINE_MAX_GUESTS], last_mouse[ONLINE_MAX_GUESTS][2], mouse_moved[ONLINE_MAX_GUESTS][2];
static int frame_width, frame_height;

void online_init(int on, int port, int kbps, int fps, int width, int height, int upnp)
{
    if (!on) return;
    char path[MAX_PATH], *slash = path;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    for (char *p = path; *p; p++)
        if (*p == '\\') slash = p + 1;
    lstrcpyA(slash, "ff2online.dll");
    HMODULE dll = LoadLibraryA(path);
    OnlineStart_t start = dll ? (OnlineStart_t)(void *)GetProcAddress(dll, "online_start") : NULL;
    wants_frame = dll ? (OnlineWantsFrame_t)(void *)GetProcAddress(dll, "online_wants_frame") : NULL;
    video = dll ? (OnlineVideo_t)(void *)GetProcAddress(dll, "online_video") : NULL;
    get_input = dll ? (OnlineGetInput_t)(void *)GetProcAddress(dll, "online_get_input") : NULL;
    if (!start || !wants_frame || !video || !get_input) {
        log_printf("online: ff2online.dll is missing next to the game (error %lu)", GetLastError());
        get_input = NULL;
        return;
    }
    frame_width = width;
    frame_height = height;
    OnlineConfig cfg = { port, kbps, fps, width, height, upnp, 0, log_printf };
    if (!start(&cfg)) get_input = NULL;
}

int online_hosting(void)
{
    return get_input != NULL;
}

void online_frame(void **back_surface, const int marks[ONLINE_MAX_GUESTS][2])
{
    if (!get_input) return;
    get_input(latest);
    for (int g = 0; g < ONLINE_MAX_GUESTS; g++) {
        OnlineInput *in = &latest[g];
        if (in->connected && !was_connected[g]) {
            last_mouse[g][0] = in->mouse_x;
            last_mouse[g][1] = in->mouse_y;
            if (!coop_requested()) {
                coop_request(1);
                log_printf("online: co-op switched on for the guests");
            }
        }
        was_connected[g] = in->connected;
        // Overwritten, not added up: movement nobody took this frame (menus, pauses) is dropped.
        mouse_moved[g][0] = in->mouse_x - last_mouse[g][0];
        mouse_moved[g][1] = in->mouse_y - last_mouse[g][1];
        last_mouse[g][0] = in->mouse_x;
        last_mouse[g][1] = in->mouse_y;
    }
    if (back_surface && online_connected() && wants_frame()) {
        int pitch;
        BYTE *bits = ambient_lock(back_surface, &pitch);
        if (bits) {
            video(bits, pitch, marks);
            ambient_unlock(back_surface);
        }
    }
}

int online_connected(void)
{
    for (int g = 0; g < ONLINE_MAX_GUESTS; g++)
        if (latest[g].connected) return 1;
    return 0;
}

void online_draw_you(void **back_surface, int x, int y)
{
    static Label label;
    static int made;
    if (!made++) label_make(&label, "You!", 18);
    int pitch;
    BYTE *bits = back_surface ? ambient_lock(back_surface, &pitch) : NULL;
    if (!bits) return;
    label_blit(&label, bits, pitch, frame_width, frame_height, x, y);
    ambient_unlock(back_surface);
}

int online_guest(int guest, float *x, float *y, int *buttons)
{
    if (guest < 0 || guest >= ONLINE_MAX_GUESTS || !latest[guest].connected) return 0;
    *x = latest[guest].x;
    *y = latest[guest].y;
    *buttons = latest[guest].buttons;
    return 1;
}

void online_guest_mouse(int guest, int *dx, int *dy)
{
    int ok = guest >= 0 && guest < ONLINE_MAX_GUESTS && latest[guest].connected;
    *dx = ok ? mouse_moved[guest][0] : 0;
    *dy = ok ? mouse_moved[guest][1] : 0;
    if (ok) mouse_moved[guest][0] = mouse_moved[guest][1] = 0;
}
