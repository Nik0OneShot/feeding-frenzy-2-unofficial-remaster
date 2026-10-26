// The changes the Remaster makes to the game's data, made from the player's own copy.
#pragma once
#include <windows.h>
#include "saf.h"

// Changes and adds the archive's files (the new data stays allocated), and makes the loose
// splash/splashScreen.xml from the original one. 0 with a reason in `err` if this copy's data
// isn't what the changes expect.
int mods_apply(Saf *saf, const BYTE *splash, DWORD splash_len, BYTE **splash_out, DWORD *splash_out_len,
               char *err, int err_len);
