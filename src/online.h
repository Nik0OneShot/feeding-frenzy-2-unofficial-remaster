#pragma once
#include <windows.h>
#include "../shared/online_api.h"

// Online co-op host: with on, loads ff2online.dll and starts hosting.
void online_init(int on, int port, int kbps, int fps, int width, int height, int upnp);
// Hosting is on (ff2online.dll loaded and started).
int online_hosting(void);
// Each frame, before it's presented: hands the frame over when one is due to be sent, with
// where each guest's "You!" label goes (x ONLINE_NO_MARK: nowhere), and takes the guests'
// latest input.
#define ONLINE_NO_MARK (-32768)
void online_frame(void **back_surface, const int marks[ONLINE_MAX_GUESTS][2]);
int online_connected(void);           // some guest is
// The "You!" label with its arrow's tip at (x, y), into the frame.
void online_draw_you(void **back_surface, int x, int y);
// A guest's input (guest 0 ..; 0: not connected): stick / keys and NET_DASH | NET_SPECIAL,
// and its mouse movement since the last frame (taken once).
int online_guest(int guest, float *x, float *y, int *buttons);
void online_guest_mouse(int guest, int *dx, int *dy);
