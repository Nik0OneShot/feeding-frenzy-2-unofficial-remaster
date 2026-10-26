#include "adpcm.h"

static const int steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
    66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
    408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
};
static const int index_step[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

// Applies a code to the state, the same way for both directions.
static void step_state(AdpcmState *s, int code)
{
    int step = steps[s->index];
    int delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    s->pred += (code & 8) ? -delta : delta;
    if (s->pred > 32767) s->pred = 32767;
    if (s->pred < -32768) s->pred = -32768;
    s->index += index_step[code];
    if (s->index < 0) s->index = 0;
    if (s->index > 88) s->index = 88;
}

static int encode_one(AdpcmState *s, int sample)
{
    int step = steps[s->index];
    int diff = sample - s->pred, code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= step) code |= 4, diff -= step;
    if (diff >= step >> 1) code |= 2, diff -= step >> 1;
    if (diff >= step >> 2) code |= 1;
    step_state(s, code);
    return code;
}

void adpcm_encode(AdpcmState st[2], const short *pcm, int frames, unsigned char *out)
{
    for (int i = 0; i < frames; i++)
        out[i] = (unsigned char)(encode_one(&st[0], pcm[2 * i]) | encode_one(&st[1], pcm[2 * i + 1]) << 4);
}

void adpcm_decode(AdpcmState st[2], const unsigned char *in, int frames, short *pcm)
{
    for (int i = 0; i < frames; i++) {
        step_state(&st[0], in[i] & 15);
        step_state(&st[1], in[i] >> 4);
        pcm[2 * i] = (short)st[0].pred;
        pcm[2 * i + 1] = (short)st[1].pred;
    }
}
