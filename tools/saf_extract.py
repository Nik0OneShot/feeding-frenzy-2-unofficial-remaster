"""Extract FF2.saf (PopCap 'FFAS' archive).
Header: 'FFAS', u32 version, u32 toc_offset, ... ; file offsets are absolute.
TOC: u32 ?, 16-byte hash, u32 file_count, then per file:
     u32 offset, u32 size, 16-byte hash, u16 name_len (incl. NUL), name."""
import struct, sys, os
DATA_BASE = 0
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
magic, ver, toc = struct.unpack_from('<4sII', d, 0)
assert magic == b'FFAS', magic
count, = struct.unpack_from('<I', d, toc + 20)
o = toc + 24
for _ in range(count):
    off, size = struct.unpack_from('<II', d, o)
    ln, = struct.unpack_from('<H', d, o + 24)
    name = d[o+26:o+26+ln].rstrip(b'\0').decode()
    o += 26 + ln
    p = os.path.join(dst, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, 'wb').write(d[DATA_BASE+off:DATA_BASE+off+size])
print('extracted', count, 'files; toc ends at', hex(o), 'file size', hex(len(d)))
