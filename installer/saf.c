// FF2.saf: 'FFAS', u32 version, u32 toc_offset, the files' data, then the table of contents:
// u32 1, 16-byte hash of the rest of the table, u32 count, entries (u32 offset, u32 size,
// 16-byte hash of the file, u16 name length with its NUL, name). The hashes are MD5 from
// FF2's own initial state; the game checks the table's (ArchiveLoader). Entries are kept in
// Windows directory-listing order: compared path part by part, case-insensitively, so '_'
// comes after the letters.
#include <stdlib.h>
#include <string.h>
#include "saf.h"

static const DWORD FF2_IV[4] = { 0xfb132803, 0x06e828f3, 0xa12a051c, 0x9bedf4fc };
static const DWORD PLAIN_IV[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
static const int S[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                           5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                           4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                           6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
static const DWORD K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static void md5_block(DWORD h[4], const BYTE *p)
{
    DWORD m[16], a = h[0], b = h[1], c = h[2], d = h[3];
    memcpy(m, p, 64);
    for (int i = 0; i < 64; i++) {
        DWORD f;
        int g;
        if (i < 16) f = (b & c) | (~b & d), g = i;
        else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
        else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
        else f = c ^ (b | ~d), g = (7 * i) % 16;
        f += a + K[i] + m[g];
        a = d, d = c, c = b;
        b += (f << S[i]) | (f >> (32 - S[i]));
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d;
}

static void md5_from(const DWORD iv[4], const BYTE *data, DWORD len, BYTE out[16])
{
    DWORD h[4] = { iv[0], iv[1], iv[2], iv[3] };
    DWORD i = 0;
    for (; i + 64 <= len; i += 64) md5_block(h, data + i);
    BYTE tail[128] = { 0 };
    DWORD rest = len - i, n = rest < 56 ? 64 : 128;
    memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    unsigned long long bits = (unsigned long long)len * 8;
    memcpy(tail + n - 8, &bits, 8);
    for (DWORD o = 0; o < n; o += 64) md5_block(h, tail + o);
    memcpy(out, h, 16);
}

void saf_md5(const BYTE *data, DWORD len, BYTE out[16])
{
    md5_from(FF2_IV, data, len, out);
}

void md5_plain(const BYTE *data, DWORD len, BYTE out[16])
{
    md5_from(PLAIN_IV, data, len, out);
}

static DWORD u32(const BYTE *p)
{
    DWORD v;
    memcpy(&v, p, 4);
    return v;
}

int saf_read(Saf *saf, BYTE *file, DWORD len)
{
    memset(saf, 0, sizeof *saf);
    if (len < 12 || memcmp(file, "FFAS", 4)) return 0;
    saf->version = u32(file + 4);
    DWORD toc = u32(file + 8);
    if (toc + 24 > len) return 0;
    DWORD count = u32(file + toc + 20), o = toc + 24;
    saf->entries = calloc(count + 64, sizeof *saf->entries);
    saf->cap = count + 64;
    for (DWORD i = 0; i < count; i++) {
        if (o + 26 > len) return 0;
        DWORD off = u32(file + o), size = u32(file + o + 4);
        WORD name_len;
        memcpy(&name_len, file + o + 24, 2);
        if (!name_len || o + 26 + name_len > len || off + size > len) return 0;
        SafEntry *e = &saf->entries[saf->count++];
        e->name = malloc(name_len);
        memcpy(e->name, file + o + 26, name_len - 1);
        e->name[name_len - 1] = 0;
        e->data = file + off;
        e->size = size;
        o += 26 + name_len;
    }
    return 1;
}

SafEntry *saf_find(Saf *saf, const char *name)
{
    for (int i = 0; i < saf->count; i++)
        if (!strcmp(saf->entries[i].name, name)) return &saf->entries[i];
    return NULL;
}

static int upper(int c)
{
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

// Directory-listing order: part by part, case-insensitively; a path that runs out first
// (a prefix of the other's parts) comes first.
static int listing_cmp(const char *a, const char *b)
{
    for (;;) {
        const char *ea = strchr(a, '/'), *eb = strchr(b, '/');
        size_t la = ea ? (size_t)(ea - a) : strlen(a), lb = eb ? (size_t)(eb - b) : strlen(b);
        for (size_t i = 0; i < la && i < lb; i++) {
            int ca = upper((BYTE)a[i]), cb = upper((BYTE)b[i]);
            if (ca != cb) return ca < cb ? -1 : 1;
        }
        if (la != lb) return la < lb ? -1 : 1;
        if (!ea || !eb) return !ea && !eb ? 0 : !ea ? -1 : 1;
        a = ea + 1, b = eb + 1;
    }
}

void saf_put(Saf *saf, const char *name, BYTE *data, DWORD size)
{
    SafEntry *e = saf_find(saf, name);
    if (e) {
        e->data = data;
        e->size = size;
        return;
    }
    int pos = 0;
    while (pos < saf->count && listing_cmp(saf->entries[pos].name, name) <= 0) pos++;
    if (saf->count == saf->cap) {
        saf->cap *= 2;
        saf->entries = realloc(saf->entries, saf->cap * sizeof *saf->entries);
    }
    memmove(&saf->entries[pos + 1], &saf->entries[pos], (saf->count - pos) * sizeof *saf->entries);
    saf->count++;
    e = &saf->entries[pos];
    e->name = _strdup(name);
    e->data = data;
    e->size = size;
}

BYTE *saf_build(const Saf *saf, DWORD *len)
{
    DWORD data_len = 12, table_len = 4;
    for (int i = 0; i < saf->count; i++) {
        data_len += saf->entries[i].size;
        table_len += 26 + (DWORD)strlen(saf->entries[i].name) + 1;
    }
    BYTE *out = malloc(data_len + 4 + 16 + table_len);
    memcpy(out, "FFAS", 4);
    memcpy(out + 4, &saf->version, 4);
    memcpy(out + 8, &data_len, 4);
    BYTE *table = out + data_len + 20;
    DWORD at = 12, t = 0, count = saf->count;
    memcpy(table, &count, 4);
    t = 4;
    for (int i = 0; i < saf->count; i++) {
        const SafEntry *e = &saf->entries[i];
        memcpy(out + at, e->data, e->size);
        memcpy(table + t, &at, 4);
        memcpy(table + t + 4, &e->size, 4);
        saf_md5(e->data, e->size, table + t + 8);
        WORD name_len = (WORD)(strlen(e->name) + 1);
        memcpy(table + t + 24, &name_len, 2);
        memcpy(table + t + 26, e->name, name_len);
        t += 26 + name_len;
        at += e->size;
    }
    DWORD one = 1;
    memcpy(out + data_len, &one, 4);
    saf_md5(table, table_len, out + data_len + 4);
    *len = data_len + 20 + table_len;
    return out;
}
