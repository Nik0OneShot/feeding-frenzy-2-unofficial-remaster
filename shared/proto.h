// Online co-op packets (UDP), shared by the host (ff2online.dll, inside the game) and the
// guest viewer (ff2guest.exe). Every packet starts with its type byte; little-endian.
//
// The guest sends HELLO until the host answers WELCOME, then INPUT every few milliseconds
// (its whole input state, so a lost packet costs nothing; it also keeps the connection
// alive). The host streams VIDEO (H.264 frames split into fragments) and AUDIO (IMA ADPCM
// blocks), and answers some INPUTs with PONG for the guest's ping display. A guest that
// misses part of a frame asks for a fresh keyframe with KEYFRAME.
//
// The host takes several guests at once (ONLINE_MAX_GUESTS, online_api.h). WELCOME hands
// each a ticket that its later packets carry: the host knows each guest by its ticket, not
// by the address its packets come from, since routers may change that (the host then sends
// to the new one). A guest reconnecting sends its ticket in HELLO, and gets its own place
// (and so its own fish) back.
//
// The guest sends its INPUT, KEYFRAME and BYE twice: on the main connection and to the
// host's controls port (WELCOME's input_port, the main port + 1), on which the host only
// ever receives. Some routers (OpenWrt with flow offloading) lose incoming packets on a
// forwarded connection once it carries traffic both ways, like the main one does.
#pragma once
#include <windows.h>

#define NET_MAGIC     0x4f324646   // "FF2O"
#define NET_VERSION   5
#define NET_PORT      47600
#define NET_FRAG      1200         // video bytes per packet: fits any internet path's MTU
#define NET_MAX_FRAGS 1024         // largest frame: 1.2 MB
#define NET_TIMEOUT   5000         // ms without packets before the other side counts as gone

enum {
    PKT_HELLO = 1, PKT_WELCOME, PKT_REJECT, PKT_INPUT, PKT_PONG, PKT_KEYFRAME, PKT_BYE,
    PKT_VIDEO, PKT_AUDIO,
};

enum { REJECT_BUSY = 1, REJECT_VERSION, REJECT_CLOSED };

#define NET_DASH    1
#define NET_SPECIAL 2

#pragma pack(push, 1)
typedef struct { BYTE type; BYTE pad; WORD version; DWORD magic; DWORD ticket; } PktHello; // ticket: 0 the first time
typedef struct {
    BYTE type, fps;
    WORD width, height;            // the picture
    WORD enc_width, enc_height;    // the coded size: the picture plus padding to 16
    DWORD ticket;
    WORD input_port;               // 0: none
} PktWelcome;
typedef struct { BYTE type, reason; } PktReject;
typedef struct {
    BYTE type, buttons;            // NET_DASH | NET_SPECIAL
    short x, y;                    // stick / keys, -32767..32767, length <= 32767
    DWORD seq, time;               // time: the guest's clock in ms, echoed back in PONG
    int mouse_x, mouse_y;          // running totals of the guest's mouse movement
    DWORD ticket;
} PktInput;
typedef struct { BYTE type; DWORD time; } PktPong;
typedef struct { BYTE type; DWORD ticket; } PktControl;   // KEYFRAME, BYE
typedef struct {
    BYTE type, key;                // key: the frame is a keyframe (decodable on its own)
    WORD frag, count;
    DWORD frame;                   // counts up by one per frame
    short mark_x, mark_y;          // the guest's "You!" label: its arrow's tip, in the
                                   // picture's pixels; mark_x -32768: none
} PktVideo;                        // + up to NET_FRAG bytes
typedef struct {
    BYTE type, pad;
    WORD frames;                   // stereo sample frames, at 48000 Hz
    DWORD seq;
    short pred[2];                 // ADPCM state at the block start, per channel
    BYTE index[2];
} PktAudio;                        // + one byte per frame (left nibble low, right high)
#pragma pack(pop)

// Windows reports an ICMP "port unreachable" as an error on the next receive unless this is off.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET 0x9800000C
#endif

#define AUDIO_RATE  48000
#define AUDIO_BLOCK 480            // frames per packet: 10 ms
