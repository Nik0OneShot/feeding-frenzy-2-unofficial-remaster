#pragma once
#include <windows.h>

// Gameplay changes beyond the picture (tweaks.c): the Frenzy chain holding for the mermaid
// after a perfect stage, and golden minnows coming in on screen.
void tweaks_init(int (*patch_u32)(DWORD, DWORD, DWORD));
