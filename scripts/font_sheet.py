# Project Oogie: write the game's built-in font sheets (DXT5 DDS inside
# default.xbe, pointer table at VA 0x2C3800, see recomp_manual.c "European
# text font") as PNG files, with a 24-pixel grid, for reading which glyph
# sits where. Read-only on game\default.xbe.
# Usage: py -3 scripts\font_sheet.py   (writes build_out\font\us_font0.png, us_font1.png)
import os, struct, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def xbe_reader(path):
    d = open(path, 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    n = struct.unpack_from('<I', d, 0x11C)[0]
    sh = struct.unpack_from('<I', d, 0x120)[0] - base
    secs = [struct.unpack_from('<5I', d, sh + i * 0x38) for i in range(n)]

    def off(va):
        for _, v, _, ra, rs in secs:
            if v <= va < v + rs:
                return ra + va - v
        raise ValueError(hex(va))
    return d, off


def rgb565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31


def dxt5(data, w, h):
    px = [[(0, 0, 0, 0)] * w for _ in range(h)]
    o = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            a0, a1 = data[o], data[o + 1]
            abits = int.from_bytes(data[o + 2:o + 8], 'little')
            al = [a0, a1] + ([((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)] if a0 > a1
                             else [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255])
            c0, c1 = struct.unpack_from('<HH', data, o + 8)
            cbits = struct.unpack_from('<I', data, o + 12)[0]
            p0, p1 = rgb565(c0), rgb565(c1)
            cl = [p0, p1, tuple((2 * a + b) // 3 for a, b in zip(p0, p1)), tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
            for i in range(16):
                x, y = bx + i % 4, by + i // 4
                a = al[(abits >> (3 * i)) & 7]
                c = cl[(cbits >> (2 * i)) & 3]
                px[y][x] = (c[0], c[1], c[2], a)
            o += 16
    return px


def write_png(path, px, grid=24):
    h, w = len(px), len(px[0])
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            r, g, b, a = px[y][x]
            v = a  # glyphs are white with alpha: show alpha as brightness on dark grey
            if grid and (x % grid == 0 or y % grid == 0):
                raw += bytes((90, 30, 30))
            else:
                raw += bytes((40 + v * 215 // 255,) * 3)
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def main():
    d, off = xbe_reader(os.path.join(ROOT, 'game', 'default.xbe'))
    out = os.path.join(ROOT, 'build_out', 'font')
    os.makedirs(out, exist_ok=True)
    for i, va in enumerate(struct.unpack_from('<2I', d, off(0x2C3800))):
        o = off(va)
        assert d[o:o + 4] == b'DDS ', hex(va)
        h, w = struct.unpack_from('<II', d, o + 12)
        px = dxt5(d[o + 128:o + 128 + w * h], w, h)
        write_png(os.path.join(out, 'us_font%d.png' % i), px)
        print('us_font%d.png %dx%d' % (i, w, h))


if __name__ == '__main__':
    main()
