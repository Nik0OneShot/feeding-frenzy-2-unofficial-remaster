// ff2guest.exe: plays player 2 in an online co-op game of Feeding Frenzy 2 hosted by
// someone with FF2 Remaster's online play switched on. Shows the host's game (H.264 over UDP),
// plays its sound (sound.c), and sends this PC's keyboard, gamepad and mouse as player 2.
// The game itself isn't needed here.
//
// Threads: the main one runs the window and draws with OpenGL; the receive thread puts
// video fragments back together, decodes them and hands each picture to the main thread;
// the input thread sends the input state every 8 ms; the sound thread plays.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <GL/gl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../shared/adpcm.h"
#include "../shared/h264.h"
#include "../shared/label.h"
#include "../shared/proto.h"
#include "guest.h"

#define GL_BGRA_EXT       0x80E1
#define GL_CLAMP_TO_EDGE  0x812F
#define WM_APP_FRAME      (WM_APP + 1)
#define TITLE             "FF2 Remaster guest"

static HWND wnd;
static HDC dc;
static volatile SOCKET sock;
static volatile SOCKET side_sock = INVALID_SOCKET;  // to the host's controls port (proto.h)
static struct sockaddr_in host_addr;
static volatile LONG ticket;             // from WELCOME, carried by every later packet
static char host_text[256];
static CRITICAL_SECTION cs, log_cs;
static FILE *log_file;

static volatile LONG welcomed;
static DWORD last_packet;
static char status[160];               // shown in the title while not playing
static int pic_w, pic_h, enc_w, enc_h;
static volatile LONG recv_bytes, shown_frames;
static volatile LONG ping_ms = -1;
static volatile LONG mouse_x, mouse_y;    // running totals while the mouse is taken
// For the log: totals since the start.
static volatile LONG total_packets, total_pictures, key_requests, inputs_sent, raw_events, pongs;

// --- Log -----------------------------------------------------------------------------

static void exe_dir_file(const char *name, char *out)
{
    char *slash = out;
    GetModuleFileNameA(NULL, out, MAX_PATH);
    for (char *p = out; *p; p++)
        if (*p == '\\') slash = p + 1;
    lstrcpyA(slash, name);
}

void guest_log(const char *fmt, ...)
{
    EnterCriticalSection(&log_cs);
    if (log_file) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(log_file, fmt, ap);
        va_end(ap);
        fputc('\n', log_file);
        fflush(log_file);
    }
    LeaveCriticalSection(&log_cs);
}

// A crash: what and where (module + offset), so it can be found in the build.
static LONG CALLBACK on_exception(PEXCEPTION_POINTERS info)
{
    static LONG reports;
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code < 0xC0000000 || code == 0xE06D7363 || InterlockedIncrement(&reports) > 4) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = info->ContextRecord;
    char module[MAX_PATH] = "?";
    HMODULE m = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)c->Eip, &m);
    if (m) GetModuleFileNameA(m, module, sizeof module);
    guest_log("CRASH %08lx at %08lx (%s +%lx), thread %lu", code, c->Eip, module, c->Eip - (DWORD)m, GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
        guest_log("  %s address %08lx", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                  (DWORD)info->ExceptionRecord->ExceptionInformation[1]);
    guest_log("  eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
              c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    HMODULE self = GetModuleHandleA(NULL);
    const DWORD *sp = (const DWORD *)c->Esp;
    for (int i = 0, found = 0; i < 1024 && found < 16 && !IsBadReadPtr(sp + i, 4); i++)
        if (sp[i] >= (DWORD)self + 0x1000 && sp[i] < (DWORD)self + 0x40000) {
            guest_log("  stack[%03x] ff2guest +%lx", i * 4, sp[i] - (DWORD)self);
            found++;
        }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void set_status(const char *text)
{
    EnterCriticalSection(&cs);
    snprintf(status, sizeof status, "%s", text);
    LeaveCriticalSection(&cs);
}

// Logs each kind of network error once.
static void log_net_error(const char *what, int err)
{
    static int seen[8], count;
    for (int i = 0; i < count; i++)
        if (seen[i] == err) return;
    if (count < 8) seen[count++] = err;
    guest_log("%s failed: network error %d", what, err);
}

static void send_packet(const void *data, int len)
{
    if (send(sock, data, len, 0) < 0) log_net_error("sending", WSAGetLastError());
}

// Input, keyframe requests, bye: also to the controls port, in case the main connection
// loses what comes back on it (proto.h).
static void send_control(const void *data, int len)
{
    send_packet(data, len);
    SOCKET s = side_sock;
    if (s != INVALID_SOCKET && send(s, data, len, 0) < 0) log_net_error("sending controls", WSAGetLastError());
}

// A UDP socket that only takes packets from the host (port: 0 = the main one).
static SOCKET open_socket(int port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL no = FALSE;
    DWORD got, timeout = 200;
    int buffer = 8 << 20;
    WSAIoctl(s, SIO_UDP_CONNRESET, &no, sizeof no, NULL, 0, &got, NULL, NULL);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&buffer, sizeof buffer);
    struct sockaddr_in to = host_addr;
    if (port) to.sin_port = htons((u_short)port);
    connect(s, (struct sockaddr *)&to, sizeof to);
    return s;
}

// --- Video: fragments -> frames -> pictures ----------------------------------------

#define SLOTS 16
typedef struct {
    DWORD frame;
    int count, got, key, size;          // count 0: free
    BYTE have[NET_MAX_FRAGS];
    BYTE *data;
} Slot;
static Slot slots[SLOTS];
static H264Dec *dec;
static DWORD last_done;                 // the last frame decoded
static volatile LONG waiting_key;       // a frame was lost: only a keyframe can follow
static volatile LONG last_key_request;

// Three pictures: the receive thread writes one, one waits to be drawn, one is drawn.
static BYTE *pictures[3];
static BYTE *writing, *waiting, *drawing;
// Each picture's "You!" label spot (PktVideo), swapped along with it.
static int mark_waiting[2], mark_drawing[2] = { -32768, 0 };
static int fresh;
static volatile LONG posted;

static void request_key(void)
{
    LONG now = (LONG)GetTickCount();
    if (now - last_key_request < 100) return;
    last_key_request = now;
    InterlockedIncrement(&key_requests);
    PktControl k = { PKT_KEYFRAME, (DWORD)ticket };
    send_control(&k, sizeof k);
}

static void swap_marks(int *a, int *b)
{
    int t[2] = { a[0], a[1] };
    a[0] = b[0], a[1] = b[1];
    b[0] = t[0], b[1] = t[1];
}

static void on_picture(void *ctx, const BYTE *nv12, int pitch, int plane_h)
{
    const int *mark = ctx;
    if (pitch < pic_w || plane_h < pic_h) return;
    nv12_to_bgra(nv12, pitch, plane_h, writing, pic_w * 4, pic_w, pic_h);
    EnterCriticalSection(&cs);
    BYTE *t = waiting;
    waiting = writing;
    writing = t;
    mark_waiting[0] = mark[0];
    mark_waiting[1] = mark[1];
    fresh = 1;
    LeaveCriticalSection(&cs);
    InterlockedIncrement(&shown_frames);
    InterlockedIncrement(&total_pictures);
    if (!InterlockedExchange(&posted, 1)) PostMessageA(wnd, WM_APP_FRAME, 0, 0);
}

static void on_welcome(const PktWelcome *w)
{
    ticket = (LONG)w->ticket;
    if (w->input_port && side_sock == INVALID_SOCKET) side_sock = open_socket(w->input_port);
    char err[160];
    if (!dec || w->enc_width != enc_w || w->enc_height != enc_h || w->width != pic_w || w->height != pic_h) {
        h264_dec_close(dec);
        enc_w = w->enc_width;
        enc_h = w->enc_height;
        dec = h264_dec_open(enc_w, enc_h, err, sizeof err);
        if (!dec) {
            guest_log("%s", err);
            set_status(err);
            return;
        }
        EnterCriticalSection(&cs);
        pic_w = w->width;
        pic_h = w->height;
        // the old pictures are kept (not freed): the main thread may still be drawing one
        for (int i = 0; i < 3; i++) pictures[i] = calloc((size_t)pic_w * pic_h, 4);
        writing = pictures[0];
        waiting = pictures[1];
        drawing = pictures[2];
        fresh = 0;
        LeaveCriticalSection(&cs);
    }
    for (int i = 0; i < SLOTS; i++) slots[i].count = 0;
    last_done = 0;
    waiting_key = 1;
    sound_reset();
    mouse_x = mouse_y = 0;
    guest_log("connected: picture %dx%d, %d fps", w->width, w->height, w->fps);
    welcomed = 1;
}

static void on_fragment(const BYTE *buf, int n)
{
    const PktVideo *h = (const PktVideo *)buf;
    int part = n - (int)sizeof *h;
    if (part <= 0 || part > NET_FRAG || !h->count || h->count > NET_MAX_FRAGS || h->frag >= h->count) return;
    if (last_done && (int)(h->frame - last_done) <= 0) return;
    Slot *s = &slots[h->frame % SLOTS];
    if (!s->count || s->frame != h->frame) {
        s->frame = h->frame;
        s->count = h->count;
        s->got = 0;
        s->key = h->key;
        s->size = 0;
        memset(s->have, 0, s->count);
    }
    if (h->count != s->count || s->have[h->frag]) return;
    s->have[h->frag] = 1;
    s->got++;
    memcpy(s->data + h->frag * NET_FRAG, buf + sizeof *h, part);
    if (h->frag == s->count - 1) s->size = h->frag * NET_FRAG + part;
    if (s->got < s->count) return;

    s->count = 0;
    if (!s->key && (waiting_key || (last_done && s->frame != last_done + 1))) {
        if (!waiting_key) guest_log("frame %lu lost on the way; asking for a keyframe", (unsigned long)(last_done + 1));
        waiting_key = 1;
        request_key();
        return;
    }
    last_done = s->frame;
    waiting_key = 0;
    int mark[2] = { h->mark_x, h->mark_y };
    if (!h264_dec_frame(dec, s->data, s->size, on_picture, mark)) {
        guest_log("decoding frame %lu failed", (unsigned long)s->frame);
        waiting_key = 1;
        request_key();
    }
}

static void on_audio(const BYTE *buf, int n)
{
    const PktAudio *h = (const PktAudio *)buf;
    if (n < (int)sizeof *h || h->frames > AUDIO_BLOCK || n < (int)sizeof *h + h->frames) return;
    short pcm[AUDIO_BLOCK * 2];
    AdpcmState st[2];
    for (int c = 0; c < 2; c++) {
        st[c].pred = h->pred[c];
        st[c].index = h->index[c] > 88 ? 88 : h->index[c];
    }
    adpcm_decode(st, buf + sizeof *h, h->frames, pcm);
    sound_push(pcm, h->frames, h->seq);
}

static DWORD WINAPI recv_thread(void *arg)
{
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (int i = 0; i < SLOTS; i++) slots[i].data = malloc((size_t)NET_MAX_FRAGS * NET_FRAG);
    static BYTE buf[2048];
    for (;;) {
        int n = recv(sock, (char *)buf, sizeof buf, 0);
        DWORD now = GetTickCount();
        if (n < 0 && WSAGetLastError() != WSAETIMEDOUT) log_net_error("receiving", WSAGetLastError());
        if (n <= 0) {
            if (welcomed && now - last_packet > 3000) {
                welcomed = 0;
                guest_log("the host stopped sending; reconnecting from a new port");
                SOCKET old = sock, old_side = side_sock;
                sock = open_socket(0);
                side_sock = INVALID_SOCKET;
                closesocket(old);
                if (old_side != INVALID_SOCKET) closesocket(old_side);
                set_status("lost the connection; trying again...");
            }
            continue;
        }
        last_packet = now;
        InterlockedExchangeAdd(&recv_bytes, n);
        InterlockedIncrement(&total_packets);
        switch (buf[0]) {
        case PKT_WELCOME:
            if (!welcomed && n >= (int)sizeof(PktWelcome)) on_welcome((const PktWelcome *)buf);
            break;
        case PKT_REJECT:
            if (n >= (int)sizeof(PktReject) && !welcomed) {
                const char *why = buf[1] == REJECT_VERSION ? "the host has a different FF2 Remaster version"
                                                           : "the host's game is full";
                set_status(why);
            }
            break;
        case PKT_PONG:
            if (n >= (int)sizeof(PktPong)) {
                ping_ms = (LONG)(now - ((const PktPong *)buf)->time);
                if (InterlockedIncrement(&pongs) == 1) guest_log("the host gets our controls (ping %ld ms)", ping_ms);
            }
            break;
        case PKT_VIDEO:
            if (welcomed && n > (int)sizeof(PktVideo)) on_fragment(buf, n);
            break;
        case PKT_AUDIO:
            if (welcomed) on_audio(buf, n);
            break;
        }
    }
    return 0;
}

// --- Input ---------------------------------------------------------------------------

typedef struct { WORD buttons; BYTE lt, rt; short lx, ly, rx, ry; } PadState;
typedef DWORD (WINAPI *XInputGetState_t)(DWORD pad, void *state);
static XInputGetState_t xinput_get_state;
static volatile LONG captured;
static int ignore_left;                 // the click that took the mouse doesn't dash

static int key(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Same controls as player 2 on the host's PC (the patch's coop.c).
static void read_input(PktInput *in)
{
    static int pad = -1;
    static DWORD pad_scan;
    float x = 0, y = 0;
    int buttons = 0;
    int focused = GetForegroundWindow() == wnd;
    if (xinput_get_state && focused) {
        struct { DWORD packet; PadState pad; } state;
        // Asking for pads that aren't there is slow: look for one once a second.
        if (pad < 0 && GetTickCount() - pad_scan > 1000) {
            pad_scan = GetTickCount();
            for (int i = 0; i < 4 && pad < 0; i++)
                if (xinput_get_state(i, &state) == ERROR_SUCCESS) pad = i;
        }
        if (pad >= 0) {
            static int logged_pad = -1;
            if (pad != logged_pad) {
                guest_log("gamepad %d in use", pad);
                logged_pad = pad;
            }
            if (xinput_get_state(pad, &state) != ERROR_SUCCESS) pad = -1;
            else {
                float lx = state.pad.lx / 32767.0f, ly = -state.pad.ly / 32767.0f;
                float len = sqrtf(lx * lx + ly * ly);
                const float dead = 0.24f;
                if (len > dead) {
                    float s = (len > 1 ? 1 : len - dead) / (1 - dead) / len;
                    x = lx * s;
                    y = ly * s;
                }
                if (state.pad.buttons & 0x1000) buttons |= NET_DASH;    // A
                if (state.pad.buttons & 0x2000) buttons |= NET_SPECIAL; // B
            }
        }
    }
    if (focused) {
        if (key('A') || key(VK_LEFT)) x -= 1;
        if (key('D') || key(VK_RIGHT)) x += 1;
        if (key('W') || key(VK_UP)) y -= 1;
        if (key('S') || key(VK_DOWN)) y += 1;
        if (key(VK_SPACE) || key(VK_RCONTROL)) buttons |= NET_DASH;
        if (key('E') || key(VK_RETURN) || key(VK_LSHIFT)) buttons |= NET_SPECIAL;
        if (captured) {
            if (!key(VK_LBUTTON)) ignore_left = 0;
            if (key(VK_LBUTTON) && !ignore_left) buttons |= NET_DASH;
            if (key(VK_RBUTTON)) buttons |= NET_SPECIAL;
        }
    }
    float len = sqrtf(x * x + y * y);
    if (len > 1) x /= len, y /= len;
    in->x = (short)(x * 32767);
    in->y = (short)(y * 32767);
    in->buttons = (BYTE)buttons;
    in->mouse_x = mouse_x;
    in->mouse_y = mouse_y;
}

static DWORD WINAPI input_thread(void *arg)
{
    (void)arg;
    DWORD seq = 0, last_hello = 0, last_stats = GetTickCount(), last_sent = 0;
    PktInput sent;
    memset(&sent, 0, sizeof sent);
    for (;;) {
        Sleep(8);
        DWORD now = GetTickCount();
        if (now - last_stats >= 5000) {
            last_stats = now;
            guest_log("status: %s, %ld packets in, %ld pictures, %ld inputs out, %ld answers, %ld keyframe requests%s, "
                      "ping %ld ms, window %s, mouse %s (%ld moves)", welcomed ? "connected" : "not connected",
                      total_packets, total_pictures, inputs_sent, pongs, key_requests,
                      waiting_key ? " (waiting for one)" : "", ping_ms,
                      GetForegroundWindow() == wnd ? "in front" : "behind", captured ? "taken" : "free", raw_events);
        }
        if (!welcomed) {
            if (now - last_hello >= 500) {
                // with the last ticket, to get our own place (and fish) back after a reconnect
                PktHello h = { PKT_HELLO, 0, NET_VERSION, NET_MAGIC, (DWORD)ticket };
                send_packet(&h, sizeof h);
                last_hello = now;
            }
            continue;
        }
        if (waiting_key && (LONG)now - last_key_request >= 250) request_key();
        PktInput in;
        memset(&in, 0, sizeof in);
        in.type = PKT_INPUT;
        read_input(&in);
        int changed = in.x != sent.x || in.y != sent.y || in.buttons != sent.buttons || in.mouse_x != sent.mouse_x
                      || in.mouse_y != sent.mouse_y;
        if (now - last_sent < (changed ? 16u : 250u)) continue;
        in.seq = ++seq;
        in.time = now;
        in.ticket = (DWORD)ticket;
        send_control(&in, sizeof in);
        sent = in;
        last_sent = now;
        InterlockedIncrement(&inputs_sent);
    }
    return 0;
}

static void capture_mouse(void)
{
    RECT r;
    GetClientRect(wnd, &r);
    MapWindowPoints(wnd, NULL, (POINT *)&r, 2);
    ClipCursor(&r);
    if (!captured) {
        captured = 1;
        ignore_left = 1;
        SetCursor(NULL);
    }
}

static void release_mouse(void)
{
    if (!captured) return;
    captured = 0;
    ClipCursor(NULL);
    SetCursor(LoadCursor(NULL, IDC_ARROW));
}

// --- Window --------------------------------------------------------------------------

static GLuint texture;
static int tex_w, tex_h;

// The "You!" label over player 2: made at twice the host's size (sharp when the picture is
// enlarged), drawn at the host's size times the picture's scale, its arrow at the spot.
static GLuint you_texture;
static Label you;

static void draw_you(const int *mark, float scale, int cw, int ch)
{
    if (!you_texture) {
        if (!label_make(&you, "You!", 36)) return;
        glGenTextures(1, &you_texture);
        glBindTexture(GL_TEXTURE_2D, you_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, you.w, you.h, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, you.bgra);
    }
    float ox = (cw - tex_w * scale) / 2, oy = (ch - tex_h * scale) / 2;
    float x = ox + mark[0] * scale, y = oy + mark[1] * scale; // window pixels
    float w = you.w * scale / 2, h = you.h * scale / 2;
    float l = (x - w / 2) / cw * 2 - 1, r = (x + w / 2) / cw * 2 - 1;
    float t = 1 - (y - h) / ch * 2, b = 1 - y / ch * 2;
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied
    glBindTexture(GL_TEXTURE_2D, you_texture);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 1); glVertex2f(l, b);
    glTexCoord2f(1, 1); glVertex2f(r, b);
    glTexCoord2f(1, 0); glVertex2f(r, t);
    glTexCoord2f(0, 0); glVertex2f(l, t);
    glEnd();
    glDisable(GL_BLEND);
}

static void render(void)
{
    RECT r;
    GetClientRect(wnd, &r);
    int cw = r.right, ch = r.bottom;
    glViewport(0, 0, cw, ch);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    EnterCriticalSection(&cs);
    int w = pic_w, h = pic_h, upload = fresh;
    if (fresh) {
        BYTE *t = drawing;
        drawing = waiting;
        waiting = t;
        swap_marks(mark_drawing, mark_waiting);
        fresh = 0;
    }
    LeaveCriticalSection(&cs);
    if (upload) {
        if (!texture) glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        if (tex_w != w || tex_h != h) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, drawing);
            tex_w = w;
            tex_h = h;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, drawing);
        }
    }
    if (tex_w && cw > 0 && ch > 0) {
        // the whole picture, as large as fits, black bars on the other sides
        float scale = fminf((float)cw / tex_w, (float)ch / tex_h);
        float sx = tex_w * scale / cw, sy = tex_h * scale / ch;
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texture);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 1); glVertex2f(-sx, -sy);
        glTexCoord2f(1, 1); glVertex2f(sx, -sy);
        glTexCoord2f(1, 0); glVertex2f(sx, sy);
        glTexCoord2f(0, 0); glVertex2f(-sx, sy);
        glEnd();
        if (mark_drawing[0] != -32768) draw_you(mark_drawing, scale, cw, ch);
    }
    SwapBuffers(dc);
}

static void update_title(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    float seconds = last ? (now - last) / 1000.0f : 1;
    last = now;
    LONG bytes = InterlockedExchange(&recv_bytes, 0), frames = InterlockedExchange(&shown_frames, 0);
    char title[600];
    if (welcomed) {
        char ping[32] = "";
        if (ping_ms >= 0) snprintf(ping, sizeof ping, ", ping %ld ms", ping_ms);
        snprintf(title, sizeof title, "%s - %s - %.0f fps, %.1f Mbit/s%s - %s", TITLE, host_text, frames / seconds,
                 bytes * 8 / seconds / 1e6, ping,
                 captured ? "Esc frees the mouse" : "click the picture to steer with the mouse; F11: full screen");
    } else {
        EnterCriticalSection(&cs);
        snprintf(title, sizeof title, "%s - %s: %s", TITLE, host_text, status);
        LeaveCriticalSection(&cs);
    }
    SetWindowTextA(wnd, title);
}

static void toggle_fullscreen(void)
{
    static WINDOWPLACEMENT placement;
    DWORD style = GetWindowLongA(wnd, GWL_STYLE);
    placement.length = sizeof placement;
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (GetWindowPlacement(wnd, &placement) && GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongA(wnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        SetWindowLongA(wnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(wnd, &placement);
        SetWindowPos(wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    if (captured) capture_mouse();
}

static LRESULT CALLBACK window_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_APP_FRAME:
        posted = 0;
        render();
        return 0;
    case WM_PAINT:
        ValidateRect(w, NULL);
        render();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (captured) capture_mouse();
        return 0;
    case WM_TIMER:
        update_title();
        return 0;
    case WM_LBUTTONDOWN:
        if (welcomed && !captured) guest_log("mouse taken");
        if (welcomed) capture_mouse();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && captured) guest_log("mouse freed");
        if (wp == VK_ESCAPE) release_mouse();
        if (wp == VK_F11) toggle_fullscreen();
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {
            toggle_fullscreen();
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0; // no beep after Alt+Enter
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) release_mouse();
        break;
    case WM_KILLFOCUS:
        release_mouse();
        break;
    case WM_SETCURSOR:
        if (captured && LOWORD(lp) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_INPUT: {
        RAWINPUT raw;
        UINT size = sizeof raw;
        if (captured && GetRawInputData((HRAWINPUT)lp, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1
            && raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            InterlockedIncrement(&raw_events);
            InterlockedExchangeAdd(&mouse_x, raw.data.mouse.lLastX);
            InterlockedExchangeAdd(&mouse_y, raw.data.mouse.lLastY);
        }
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

// --- Asking for the host's address ---------------------------------------------------

#define ID_ADDRESS 100
static int prompt_result;

static LRESULT CALLBACK prompt_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        if (LOWORD(wp) == IDOK) GetDlgItemTextA(w, ID_ADDRESS, host_text, sizeof host_text);
        prompt_result = LOWORD(wp) == IDOK ? 1 : -1;
        DestroyWindow(w);
        return 0;
    }
    if (msg == WM_CLOSE) {
        prompt_result = -1;
        DestroyWindow(w);
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static int ask_address(HINSTANCE inst)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = prompt_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = "FF2GuestPrompt";
    RegisterClassA(&wc);
    NONCLIENTMETRICSA ncm;
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    HFONT font = CreateFontIndirectA(&ncm.lfMessageFont);
    int dpi = GetDeviceCaps(GetDC(NULL), LOGPIXELSX);
#define S(v) MulDiv(v, dpi, 96)
    RECT r = { 0, 0, S(440), S(176) };
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND w = CreateWindowExA(WS_EX_DLGMODALFRAME, wc.lpszClassName, TITLE, WS_CAPTION | WS_SYSMENU,
                             (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2,
                             ww, wh, NULL, NULL, inst, NULL);
    struct { const char *cls, *text; DWORD style, ex; int id, x, y, w, h; } controls[] = {
        { "STATIC", "Host's address (from the host's ff2wide.log), as IP or IP:port:", 0, 0, -1, 16, 14, 408, 20 },
        { "EDIT", host_text, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, ID_ADDRESS, 16, 38, 408, 24 },
        { "STATIC", "You play the second fish: WASD or arrows, Space to dash, E for the special, "
                    "or a gamepad; click the picture to steer with the mouse.", 0, 0, -1, 16, 72, 408, 40 },
        { "BUTTON", "Connect", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, IDOK, 240, 130, 88, 28 },
        { "BUTTON", "Cancel", WS_TABSTOP, 0, IDCANCEL, 336, 130, 88, 28 },
    };
    for (int i = 0; i < (int)(sizeof controls / sizeof *controls); i++) {
        HWND c = CreateWindowExA(controls[i].ex, controls[i].cls, controls[i].text, WS_CHILD | WS_VISIBLE | controls[i].style,
                                 S(controls[i].x), S(controls[i].y), S(controls[i].w), S(controls[i].h), w,
                                 (HMENU)(INT_PTR)controls[i].id, inst, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)font, TRUE);
    }
#undef S
    ShowWindow(w, SW_SHOW);
    HWND edit = GetDlgItem(w, ID_ADDRESS);
    SetFocus(edit);
    SendMessageA(edit, EM_SETSEL, 0, -1);
    prompt_result = 0;
    MSG msg;
    while (!prompt_result && GetMessageA(&msg, NULL, 0, 0) > 0)
        if (!IsDialogMessageA(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    DeleteObject(font);
    return prompt_result > 0;
}

// "1.2.3.4", "1.2.3.4:47600" or "name:port" -> address.
static int resolve(const char *text, struct sockaddr_in *out)
{
    char host[256], port[16];
    snprintf(host, sizeof host, "%s", text);
    snprintf(port, sizeof port, "%d", NET_PORT);
    char *colon = strrchr(host, ':');
    if (colon && colon == strchr(host, ':')) {
        snprintf(port, sizeof port, "%s", colon + 1);
        *colon = 0;
    }
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res) || !res) return 0;
    *out = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    return 1;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev;
    char path[MAX_PATH], ini[MAX_PATH];
    InitializeCriticalSection(&cs);
    InitializeCriticalSection(&log_cs);
    SetProcessDPIAware();
    exe_dir_file("ff2guest.log", path);
    log_file = fopen(path, "w");
    AddVectoredExceptionHandler(1, on_exception);
    exe_dir_file("ff2guest.ini", ini);

    while (*cmd == ' ') cmd++;
    if (*cmd) snprintf(host_text, sizeof host_text, "%s", cmd);
    else {
        GetPrivateProfileStringA("guest", "address", "", host_text, sizeof host_text, ini);
        if (!ask_address(inst)) return 0;
    }
    for (char *p = host_text + strlen(host_text); p > host_text && (p[-1] == ' ' || p[-1] == '"'); ) *--p = 0;
    WritePrivateProfileStringA("guest", "address", host_text, ini);

    WSADATA wsa;
    struct sockaddr_in host;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    if (!resolve(host_text, &host)) {
        char text[400];
        snprintf(text, sizeof text, "Couldn't find the host \"%s\".", host_text);
        MessageBoxA(NULL, text, TITLE, MB_ICONERROR);
        return 1;
    }
    char ip[64];
    inet_ntop(AF_INET, &host.sin_addr, ip, sizeof ip);
    guest_log("connecting to %s (%s:%d)", host_text, ip, ntohs(host.sin_port));
    host_addr = host;
    sock = open_socket(0);
    set_status("connecting...");

    HMODULE xinput = LoadLibraryA("xinput1_4.dll");
    if (!xinput) xinput = LoadLibraryA("xinput9_1_0.dll");
    if (xinput) xinput_get_state = (XInputGetState_t)(void *)GetProcAddress(xinput, "XInputGetState");

    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = "FF2Guest";
    RegisterClassA(&wc);
    RECT work;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    int ww = (work.right - work.left) * 3 / 4, wh = ww * 9 / 16;
    if (wh > (work.bottom - work.top) * 9 / 10) wh = (work.bottom - work.top) * 9 / 10;
    wnd = CreateWindowExA(0, wc.lpszClassName, TITLE, WS_OVERLAPPEDWINDOW, work.left + (work.right - work.left - ww) / 2,
                          work.top + (work.bottom - work.top - wh) / 2, ww, wh, NULL, NULL, inst, NULL);
    dc = GetDC(wnd);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    wglMakeCurrent(dc, wglCreateContext(dc));
    typedef BOOL (WINAPI *SwapInterval_t)(int);
    SwapInterval_t swap_interval = (SwapInterval_t)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap_interval) swap_interval(1);
    RAWINPUTDEVICE mouse = { 1, 2, 0, wnd };
    RegisterRawInputDevices(&mouse, 1, sizeof mouse);
    ShowWindow(wnd, show);
    SetTimer(wnd, 1, 1000, NULL);
    update_title();

    timeBeginPeriod(1);
    sound_start();
    CloseHandle(CreateThread(NULL, 0, recv_thread, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, input_thread, NULL, 0, NULL));
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    PktControl bye = { PKT_BYE, (DWORD)ticket };
    for (int i = 0; i < 3; i++) send_control(&bye, sizeof bye);
    guest_log("closed");
    return 0;
}
