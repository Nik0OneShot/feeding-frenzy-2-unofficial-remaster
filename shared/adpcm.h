// IMA ADPCM: 16-bit samples as 4-bit codes, a quarter of the size. Each packet carries the
// coder state it starts from, so packets decode on their own and a lost one is just a gap.
#pragma once

typedef struct { int pred, index; } AdpcmState;

// Stereo: frames interleaved L,R in `pcm`, one byte out per frame (left nibble low).
void adpcm_encode(AdpcmState st[2], const short *pcm, int frames, unsigned char *out);
void adpcm_decode(AdpcmState st[2], const unsigned char *in, int frames, short *pcm);
