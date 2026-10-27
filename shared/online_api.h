// Between the patch (version.dll) and ff2online.dll, the online co-op host. The patch loads
// ff2online.dll only with online play switched on, so otherwise the game never loads the
// network, video and audio DLLs it needs.
#pragma once
#include <windows.h>

typedef struct {
    int port;                       // UDP
    int kbps, fps;                  // video stream
    int width, height;              // the game's back buffer
    int upnp;                       // ask the router to open the port
    int loopback_only;              // tests: only this PC can connect
    void (*log)(const char *fmt, ...);
} OnlineConfig;

#define ONLINE_MAX_GUESTS 9          // players 2 to 10

typedef struct {
    int connected;
    float x, y;                     // stick / keys, length <= 1
    int buttons;                    // NET_DASH | NET_SPECIAL (proto.h)
    int mouse_x, mouse_y;           // running totals of the guest's mouse movement
} OnlineInput;

// Exported by ff2online.dll (cdecl, undecorated names).
typedef int (*OnlineStart_t)(const OnlineConfig *cfg);      // 0: failed (logged)
typedef int (*OnlineWantsFrame_t)(void);                    // a frame is due to be sent
// Copies the frame, returns at once. marks: where each guest's "You!" label goes, in the
// frame's pixels (x, y per guest; x -32768: nowhere).
typedef void (*OnlineVideo_t)(const BYTE *bgra, int pitch, const int marks[ONLINE_MAX_GUESTS][2]);
// Each guest's latest input (out[ONLINE_MAX_GUESTS]); a guest keeps its place while it's gone.
typedef void (*OnlineGetInput_t)(OnlineInput *out);
// Switched off: every guest is let go and new ones turned away; on again: taking guests.
typedef void (*OnlineSetOpen_t)(int on);
// What friends type in to connect (the internet address once known, else this PC's).
typedef void (*OnlineAddress_t)(char *out, int len);
