// FF2.saf, the game's data archive: reading, changing and writing it.
#pragma once
#include <windows.h>

typedef struct {
    char *name;                 // lowercase path, '/' separated
    BYTE *data;
    DWORD size;
} SafEntry;

typedef struct {
    DWORD version;
    SafEntry *entries;
    int count, cap;
} Saf;

// MD5 with FF2's own initial state (the archive's hashes).
void saf_md5(const BYTE *data, DWORD len, BYTE out[16]);
// Plain MD5, for checking files are the ones the mod was made for.
void md5_plain(const BYTE *data, DWORD len, BYTE out[16]);

// 0: not an FF2 archive. The entries point into `file`, which must stay alive.
int saf_read(Saf *saf, BYTE *file, DWORD len);
SafEntry *saf_find(Saf *saf, const char *name);
// Replaces the entry, or adds it where a directory listing would put it. Keeps `data`.
void saf_put(Saf *saf, const char *name, BYTE *data, DWORD size);
// The whole archive, malloc'ed.
BYTE *saf_build(const Saf *saf, DWORD *len);
