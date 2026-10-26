#pragma once
#include <windows.h>

void ambient_init(int width, int shift);
// surface: the IDirectDrawSurface* the frame was drawn into.
void ambient_fill(void **surface);
// Co-op zoom: around `content` (back-buffer pixels the scenery covers), the scenery's picture
// blurred, enlarged and darkened. Capture after drawing a frame; fill before drawing the next.
void ambient_zoom_capture(void **surface, const RECT *content);
void ambient_zoom_fill(void **surface, const RECT *content);
// Beside a stage complete panel (opacity 0..1): its blurred picture, faded in with it.
void ambient_panel_fill(void **surface, float opacity);
// The surface's 32-bit pixels for reading (NULL: not lockable); unlock when done.
BYTE *ambient_lock(void **surface, int *pitch);
void ambient_unlock(void **surface);
