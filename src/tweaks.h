#pragma once
#include <windows.h>

// Gameplay changes beyond the picture (tweaks.c), each switched by its setting: the Remaster's
// golden minnows (else the original's), and the Frenzy chain waiting for the mermaid after a
// perfect stage.
void tweaks_init(int golden_minnows, int frenzy_waits, int (*patch_u32)(DWORD, DWORD, DWORD));
