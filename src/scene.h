#pragma once
#include <windows.h>

void scene_init(int width, int shift);
void scene_before_draw(BYTE *engine);
void scene_after_draw(void);
int scene_stage_shown(void);
// 0..1: how far a stage complete panel is faded in this frame.
float scene_complete_panel_opacity(void);
