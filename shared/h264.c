#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <icodecapi.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include "h264.h"

// mfplat.dll is loaded at run time, so a Windows without media features gets a message
// instead of a program that doesn't start.
typedef HRESULT (WINAPI *MFStartup_t)(ULONG version, DWORD flags);
typedef HRESULT (WINAPI *MFCreateSample_t)(IMFSample **sample);
typedef HRESULT (WINAPI *MFCreateMemoryBuffer_t)(DWORD size, IMFMediaBuffer **buffer);
typedef HRESULT (WINAPI *MFCreateMediaType_t)(IMFMediaType **type);
static MFCreateSample_t create_sample;
static MFCreateMemoryBuffer_t create_buffer;
static MFCreateMediaType_t create_type;

#define MF_VERSION_WIN7 0x00020070

#define GUID_OF(l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
#define CODEC_GUID(name) GUID_OF(name)
static const GUID API_LOW_LATENCY   = CODEC_GUID(STATIC_CODECAPI_AVLowLatencyMode);
static const GUID API_RATE_CONTROL  = CODEC_GUID(STATIC_CODECAPI_AVEncCommonRateControlMode);
static const GUID API_MEAN_BIT_RATE = CODEC_GUID(STATIC_CODECAPI_AVEncCommonMeanBitRate);
static const GUID API_GOP_SIZE      = CODEC_GUID(STATIC_CODECAPI_AVEncMPVGOPSize);
static const GUID API_B_FRAMES      = CODEC_GUID(STATIC_CODECAPI_AVEncMPVDefaultBPictureCount);
static const GUID API_FORCE_KEY     = CODEC_GUID(STATIC_CODECAPI_AVEncVideoForceKeyFrame);

static int mf_load(char *err, int errlen)
{
    static int state; // 1 loaded, -1 failed
    if (!state) {
        HMODULE m = LoadLibraryA("mfplat.dll");
        MFStartup_t startup = m ? (MFStartup_t)(void *)GetProcAddress(m, "MFStartup") : NULL;
        create_sample = m ? (MFCreateSample_t)(void *)GetProcAddress(m, "MFCreateSample") : NULL;
        create_buffer = m ? (MFCreateMemoryBuffer_t)(void *)GetProcAddress(m, "MFCreateMemoryBuffer") : NULL;
        create_type = m ? (MFCreateMediaType_t)(void *)GetProcAddress(m, "MFCreateMediaType") : NULL;
        state = startup && create_sample && create_buffer && create_type
                && SUCCEEDED(startup(MF_VERSION_WIN7, 1 /* MFSTARTUP_LITE */)) ? 1 : -1;
    }
    if (state < 0) snprintf(err, errlen, "Media Foundation is missing (Windows N: install the Media Feature Pack)");
    return state > 0;
}

static IMFSample *new_sample(DWORD size)
{
    IMFSample *s;
    IMFMediaBuffer *b;
    if (FAILED(create_sample(&s))) return NULL;
    if (FAILED(create_buffer(size, &b))) {
        IMFSample_Release(s);
        return NULL;
    }
    IMFSample_AddBuffer(s, b);
    IMFMediaBuffer_Release(b);
    return s;
}

static IMFSample *sample_from(const BYTE *data, DWORD len, LONGLONG time, LONGLONG duration)
{
    IMFSample *s = new_sample(len);
    IMFMediaBuffer *b;
    BYTE *p;
    if (!s) return NULL;
    if (FAILED(IMFSample_GetBufferByIndex(s, 0, &b))) goto fail;
    if (FAILED(IMFMediaBuffer_Lock(b, &p, NULL, NULL))) {
        IMFMediaBuffer_Release(b);
        goto fail;
    }
    memcpy(p, data, len);
    IMFMediaBuffer_Unlock(b);
    IMFMediaBuffer_SetCurrentLength(b, len);
    IMFMediaBuffer_Release(b);
    IMFSample_SetSampleTime(s, time);
    IMFSample_SetSampleDuration(s, duration);
    return s;
fail:
    IMFSample_Release(s);
    return NULL;
}

static void set_video_type(IMFMediaType *t, const GUID *subtype, int w, int h, int fps)
{
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, subtype);
    IMFMediaType_SetUINT64(t, &MF_MT_FRAME_SIZE, (UINT64)w << 32 | (UINT32)h);
    IMFMediaType_SetUINT64(t, &MF_MT_FRAME_RATE, (UINT64)fps << 32 | 1);
    IMFMediaType_SetUINT64(t, &MF_MT_PIXEL_ASPECT_RATIO, (UINT64)1 << 32 | 1);
    IMFMediaType_SetUINT32(t, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
}

static void set_api(ICodecAPI *api, const GUID *what, VARTYPE vt, ULONG value)
{
    VARIANT v;
    VariantInit(&v);
    V_VT(&v) = vt;
    if (vt == VT_BOOL) V_BOOL(&v) = value ? VARIANT_TRUE : VARIANT_FALSE;
    else V_UI4(&v) = value;
    ICodecAPI_SetValue(api, what, &v);
}

// Takes every frame the transform has ready. Returns 0 on an error.
typedef void (*Output_t)(void *self, IMFSample *sample);
static int drain(IMFTransform *mft, DWORD out_size, int provides, Output_t out, void *self,
                 int (*stream_change)(void *self))
{
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob;
        DWORD status = 0;
        memset(&ob, 0, sizeof ob);
        if (!provides && !(ob.pSample = new_sample(out_size))) return 0;
        HRESULT hr = IMFTransform_ProcessOutput(mft, 0, 1, &ob, &status);
        if (ob.pEvents) IMFCollection_Release(ob.pEvents);
        if (SUCCEEDED(hr) && ob.pSample) out(self, ob.pSample);
        if (ob.pSample) IMFSample_Release(ob.pSample);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return 1;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE && stream_change && stream_change(self)) {
            // the output size may have changed with the type: re-read it
            MFT_OUTPUT_STREAM_INFO info;
            if (SUCCEEDED(IMFTransform_GetOutputStreamInfo(mft, 0, &info))) out_size = info.cbSize;
            continue;
        }
        if (FAILED(hr)) return 0;
    }
}

static int feed(IMFTransform *mft, IMFSample *s, DWORD out_size, int provides, Output_t out, void *self,
                int (*stream_change)(void *self))
{
    HRESULT hr = IMFTransform_ProcessInput(mft, 0, s, 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (!drain(mft, out_size, provides, out, self, stream_change)) return 0;
        hr = IMFTransform_ProcessInput(mft, 0, s, 0);
    }
    if (FAILED(hr)) return 0;
    return drain(mft, out_size, provides, out, self, stream_change);
}

// --- Encoder -------------------------------------------------------------------------

struct H264Enc {
    IMFTransform *mft;
    ICodecAPI *api;
    int w, h, fps, provides;
    DWORD out_size;
    LONGLONG frame;
    H264Packet_t cb;
    void *ctx;
};

static void enc_output(void *self, IMFSample *s)
{
    H264Enc *e = self;
    IMFMediaBuffer *b;
    BYTE *p;
    DWORD len;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &b))) return;
    if (SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, &len))) {
        UINT32 key = 0;
        IMFSample_GetUINT32(s, &MFSampleExtension_CleanPoint, &key);
        e->cb(e->ctx, p, (int)len, key != 0);
        IMFMediaBuffer_Unlock(b);
    }
    IMFMediaBuffer_Release(b);
}

H264Enc *h264_enc_open(int width, int height, int fps, int kbps, char *err, int errlen)
{
    if (!mf_load(err, errlen)) return NULL;
    H264Enc *e = calloc(1, sizeof *e);
    IMFMediaType *t = NULL;
    HRESULT hr;
    const char *step = "create";
    e->w = width;
    e->h = height;
    e->fps = fps;
    hr = CoCreateInstance(&CLSID_CMSH264EncoderMFT, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void **)&e->mft);
    if (FAILED(hr)) goto fail;
    step = "codec settings";
    hr = IMFTransform_QueryInterface(e->mft, &IID_ICodecAPI, (void **)&e->api);
    if (FAILED(hr)) goto fail;
    set_api(e->api, &API_LOW_LATENCY, VT_BOOL, 1);
    set_api(e->api, &API_RATE_CONTROL, VT_UI4, eAVEncCommonRateControlMode_CBR);
    set_api(e->api, &API_MEAN_BIT_RATE, VT_UI4, (ULONG)kbps * 1000);
    set_api(e->api, &API_B_FRAMES, VT_UI4, 0);
    set_api(e->api, &API_GOP_SIZE, VT_UI4, (ULONG)fps * 10); // a keyframe every 10 s at least

    step = "output type";
    if (FAILED(hr = create_type(&t))) goto fail;
    set_video_type(t, &MFVideoFormat_H264, width, height, fps);
    IMFMediaType_SetUINT32(t, &MF_MT_AVG_BITRATE, (UINT32)kbps * 1000);
    IMFMediaType_SetUINT32(t, &MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    hr = IMFTransform_SetOutputType(e->mft, 0, t, 0);
    IMFMediaType_Release(t);
    if (FAILED(hr)) goto fail;
    step = "input type";
    if (FAILED(hr = create_type(&t))) goto fail;
    set_video_type(t, &MFVideoFormat_NV12, width, height, fps);
    hr = IMFTransform_SetInputType(e->mft, 0, t, 0);
    IMFMediaType_Release(t);
    if (FAILED(hr)) goto fail;

    MFT_OUTPUT_STREAM_INFO info;
    step = "stream info";
    if (FAILED(hr = IMFTransform_GetOutputStreamInfo(e->mft, 0, &info))) goto fail;
    e->out_size = info.cbSize ? info.cbSize : (DWORD)(width * height * 2);
    e->provides = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return e;
fail:
    snprintf(err, errlen, "H.264 encoder: %s failed (0x%08lx)", step, (unsigned long)hr);
    h264_enc_close(e);
    return NULL;
}

int h264_enc_frame(H264Enc *e, const BYTE *nv12, int key, H264Packet_t out, void *ctx)
{
    if (key) set_api(e->api, &API_FORCE_KEY, VT_UI4, 1);
    LONGLONG duration = 10000000 / e->fps;
    IMFSample *s = sample_from(nv12, (DWORD)(e->w * e->h * 3 / 2), e->frame++ * duration, duration);
    if (!s) return 0;
    e->cb = out;
    e->ctx = ctx;
    int ok = feed(e->mft, s, e->out_size, e->provides, enc_output, e, NULL);
    IMFSample_Release(s);
    return ok;
}

void h264_enc_close(H264Enc *e)
{
    if (!e) return;
    if (e->api) ICodecAPI_Release(e->api);
    if (e->mft) IMFTransform_Release(e->mft);
    free(e);
}

// --- Decoder -------------------------------------------------------------------------

struct H264Dec {
    IMFTransform *mft;
    int pitch, plane_h, provides;
    DWORD out_size;
    LONGLONG frame;
    H264Picture_t cb;
    void *ctx;
};

static int dec_pick_output(void *self)
{
    H264Dec *d = self;
    for (DWORD i = 0;; i++) {
        IMFMediaType *t;
        GUID sub;
        if (FAILED(IMFTransform_GetOutputAvailableType(d->mft, 0, i, &t))) return 0;
        if (SUCCEEDED(IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub)) && IsEqualGUID(&sub, &MFVideoFormat_NV12)) {
            HRESULT hr = IMFTransform_SetOutputType(d->mft, 0, t, 0);
            UINT64 size = 0;
            UINT32 stride = 0;
            IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &size);
            d->pitch = (int)(size >> 32);
            d->plane_h = (int)(UINT32)size;
            if (SUCCEEDED(IMFMediaType_GetUINT32(t, &MF_MT_DEFAULT_STRIDE, &stride)) && (int)stride >= d->pitch)
                d->pitch = (int)stride;
            IMFMediaType_Release(t);
            MFT_OUTPUT_STREAM_INFO info;
            if (FAILED(hr) || FAILED(IMFTransform_GetOutputStreamInfo(d->mft, 0, &info))) return 0;
            d->out_size = info.cbSize;
            d->provides = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
            return 1;
        }
        IMFMediaType_Release(t);
    }
}

static void dec_output(void *self, IMFSample *s)
{
    H264Dec *d = self;
    IMFMediaBuffer *b;
    BYTE *p;
    DWORD len;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &b))) return;
    if (SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, &len))) {
        if (len >= (DWORD)(d->pitch * d->plane_h * 3 / 2)) d->cb(d->ctx, p, d->pitch, d->plane_h);
        IMFMediaBuffer_Unlock(b);
    }
    IMFMediaBuffer_Release(b);
}

H264Dec *h264_dec_open(int width, int height, char *err, int errlen)
{
    if (!mf_load(err, errlen)) return NULL;
    H264Dec *d = calloc(1, sizeof *d);
    IMFMediaType *t = NULL;
    IMFAttributes *attr = NULL;
    HRESULT hr;
    const char *step = "create";
    hr = CoCreateInstance(&CLSID_CMSH264DecoderMFT, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void **)&d->mft);
    if (FAILED(hr)) goto fail;
    // Without low latency the decoder holds frames back to reorder them.
    if (SUCCEEDED(IMFTransform_GetAttributes(d->mft, &attr))) {
        IMFAttributes_SetUINT32(attr, &API_LOW_LATENCY, TRUE);
        IMFAttributes_Release(attr);
    }
    step = "input type";
    if (FAILED(hr = create_type(&t))) goto fail;
    set_video_type(t, &MFVideoFormat_H264, width, height, 60);
    hr = IMFTransform_SetInputType(d->mft, 0, t, 0);
    IMFMediaType_Release(t);
    if (FAILED(hr)) goto fail;
    step = "output type";
    hr = E_FAIL;
    if (!dec_pick_output(d)) goto fail;
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return d;
fail:
    snprintf(err, errlen, "H.264 decoder: %s failed (0x%08lx)", step, (unsigned long)hr);
    h264_dec_close(d);
    return NULL;
}

int h264_dec_frame(H264Dec *d, const BYTE *data, int len, H264Picture_t out, void *ctx)
{
    IMFSample *s = sample_from(data, (DWORD)len, d->frame++ * 166667, 166667);
    if (!s) return 0;
    d->cb = out;
    d->ctx = ctx;
    int ok = feed(d->mft, s, d->out_size, d->provides, dec_output, d, dec_pick_output);
    IMFSample_Release(s);
    return ok;
}

void h264_dec_close(H264Dec *d)
{
    if (!d) return;
    if (d->mft) IMFTransform_Release(d->mft);
    free(d);
}

// --- Color conversion ------------------------------------------------------------------

void bgra_to_nv12(const BYTE *src, int src_pitch, int src_w, int src_h, BYTE *nv12, int w, int h)
{
    BYTE *uv = nv12 + w * h;
    memset(nv12, 16, (size_t)w * h);
    memset(uv, 128, (size_t)w * h / 2);
    for (int y = 0; y < src_h; y++) {
        const BYTE *s = src + (size_t)y * src_pitch;
        BYTE *d = nv12 + (size_t)y * w;
        for (int x = 0; x < src_w; x++, s += 4)
            d[x] = (BYTE)(((66 * s[2] + 129 * s[1] + 25 * s[0] + 128) >> 8) + 16);
    }
    for (int y = 0; y + 1 < src_h; y += 2) {
        const BYTE *s0 = src + (size_t)y * src_pitch, *s1 = s0 + src_pitch;
        BYTE *d = uv + (size_t)(y / 2) * w;
        for (int x = 0; x + 1 < src_w; x += 2, s0 += 8, s1 += 8) {
            int b = s0[0] + s0[4] + s1[0] + s1[4];
            int g = s0[1] + s0[5] + s1[1] + s1[5];
            int r = s0[2] + s0[6] + s1[2] + s1[6];
            d[x] = (BYTE)(((-38 * r - 74 * g + 112 * b + 512) >> 10) + 128);
            d[x + 1] = (BYTE)(((112 * r - 94 * g - 18 * b + 512) >> 10) + 128);
        }
    }
}

static BYTE clip(int v)
{
    return (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v);
}

void nv12_to_bgra(const BYTE *nv12, int pitch, int plane_h, BYTE *dst, int dst_pitch, int w, int h)
{
    const BYTE *uv_plane = nv12 + (size_t)pitch * plane_h;
    for (int y = 0; y < h; y++) {
        const BYTE *py = nv12 + (size_t)y * pitch, *puv = uv_plane + (size_t)(y / 2) * pitch;
        BYTE *d = dst + (size_t)y * dst_pitch;
        for (int x = 0; x < w; x++, d += 4) {
            int c = 298 * (py[x] - 16) + 128;
            int u = puv[x & ~1] - 128, v = puv[x | 1] - 128;
            d[0] = clip((c + 516 * u) >> 8);
            d[1] = clip((c - 100 * u - 208 * v) >> 8);
            d[2] = clip((c + 409 * v) >> 8);
            d[3] = 255;
        }
    }
}
