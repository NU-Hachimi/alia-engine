"""Reading pixels back from the engine's PNG screenshots (tests; no Pillow in the runtime image)."""

import struct
import zlib


def pixel(path, x, y):
    data = open(path, "rb").read()
    w, h = struct.unpack(">2I", data[16:24])
    idat, pos = b"", 8
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        if kind == b"IDAT":
            idat += data[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw, bpp, stride = zlib.decompress(idat), 4, w * 4
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):  # PNG filters (the engine writes with stb: any of them)
        f, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for k in range(stride):
            a = line[k - bpp] if k >= bpp else 0
            b, c = prev[k], prev[k - bpp] if k >= bpp else 0
            p = a + b - c
            pr = a if abs(p - a) <= abs(p - b) and abs(p - a) <= abs(p - c) else b if abs(p - b) <= abs(p - c) else c
            line[k] = (line[k] + (0, a, b, (a + b) // 2, pr)[f]) & 0xFF
        rows.append(line)
        prev = line
    return tuple(rows[y][x * 4:x * 4 + 3])


def near(a, b, tol=6):
    return all(abs(p - q) <= tol for p, q in zip(a, b))
