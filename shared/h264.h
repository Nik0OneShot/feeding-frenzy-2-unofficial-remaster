// H.264 with Windows' own Media Foundation codecs (the software ones, which every Windows
// with media features has), set up for streaming: low latency, no B-frames, one frame out
// for each frame in. Call from a thread with COM initialized.
#pragma once
#include <windows.h>

typedef struct H264Enc H264Enc;
typedef struct H264Dec H264Dec;

// A coded frame (Annex B byte stream); key: decodable on its own.
typedef void (*H264Packet_t)(void *ctx, const BYTE *data, int len, int key);
// A decoded frame: NV12 with the luma plane `pitch` wide and `height` tall, chroma after it.
typedef void (*H264Picture_t)(void *ctx, const BYTE *nv12, int pitch, int height);

// width, height: multiples of 16. On failure returns NULL with the reason in err.
H264Enc *h264_enc_open(int width, int height, int fps, int kbps, char *err, int errlen);
int h264_enc_frame(H264Enc *e, const BYTE *nv12, int key, H264Packet_t out, void *ctx);
void h264_enc_close(H264Enc *e);

H264Dec *h264_dec_open(int width, int height, char *err, int errlen);
// Returns 0 if the decoder failed on this data (the next keyframe starts it over).
int h264_dec_frame(H264Dec *d, const BYTE *data, int len, H264Picture_t out, void *ctx);
void h264_dec_close(H264Dec *d);

// BT.601 limited range, both ways. The NV12 picture is `w` x `h`; a source smaller than that
// is padded with black.
void bgra_to_nv12(const BYTE *src, int src_pitch, int src_w, int src_h, BYTE *nv12, int w, int h);
void nv12_to_bgra(const BYTE *nv12, int pitch, int plane_h, BYTE *dst, int dst_pitch, int w, int h);
