#pragma once
#include <windows.h>

void mouse_init(float mouse_sensitivity);
float mouse_get_sensitivity(void);
void mouse_recenter_thunk(void);
void player_drag_thunk(void);
