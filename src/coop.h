#pragma once
#include <windows.h>

#define MAX_PLAYERS 10    // with online guests
#define LOCAL_PLAYERS 4   // on this PC's own devices, player 1 included

// player_colors[i]: player i+1's color from ff2wide.ini ("r,g,b"), [0] unused; NULL or ""
// for the default.
// mode_key: coop_mode in ff2wide.ini ("together", "hunger").
int coop_init(int enable, const char *mode_key, float speed, const char *const *player_colors,
              int (*patch_u32)(DWORD, DWORD, DWORD));
void coop_frame(BYTE *engine);
void coop_before_draw(void);
void coop_after_draw(void);
void coop_steer(BYTE *player, float dt, float speed_option);
// The positions of everyone in play, for the co-op camera: how many (up to max), when two or
// more, else 0.
int coop_player_positions(float (*pos)[2], int max);
int coop_respawn_framing(void);
// Where the "You!" label over a fish goes (which: the player's number), in the play layer's
// coordinates; 0 while that fish isn't in play.
int coop_label_spot(int which, float *spot);
// An online guest's player number (guest 0 ..; 0: none).
int coop_guest_player(int guest);
// The options screen's Co-op checkbox: the setting (saved to ff2wide.ini). On takes effect
// when the next level starts; off at once, taking the other fish out.
int coop_requested(void);
void coop_request(int on);
// Online play was switched off: the guests' fish are taken out (in the next frame).
void coop_online_closed(void);
// The Options screen's Co-op button shows this: "Off", or the mode co-op plays in.
const char *coop_mode_name(void);
// A competitive round's winner, across the frame (back buffer, width x height) for a while.
void coop_draw_banner(void **back_surface, int width, int height);
// The Options screen's color buttons show these (player 2 .. MAX_PLAYERS; cycled by the
// buttons).
const char *coop_color_name(int player);
// Sets a TextItem's text (unless it's that already); the TextItem inside a button.
void coop_set_text(BYTE *text, const char *value);
BYTE *coop_button_text(BYTE *button);
// The HUD's shared score text and the per-player totals (texts[MAX_PLAYERS]; any may be NULL).
void coop_show_scores(BYTE *total, BYTE **texts);
