"""MD5 with a custom initial state, as used by FF2.saf (ArchiveLoader TOC validation, 0x450800)."""
import struct, math

FF2_IV = (0xfb132803, 0x06e828f3, 0xa12a051c, 0x9bedf4fc)
_S = [7, 12, 17, 22] * 4 + [5, 9, 14, 20] * 4 + [4, 11, 16, 23] * 4 + [6, 10, 15, 21] * 4
_K = [int(abs(math.sin(i + 1)) * 2**32) & 0xffffffff for i in range(64)]

def md5(data, iv=FF2_IV):
    a0, b0, c0, d0 = iv
    msg = bytearray(data) + b'\x80'
    msg += b'\0' * ((56 - len(msg) % 64) % 64) + struct.pack('<Q', (len(data) * 8) & 0xffffffffffffffff)
    for off in range(0, len(msg), 64):
        m = struct.unpack_from('<16I', msg, off)
        a, b, c, d = a0, b0, c0, d0
        for i in range(64):
            if i < 16: f, g = (b & c) | (~b & d), i
            elif i < 32: f, g = (d & b) | (~d & c), (5 * i + 1) % 16
            elif i < 48: f, g = b ^ c ^ d, (3 * i + 5) % 16
            else: f, g = c ^ (b | ~d), (7 * i) % 16
            f = (f + a + _K[i] + m[g]) & 0xffffffff
            a, d, c = d, c, b
            b = (b + ((f << _S[i]) | (f >> (32 - _S[i])))) & 0xffffffff
        a0, b0, c0, d0 = (a0 + a) & 0xffffffff, (b0 + b) & 0xffffffff, (c0 + c) & 0xffffffff, (d0 + d) & 0xffffffff
    return struct.pack('<4I', a0, b0, c0, d0)
