"""Rebuild FF2.saf with added or replaced files.

Usage: saf_repack.py ORIGINAL.saf MODS_DIR OUTPUT.saf
Every file under MODS_DIR (e.g. MODS_DIR/resource/shell/x.png) replaces or adds the
archive entry with the same lowercase path.

Format (see saf_extract.py): 'FFAS', u32 version, u32 toc_offset, file data...,
TOC = u32 1, 16-byte toc hash, u32 count, entries (u32 offset, u32 size, 16-byte hash,
u16 name_len incl. NUL, name). Hashes are MD5 with FF2's custom initial state; the TOC
hash covers everything after itself. Entries are kept in Windows directory-listing order
(case-insensitive, '_' after letters), with new files inserted where a listing would put them.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from saf_md5 import md5


def read_entries(d):
    magic, ver, toc = struct.unpack_from('<4sII', d, 0)
    assert magic == b'FFAS', magic
    count, = struct.unpack_from('<I', d, toc + 20)
    o, entries = toc + 24, []
    for _ in range(count):
        off, size = struct.unpack_from('<II', d, o)
        ln, = struct.unpack_from('<H', d, o + 24)
        entries.append([d[o + 26:o + 26 + ln - 1].decode(), d[off:off + size]])
        o += 26 + ln
    return ver, entries


def listing_key(name):
    # Directory walk order: compare path component by component, case-insensitively.
    return [part.upper() for part in name.split('/')]


def build(ver, entries):
    out = bytearray(b'FFAS' + struct.pack('<II', ver, 0))
    table = bytearray(struct.pack('<I', len(entries)))
    for name, data in entries:
        table += struct.pack('<II', len(out), len(data)) + md5(data)
        table += struct.pack('<H', len(name) + 1) + name.encode() + b'\0'
        out += data
    struct.pack_into('<I', out, 8, len(out))
    out += struct.pack('<I', 1) + md5(bytes(table)) + table
    return bytes(out)


def main(original, mods_dir, output):
    ver, entries = read_entries(open(original, 'rb').read())
    index = {name: i for i, (name, _) in enumerate(entries)}
    for root, _, files in os.walk(mods_dir):
        for f in files:
            path = os.path.join(root, f)
            name = os.path.relpath(path, mods_dir).replace(os.sep, '/').lower()
            data = open(path, 'rb').read()
            if name in index:
                entries[index[name]][1] = data
                print('replaced', name)
            else:
                pos = next((i for i, (n, _) in enumerate(entries) if listing_key(n) > listing_key(name)), len(entries))
                entries.insert(pos, [name, data])
                index = {n: i for i, (n, _) in enumerate(entries)}
                print('added', name, 'at', pos)
    open(output, 'wb').write(build(ver, entries))
    print('wrote', output, len(entries), 'files')


if __name__ == '__main__':
    main(*sys.argv[1:4])
