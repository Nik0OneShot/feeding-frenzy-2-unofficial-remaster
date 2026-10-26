#pragma once
#include <windows.h>

void guest_log(const char *fmt, ...);

void sound_start(void);                                   // sound.c
void sound_reset(void);
void sound_push(const short *pcm, int frames, DWORD seq);
