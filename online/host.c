// ff2online.dll: the online co-op host, loaded into the game by the patch when online=1.
// Guests (ff2guest.exe, up to ONLINE_MAX_GUESTS at once) connect over UDP; each gets the
// game's picture as H.264 video (one encoding, sent to all) and its sound (audio.c), and each
// one's input comes back as a player of its own (the patch's coop.c).
//
// Threads: the network thread answers the guest's packets; the game thread hands over a
// frame when one is due (online_wants_frame / online_video), which the encoder thread
// converts, encodes and sends; the audio thread sends the game's sound.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../shared/h264.h"
#include "../shared/online_api.h"
#include "../shared/proto.h"
#include "host.h"

static OnlineConfig cfg;
static SOCKET sock = INVALID_SOCKET;
static SOCKET in_sock = INVALID_SOCKET; // the controls port: receive only (proto.h)
static CRITICAL_SECTION cs;
static volatile LONG want_key;

// A guest's place. It keeps its ticket after the guest is gone, so a guest coming back with
// it (HELLO) takes the same place, and the patch gives it the same fish.
typedef struct {
    int connected;
    struct sockaddr_in addr;
    DWORD ticket;
    DWORD last_heard, input_seq, gone_at;
    OnlineInput input;
    int got_main, got_side;          // this connection's inputs, for the log
} Guest;
static Guest guests[ONLINE_MAX_GUESTS];
static int connected_count;
static int open = 1;                 // taking guests (online_set_open)
static char public_address[64];      // the router's internet address, once UPnP has it

static int enc_w, enc_h;
static BYTE *frame_bgra;            // the latest frame, handed from the game thread
static int frame_marks[ONLINE_MAX_GUESTS][2]; // and each guest's label spot (PktVideo)
static HANDLE frame_event;
static volatile LONG frame_busy;    // set by online_video, cleared once converted
static volatile LONG encoder_dead;
static LONGLONG next_due, period;
static DWORD frame_id;
static volatile LONG sent_bytes, sent_frames;
static LONGLONG tick_freq;

static LONGLONG now_ticks(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void host_log(const char *fmt, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(text, fmt, ap);
    va_end(ap);
    if (cfg.log) cfg.log("online: %s", text);
}

int host_guests(struct sockaddr_in *to)
{
    int n = 0;
    EnterCriticalSection(&cs);
    for (int i = 0; i < ONLINE_MAX_GUESTS; i++)
        if (guests[i].connected) to[n++] = guests[i].addr;
    LeaveCriticalSection(&cs);
    return n;
}

void host_send(const struct sockaddr_in *to, const void *data, int len)
{
    if (sendto(sock, data, len, 0, (const struct sockaddr *)to, sizeof *to) > 0)
        InterlockedExchangeAdd(&sent_bytes, len);
}

static void address_text(const struct sockaddr_in *a, char *out, int len)
{
    char ip[64] = "?";
    inet_ntop(AF_INET, (void *)&a->sin_addr, ip, sizeof ip);
    wsprintfA(out, "%s:%d", ip, ntohs(a->sin_port));
    (void)len;
}

// --- Network thread ------------------------------------------------------------------

static int ignored;                  // packets ignored lately, for the log

static void disconnect(Guest *g, const char *why)
{
    char addr[80];
    address_text(&g->addr, addr, sizeof addr);
    host_log("guest %d (%s) %s (inputs received: %d on the main connection, %d on the controls port)",
             (int)(g - guests) + 1, addr, why, g->got_main, g->got_side);
    g->connected = 0;
    g->gone_at = GetTickCount();
    connected_count--;
    memset(&g->input, 0, sizeof g->input);
}

static void log_ignored(const BYTE *buf, const struct sockaddr_in *from, const char *why)
{
    char addr[80];
    if (ignored++ >= 5) return;
    address_text(from, addr, sizeof addr);
    host_log("ignored a packet (type %d) from %s: %s", buf[0], addr, why);
}

static int same_address(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static Guest *by_ticket(DWORD ticket)
{
    for (int i = 0; ticket && i < ONLINE_MAX_GUESTS; i++)
        if (guests[i].connected && guests[i].ticket == ticket) return &guests[i];
    return NULL;
}

// A packet with the right ticket: the guest is wherever it came from.
static void heard(Guest *g, const struct sockaddr_in *from, DWORD now)
{
    g->last_heard = now;
    if (same_address(from, &g->addr)) return;
    char was[80], is[80];
    address_text(&g->addr, was, sizeof was);
    address_text(from, is, sizeof is);
    host_log("guest %d's address changed from %s to %s", (int)(g - guests) + 1, was, is);
    g->addr = *from;
}

// Where a HELLO goes: back to its own place (its ticket, or the same address still
// connected), else a free one, the longest gone first and never-used ones before that.
static Guest *place_for(const PktHello *h, const struct sockaddr_in *from)
{
    for (int i = 0; h->ticket && i < ONLINE_MAX_GUESTS; i++)
        if (guests[i].ticket == h->ticket) return &guests[i];
    for (int i = 0; i < ONLINE_MAX_GUESTS; i++)
        if (guests[i].connected && same_address(from, &guests[i].addr)) return &guests[i];
    Guest *best = NULL;
    for (int i = 0; i < ONLINE_MAX_GUESTS; i++) {
        Guest *g = &guests[i];
        if (g->connected) continue;
        if (!best || (!g->ticket && best->ticket) ||
            (!g->ticket == !best->ticket && GetTickCount() - g->gone_at > GetTickCount() - best->gone_at))
            best = g;
    }
    return best;
}

// side: the packet came in on the controls port. Packets there only prove the guest is
// still there; its address for the stream is learned from the main connection.
static void handle(const BYTE *buf, int n, const struct sockaddr_in *from, int side)
{
    DWORD now = GetTickCount();
    EnterCriticalSection(&cs);
    switch (buf[0]) {
    case PKT_HELLO: {
        const PktHello *h = (const PktHello *)buf;
        if (side || n < 8 || h->magic != NET_MAGIC) break;
        Guest *g = open && h->version == NET_VERSION && n >= (int)sizeof *h ? place_for(h, from) : NULL;
        if (!g) {
            PktReject r = { PKT_REJECT, h->version != NET_VERSION ? REJECT_VERSION : !open ? REJECT_CLOSED : REJECT_BUSY };
            LeaveCriticalSection(&cs);
            if (h->version != NET_VERSION) log_ignored(buf, from, "the guest's viewer is a different version");
            else log_ignored(buf, from, open ? "all the places are taken" : "online play is switched off");
            host_send(from, &r, sizeof r);
            return;
        }
        if (!g->connected || !same_address(from, &g->addr)) {
            char addr[80];
            int back = h->ticket && g->ticket == h->ticket;
            if (!g->connected) connected_count++;
            g->connected = 1;
            g->addr = *from;
            g->input_seq = 0;
            g->got_main = g->got_side = 0;
            ignored = 0;
            if (!back) {
                g->ticket = (GetTickCount() * 2654435761u) ^ (DWORD)now_ticks() ^ GetCurrentProcessId() ^ (DWORD)(g - guests);
                if (!g->ticket) g->ticket = 1;
            }
            memset(&g->input, 0, sizeof g->input);
            address_text(from, addr, sizeof addr);
            host_log(back ? "guest %d reconnected from %s" : "guest %d connected from %s", (int)(g - guests) + 1, addr);
        }
        g->last_heard = now;
        want_key = 1;
        PktWelcome w = { PKT_WELCOME, (BYTE)cfg.fps, (WORD)cfg.width, (WORD)cfg.height, (WORD)enc_w, (WORD)enc_h, g->ticket,
                         (WORD)(in_sock != INVALID_SOCKET ? cfg.port + 1 : 0) };
        LeaveCriticalSection(&cs);
        host_send(from, &w, sizeof w);
        return;
    }
    case PKT_INPUT: {
        const PktInput *in = (const PktInput *)buf;
        if (n < (int)sizeof *in) {
            log_ignored(buf, from, "too short");
            break;
        }
        Guest *g = by_ticket(in->ticket);
        if (!g) {
            log_ignored(buf, from, connected_count ? "wrong ticket" : "no guest connected");
            break;
        }
        if (side) g->last_heard = now;
        else heard(g, from, now);
        if (side ? !g->got_side++ : !g->got_main++)
            host_log("guest %d's controls are arriving (%s)", (int)(g - guests) + 1,
                     side ? "on the controls port" : "on the main connection");
        if (g->input_seq && (int)(in->seq - g->input_seq) <= 0) break;
        g->input_seq = in->seq;
        g->input.x = in->x / 32767.0f;
        g->input.y = in->y / 32767.0f;
        g->input.buttons = in->buttons;
        g->input.mouse_x = in->mouse_x;
        g->input.mouse_y = in->mouse_y;
        if ((in->seq & 7) == 0) {
            PktPong p = { PKT_PONG, in->time };
            struct sockaddr_in to = g->addr;
            LeaveCriticalSection(&cs);
            host_send(&to, &p, sizeof p);
            return;
        }
        break;
    }
    case PKT_KEYFRAME:
    case PKT_BYE: {
        const PktControl *c = (const PktControl *)buf;
        Guest *g = n >= (int)sizeof *c ? by_ticket(c->ticket) : NULL;
        if (!g) {
            log_ignored(buf, from, "not from a guest");
            break;
        }
        if (side) g->last_heard = now;
        else heard(g, from, now);
        if (buf[0] == PKT_KEYFRAME) want_key = 1;
        else disconnect(g, "left");
        break;
    }
    default:
        log_ignored(buf, from, "unknown kind");
    }
    LeaveCriticalSection(&cs);
}

static DWORD WINAPI net_thread(void *arg)
{
    (void)arg;
    BYTE buf[2048];
    DWORD last_stats = GetTickCount();
    for (;;) {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(sock, &ready);
        if (in_sock != INVALID_SOCKET) FD_SET(in_sock, &ready);
        struct timeval wait = { 0, 250000 };
        if (select(0, &ready, NULL, NULL, &wait) > 0)
            for (int side = 0; side < 2; side++) {
                SOCKET s = side ? in_sock : sock;
                if (s == INVALID_SOCKET || !FD_ISSET(s, &ready)) continue;
                struct sockaddr_in from;
                int len = sizeof from;
                int n = recvfrom(s, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &len);
                if (n > 0) handle(buf, n, &from, side);
            }
        DWORD now = GetTickCount();
        EnterCriticalSection(&cs);
        for (int i = 0; i < ONLINE_MAX_GUESTS; i++)
            if (guests[i].connected && now - guests[i].last_heard > NET_TIMEOUT) disconnect(&guests[i], "stopped answering");
        int on = connected_count;
        LeaveCriticalSection(&cs);
        if (now - last_stats >= 10000) {
            LONG bytes = InterlockedExchange(&sent_bytes, 0), frames = InterlockedExchange(&sent_frames, 0);
            if (on) {
                int tenths = (int)((LONGLONG)bytes * 8 / ((now - last_stats) * 100));
                host_log("streaming %d fps to %d guests, %d.%d Mbit/s in all", (int)(frames * 1000 / (LONG)(now - last_stats)),
                         on, tenths / 10, tenths % 10);
            }
            last_stats = now;
        }
    }
    return 0;
}

// --- Video ---------------------------------------------------------------------------

// A frame's packets go out spread over time, at up to twice the stream's bit rate for each
// guest, not as one burst: home routers' flood protection blocks an address that sends
// hundreds of packets at once, and bursts overflow slow links' buffers.
#define PACE_BURST (8 * (NET_FRAG + 16))
static void pace(int bytes, int guests_now)
{
    static double tokens = PACE_BURST;
    static LONGLONG last;
    double rate = (cfg.kbps * 2 > 4000 ? cfg.kbps * 2 : 4000) * 1000.0 / 8 * guests_now; // bytes per second
    for (;;) {
        LONGLONG now = now_ticks();
        if (last) tokens += (double)(now - last) * rate / tick_freq;
        last = now;
        if (tokens > PACE_BURST * guests_now) tokens = PACE_BURST * guests_now;
        if (tokens >= bytes) {
            tokens -= bytes;
            return;
        }
        Sleep(1);
    }
}

// The same frame to every guest, each with its own label spot.
static void send_video(void *ctx, const BYTE *data, int len, int key)
{
    const int (*marks)[2] = ctx;
    struct sockaddr_in to[ONLINE_MAX_GUESTS];
    int slot[ONLINE_MAX_GUESTS], n = 0;
    EnterCriticalSection(&cs);
    for (int i = 0; i < ONLINE_MAX_GUESTS; i++)
        if (guests[i].connected) {
            to[n] = guests[i].addr;
            slot[n++] = i;
        }
    LeaveCriticalSection(&cs);
    if (!n) return;
    int count = (len + NET_FRAG - 1) / NET_FRAG;
    if (count > NET_MAX_FRAGS) {
        host_log("frame of %d bytes too large, dropped", len);
        want_key = 1;
        return;
    }
    BYTE pkt[sizeof(PktVideo) + NET_FRAG];
    PktVideo *h = (PktVideo *)pkt;
    h->type = PKT_VIDEO;
    h->key = (BYTE)key;
    h->count = (WORD)count;
    h->frame = ++frame_id;
    for (int i = 0; i < count; i++) {
        int part = len - i * NET_FRAG < NET_FRAG ? len - i * NET_FRAG : NET_FRAG;
        h->frag = (WORD)i;
        memcpy(pkt + sizeof *h, data + i * NET_FRAG, part);
        pace(((int)sizeof *h + part) * n, n);
        for (int g = 0; g < n; g++) {
            h->mark_x = (short)marks[slot[g]][0];
            h->mark_y = (short)marks[slot[g]][1];
            host_send(&to[g], pkt, sizeof *h + part);
        }
    }
    InterlockedIncrement(&sent_frames);
}

static DWORD WINAPI encode_thread(void *arg)
{
    (void)arg;
    char err[160];
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    H264Enc *enc = h264_enc_open(enc_w, enc_h, cfg.fps, cfg.kbps, err, sizeof err);
    if (!enc) {
        host_log("no video: %s", err);
        encoder_dead = 1;
        return 0;
    }
    host_log("video %dx%d (coded %dx%d), %d fps, %d kbit/s for each guest", cfg.width, cfg.height, enc_w, enc_h,
             cfg.fps, cfg.kbps);
    BYTE *nv12 = malloc((size_t)enc_w * enc_h * 3 / 2);
    for (;;) {
        WaitForSingleObject(frame_event, INFINITE);
        bgra_to_nv12(frame_bgra, cfg.width * 4, cfg.width, cfg.height, nv12, enc_w, enc_h);
        int marks[ONLINE_MAX_GUESTS][2];
        memcpy(marks, frame_marks, sizeof marks);
        InterlockedExchange(&frame_busy, 0);
        int key = InterlockedExchange(&want_key, 0);
        if (!h264_enc_frame(enc, nv12, key, send_video, marks)) {
            host_log("encoding a frame failed");
            want_key = 1;
        }
    }
    return 0;
}

__declspec(dllexport) int online_wants_frame(void)
{
    if (!connected_count || encoder_dead || frame_busy) return 0;
    LONGLONG now = now_ticks();
    if (now < next_due) return 0;
    next_due += period;
    if (now - next_due > period) next_due = now + period; // fell behind: start over from now
    return 1;
}

__declspec(dllexport) void online_video(const BYTE *bgra, int pitch, const int marks[ONLINE_MAX_GUESTS][2])
{
    memcpy(frame_marks, marks, sizeof frame_marks);
    for (int y = 0; y < cfg.height; y++)
        memcpy(frame_bgra + (size_t)y * cfg.width * 4, bgra + (size_t)y * pitch, (size_t)cfg.width * 4);
    InterlockedExchange(&frame_busy, 1);
    SetEvent(frame_event);
}

// Online play switched off in the game (and on again): off, every guest is let go and new
// ones are told so (REJECT_CLOSED); the ports stay bound, for switching it on again.
__declspec(dllexport) void online_set_open(int on)
{
    EnterCriticalSection(&cs);
    if (open != !!on) host_log(on ? "taking guests again" : "online play switched off: guests let go");
    open = !!on;
    for (int i = 0; !open && i < ONLINE_MAX_GUESTS; i++)
        if (guests[i].connected) disconnect(&guests[i], "was let go");
    LeaveCriticalSection(&cs);
}

void host_set_public_address(const char *ip)
{
    EnterCriticalSection(&cs);
    lstrcpynA(public_address, ip, sizeof public_address);
    LeaveCriticalSection(&cs);
}

// What friends type in: the router's internet address once UPnP has it, else this PC's on
// its own network; with the port when it isn't the usual one.
__declspec(dllexport) void online_address(char *out, int len)
{
    char ip[64] = "";
    struct in_addr me;
    EnterCriticalSection(&cs);
    lstrcpynA(ip, public_address, sizeof ip);
    LeaveCriticalSection(&cs);
    if (!*ip && host_main_address(&me)) inet_ntop(AF_INET, &me, ip, sizeof ip);
    if (cfg.port == NET_PORT) snprintf(out, len, "%s", ip);
    else snprintf(out, len, "%s:%d", ip, cfg.port);
}

__declspec(dllexport) void online_get_input(OnlineInput *out)
{
    EnterCriticalSection(&cs);
    for (int i = 0; i < ONLINE_MAX_GUESTS; i++) {
        out[i] = guests[i].input;
        out[i].connected = guests[i].connected;
    }
    LeaveCriticalSection(&cs);
}

// --- Start ---------------------------------------------------------------------------

int host_main_address(struct in_addr *out)
{
    // connect() on a UDP socket only picks the route; nothing is sent.
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a;
    int len = sizeof a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl(0x01010101);
    int ok = !connect(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &len);
    closesocket(s);
    if (ok) *out = a.sin_addr;
    return ok;
}

static void log_lan_addresses(void)
{
    struct in_addr me;
    char ip[64];
    if (host_main_address(&me) && inet_ntop(AF_INET, &me, ip, sizeof ip)) {
        host_log("guests on your own network connect to %s (port %d)", ip, cfg.port);
        return;
    }
    char name[256], list[256] = "";
    struct addrinfo hints, *res, *a;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    if (gethostname(name, sizeof name) || getaddrinfo(name, NULL, &hints, &res)) return;
    for (a = res; a; a = a->ai_next) {
        char ip[64];
        inet_ntop(AF_INET, &((struct sockaddr_in *)a->ai_addr)->sin_addr, ip, sizeof ip);
        if (lstrlenA(list) + lstrlenA(ip) + 3 < (int)sizeof list) {
            if (*list) lstrcatA(list, ", ");
            lstrcatA(list, ip);
        }
    }
    freeaddrinfo(res);
    host_log("guests on your own network connect to %s (port %d)", list, cfg.port);
}

__declspec(dllexport) int online_start(const OnlineConfig *config)
{
    cfg = *config;
    if (cfg.fps < 10) cfg.fps = 10;
    if (cfg.fps > 120) cfg.fps = 120;
    if (cfg.kbps < 500) cfg.kbps = 500;
    InitializeCriticalSection(&cs);
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        host_log("no network (WSAStartup failed)");
        return 0;
    }
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)cfg.port);
    if (cfg.loopback_only) addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (sock == INVALID_SOCKET || bind(sock, (struct sockaddr *)&addr, sizeof addr)) {
        host_log("can't use UDP port %d (error %d); is another copy of the game running?", cfg.port, WSAGetLastError());
        return 0;
    }
    // A guest that went away makes Windows report "connection reset" on the next receive.
    BOOL no = FALSE;
    DWORD got;
    WSAIoctl(sock, SIO_UDP_CONNRESET, &no, sizeof no, NULL, 0, &got, NULL, NULL);
    DWORD timeout = 250;
    int buffer = 4 << 20;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char *)&buffer, sizeof buffer);
    in_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    addr.sin_port = htons((u_short)(cfg.port + 1));
    if (in_sock == INVALID_SOCKET || bind(in_sock, (struct sockaddr *)&addr, sizeof addr)) {
        host_log("can't use UDP port %d for the guest's controls (error %d); they come on the main one only",
                 cfg.port + 1, WSAGetLastError());
        if (in_sock != INVALID_SOCKET) closesocket(in_sock);
        in_sock = INVALID_SOCKET;
    } else {
        WSAIoctl(in_sock, SIO_UDP_CONNRESET, &no, sizeof no, NULL, 0, &got, NULL, NULL);
    }

    enc_w = (cfg.width + 15) & ~15;
    enc_h = (cfg.height + 15) & ~15;
    frame_bgra = malloc((size_t)cfg.width * cfg.height * 4);
    frame_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    tick_freq = f.QuadPart;
    period = f.QuadPart / cfg.fps;
    timeBeginPeriod(1); // Sleep(1) in pace() really 1 ms
    next_due = now_ticks();

    host_log("hosting up to %d guests on UDP port %d (and %d for their controls)", ONLINE_MAX_GUESTS, cfg.port,
             cfg.port + 1);
    log_lan_addresses();
    CloseHandle(CreateThread(NULL, 0, net_thread, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, encode_thread, NULL, 0, NULL));
    audio_start();
    if (cfg.upnp) upnp_start(cfg.port);
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
