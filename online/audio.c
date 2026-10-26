// The game's sound for the guest: Windows' process loopback (Windows 10 2004 and later)
// records what this process plays, nothing else on the PC, converted to 48 kHz 16-bit
// stereo; it goes out in 10 ms IMA ADPCM packets.
#define COBJMACROS
#include <winsock2.h>
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <string.h>
#include "../shared/adpcm.h"
#include "../shared/proto.h"
#include "host.h"

static const IID AUDIO_CLIENT = { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
static const IID CAPTURE_CLIENT = { 0xc8adbd64, 0xe71e, 0x48a0, { 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17 } };
static const IID COMPLETION_HANDLER = { 0x41d949ab, 0x9862, 0x444a, { 0x80, 0xf6, 0xc2, 0x61, 0x33, 0x4d, 0xa5, 0xeb } };
static const IID AGILE_OBJECT = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };
static const IID UNKNOWN = { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

// AUDIOCLIENT_ACTIVATION_PARAMS for process loopback (audioclientactivationparams.h).
typedef struct {
    int type;               // AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
    DWORD process;
    int mode;               // PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0
} LoopbackParams;

typedef HRESULT (WINAPI *Activate_t)(const WCHAR *path, REFIID riid, PROPVARIANT *params,
                                     IActivateAudioInterfaceCompletionHandler *handler,
                                     IActivateAudioInterfaceAsyncOperation **op);

// The completion handler: a static object that only signals an event.
static HANDLE activated;
static HRESULT STDMETHODCALLTYPE handler_query(IActivateAudioInterfaceCompletionHandler *self, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &UNKNOWN) || IsEqualIID(iid, &COMPLETION_HANDLER) || IsEqualIID(iid, &AGILE_OBJECT)) {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE handler_ref(IActivateAudioInterfaceCompletionHandler *self)
{
    (void)self;
    return 1;
}
static HRESULT STDMETHODCALLTYPE handler_done(IActivateAudioInterfaceCompletionHandler *self, IActivateAudioInterfaceAsyncOperation *op)
{
    (void)self;
    (void)op;
    SetEvent(activated);
    return S_OK;
}
static IActivateAudioInterfaceCompletionHandlerVtbl handler_vtbl = { handler_query, handler_ref, handler_ref, handler_done };
static IActivateAudioInterfaceCompletionHandler handler = { &handler_vtbl };

static IAudioClient *open_loopback(void)
{
    HMODULE m = LoadLibraryA("mmdevapi.dll");
    Activate_t activate = m ? (Activate_t)(void *)GetProcAddress(m, "ActivateAudioInterfaceAsync") : NULL;
    if (!activate) {
        host_log("no sound: this Windows can't record a single program's sound");
        return NULL;
    }
    LoopbackParams params = { 1, GetCurrentProcessId(), 0 };
    PROPVARIANT pv;
    memset(&pv, 0, sizeof pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof params;
    pv.blob.pBlobData = (BYTE *)&params;
    IActivateAudioInterfaceAsyncOperation *op = NULL;
    activated = CreateEventA(NULL, FALSE, FALSE, NULL);
    HRESULT hr = activate(L"VAD\\Process_Loopback", &AUDIO_CLIENT, &pv, &handler, &op);
    if (FAILED(hr) || WaitForSingleObject(activated, 5000) != WAIT_OBJECT_0) {
        host_log("no sound: recording the game failed (0x%08lx)", (unsigned long)hr);
        if (op) IActivateAudioInterfaceAsyncOperation_Release(op);
        return NULL;
    }
    HRESULT result = E_FAIL;
    IUnknown *unknown = NULL;
    IAudioClient *client = NULL;
    IActivateAudioInterfaceAsyncOperation_GetActivateResult(op, &result, &unknown);
    IActivateAudioInterfaceAsyncOperation_Release(op);
    if (FAILED(result) || !unknown) {
        host_log("no sound: recording the game failed (0x%08lx)", (unsigned long)result);
        return NULL;
    }
    IUnknown_QueryInterface(unknown, &AUDIO_CLIENT, (void **)&client);
    IUnknown_Release(unknown);
    return client;
}

static DWORD WINAPI audio_thread(void *arg)
{
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IAudioClient *client = open_loopback();
    if (!client) return 0;
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = AUDIO_RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = AUDIO_RATE * 4;
    HANDLE ready = CreateEventA(NULL, FALSE, FALSE, NULL);
    IAudioCaptureClient *capture = NULL;
    HRESULT hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
        200000, 0, &wf, NULL);
    if (SUCCEEDED(hr)) hr = IAudioClient_SetEventHandle(client, ready);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(client, &CAPTURE_CLIENT, (void **)&capture);
    if (SUCCEEDED(hr)) hr = IAudioClient_Start(client);
    if (FAILED(hr)) {
        host_log("no sound: starting the recording failed (0x%08lx)", (unsigned long)hr);
        return 0;
    }
    host_log("sound: recording the game");

    short block[AUDIO_BLOCK * 2];
    int have = 0, was_connected = 0;
    AdpcmState st[2] = { { 0, 0 }, { 0, 0 } };
    DWORD seq = 0;
    BYTE pkt[sizeof(PktAudio) + AUDIO_BLOCK];
    PktAudio *h = (PktAudio *)pkt;
    for (;;) {
        WaitForSingleObject(ready, 100);
        struct sockaddr_in to[ONLINE_MAX_GUESTS];
        int on = host_guests(to);
        if (on && !was_connected) have = 0;
        was_connected = on;
        UINT32 frames;
        while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(capture, &frames)) && frames) {
            BYTE *data;
            DWORD flags;
            if (FAILED(IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, NULL, NULL))) break;
            for (UINT32 i = 0; i < frames && on; i++) {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) block[2 * have] = block[2 * have + 1] = 0;
                else memcpy(&block[2 * have], data + i * 4, 4);
                if (++have == AUDIO_BLOCK) {
                    h->type = PKT_AUDIO;
                    h->frames = AUDIO_BLOCK;
                    h->seq = ++seq;
                    for (int c = 0; c < 2; c++) {
                        h->pred[c] = (short)st[c].pred;
                        h->index[c] = (BYTE)st[c].index;
                    }
                    adpcm_encode(st, block, AUDIO_BLOCK, pkt + sizeof *h);
                    for (int g = 0; g < on; g++) host_send(&to[g], pkt, sizeof pkt);
                    have = 0;
                }
            }
            IAudioCaptureClient_ReleaseBuffer(capture, frames);
        }
    }
    return 0;
}

void audio_start(void)
{
    CloseHandle(CreateThread(NULL, 0, audio_thread, NULL, 0, NULL));
}
