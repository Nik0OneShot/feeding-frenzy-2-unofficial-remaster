#pragma once
#include <windows.h>

#define MICE_LEFT  1
#define MICE_RIGHT 2
#define MICE_PLAYER1 (-1)  // player 1's own mouse, for mice_take / mice_buttons
#define MICE_MAX_KEYBOARDS 4

void mice_init(void);
// Call every frame on the game's thread: whether a co-op level is being played (relative
// mouse); only then do mice and keyboards belong to players.
void mice_set_play(int playing);
// Shared out right now: playing, and some other mouse belongs to a player (then player 1
// steers and clicks with its own mouse only).
int mice_shared_out(void);
// A mouse no player has that was clicked during play since the last call: it's claimed
// (it keeps its player for the session) and its id returned; -1 if none.
int mice_new_click(void);
// Movement (mouse counts) since the last call and the buttons held, for a claimed mouse (id)
// or player 1's (MICE_PLAYER1, only while shared out); 0 when there's nothing to take.
int mice_take(int id, int *dx, int *dy, int *buttons);
// The buttons a claimed mouse or player 1's holds (0 if none).
int mice_buttons(int id);
// Whether a key (VK_*, left/right Shift and Ctrl told apart) is held on one keyboard (0..),
// each tracked apart; with no keyboard seen by raw input yet, keyboard 0 is all of them.
int keys_held(int keyboard, int vk);
// How many keyboards there are to read (at least 1).
int keys_keyboards(void);
