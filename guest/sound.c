// The host's game sound: decoded ADPCM blocks go into a buffer that a WASAPI shared-mode
// stream plays from, kept about 40-60 ms full: enough to ride out uneven packet arrival,
// little enough that sound stays in step with the picture.
#define COBJMACROS
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <string.h>
#include "../shared/proto.h"
#include "guest.h"

static const CLSID DEVICE_ENUMERATOR = { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const IID IMM_DEVICE_ENUMERATOR = { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const IID AUDIO_CLIENT = { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
static const IID RENDER_CLIENT = { 0xf294acfc, 0x3146, 0x4483, { 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2 } };

#define RING   (AUDIO_RATE)             // 1 s of stereo frames
#define START  (AUDIO_RATE * 40 / 1000) // playing starts (again) once this much is buffered
#define MOST   (AUDIO_RATE * 120 / 1000)// more than this: the extra is dropped
static short ring[RING * 2];
static int head, count, playing;
static CRITICAL_SECTION cs;
static DWORD last_seq;

void sound_push(const short *pcm, int frames, DWORD seq)
{
    EnterCriticalSection(&cs);
    if (last_seq && (int)(seq - last_seq) <= 0) {   // late or repeated
        LeaveCriticalSection(&cs);
        return;
    }
    last_seq = seq;
    for (int i = 0; i < frames && count < RING; i++, count++) {
        int at = (head + count) % RING;
        ring[2 * at] = pcm[2 * i];
        ring[2 * at + 1] = pcm[2 * i + 1];
    }
    if (count > MOST) {
        int drop = count - START;
        head = (head + drop) % RING;
        count -= drop;
    }
    LeaveCriticalSection(&cs);
}

static void fill(short *out, int frames)
{
    EnterCriticalSection(&cs);
    if (!playing && count >= START) playing = 1;
    for (int i = 0; i < frames; i++) {
        if (playing && count) {
            out[2 * i] = ring[2 * head];
            out[2 * i + 1] = ring[2 * head + 1];
            head = (head + 1) % RING;
            count--;
        } else {
            out[2 * i] = out[2 * i + 1] = 0;
            playing = 0;
        }
    }
    LeaveCriticalSection(&cs);
}

static DWORD WINAPI sound_thread(void *arg)
{
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *devices = NULL;
    IMMDevice *device = NULL;
    IAudioClient *client = NULL;
    IAudioRenderClient *render = NULL;
    HANDLE ready = CreateEventA(NULL, FALSE, FALSE, NULL);
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = AUDIO_RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = AUDIO_RATE * 4;
    UINT32 size = 0;
    HRESULT hr = CoCreateInstance(&DEVICE_ENUMERATOR, NULL, CLSCTX_ALL, &IMM_DEVICE_ENUMERATOR, (void **)&devices);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(devices, eRender, eConsole, &device);
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(device, &AUDIO_CLIENT, CLSCTX_ALL, NULL, (void **)&client);
    if (SUCCEEDED(hr)) hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        200000, 0, &wf, NULL);
    if (SUCCEEDED(hr)) hr = IAudioClient_SetEventHandle(client, ready);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetBufferSize(client, &size);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(client, &RENDER_CLIENT, (void **)&render);
    if (SUCCEEDED(hr)) hr = IAudioClient_Start(client);
    if (FAILED(hr)) {
        guest_log("no sound output (0x%08lx)", (unsigned long)hr);
        return 0;
    }
    for (;;) {
        WaitForSingleObject(ready, 100);
        UINT32 padding;
        BYTE *data;
        if (FAILED(IAudioClient_GetCurrentPadding(client, &padding))) continue;
        UINT32 frames = size - padding;
        if (frames && SUCCEEDED(IAudioRenderClient_GetBuffer(render, frames, &data))) {
            fill((short *)data, (int)frames);
            IAudioRenderClient_ReleaseBuffer(render, frames, 0);
        }
    }
    return 0;
}

void sound_start(void)
{
    InitializeCriticalSection(&cs);
    CloseHandle(CreateThread(NULL, 0, sound_thread, NULL, 0, NULL));
}

void sound_reset(void)
{
    EnterCriticalSection(&cs);
    last_seq = 0;
    count = 0;
    playing = 0;
    LeaveCriticalSection(&cs);
}
