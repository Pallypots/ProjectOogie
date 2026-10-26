#!/usr/bin/env python3
"""Read-only XDVDFS (Xbox disc) reader for Project Oogie.

Usage:
  xiso_tool.py list    <image.iso> [out.tsv]
  xiso_tool.py extract <image.iso> <path-in-image> <out-file>
  xiso_tool.py extractall <image.iso> <out-dir>

Never writes to the image. Supports game-partition offsets 0 (extract-xiso
XISO) and the Redump offsets.
"""
import os, struct, sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PART_OFFSETS = (0, 0x18300000, 0xFD90000, 0x2080000)


class XIso:
    def __init__(self, path):
        self.f = open(path, "rb")
        for off in PART_OFFSETS:
            self.f.seek(off + 32 * SECTOR)
            if self.f.read(20) == MAGIC:
                self.base = off
                break
        else:
            raise SystemExit("not an XDVDFS image")
        self.f.seek(self.base + 32 * SECTOR + 20)
        self.root_sector, self.root_size = struct.unpack("<II", self.f.read(8))

    def read(self, sector, size):
        self.f.seek(self.base + sector * SECTOR)
        return self.f.read(size)

    def entries(self, sector, size):
        if size == 0:
            return []
        data = self.read(sector, size)
        out, stack, seen = [], [0], set()
        while stack:
            off = stack.pop()
            if off in seen or off + 14 > len(data):
                continue
            seen.add(off)
            l, r, st, sz, attr, nl = struct.unpack_from("<HHIIBB", data, off)
            if l == 0xFFFF:
                continue
            name = data[off + 14: off + 14 + nl].decode("latin-1")
            out.append((name, st, sz, attr))
            if l:
                stack.append(l * 4)
            if r:
                stack.append(r * 4)
        return out

    def walk(self, sector=None, size=None, prefix=""):
        if sector is None:
            sector, size = self.root_sector, self.root_size
        for name, st, sz, attr in sorted(self.entries(sector, size), key=lambda e: e[0].lower()):
            p = prefix + name
            if attr & 0x10:
                yield p + "/", st, sz, True
                yield from self.walk(st, sz, p + "/")
            else:
                yield p, st, sz, False

    def copy(self, st, sz, dst):
        os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
        self.f.seek(self.base + st * SECTOR)
        with open(dst, "wb") as o:
            left = sz
            while left:
                b = self.f.read(min(left, 1 << 22))
                o.write(b)
                left -= len(b)


def main():
    cmd, img = sys.argv[1], sys.argv[2]
    x = XIso(img)
    if cmd == "list":
        out = open(sys.argv[3], "w") if len(sys.argv) > 3 else sys.stdout
        print(f"# partition_base=0x{x.base:x} root_sector={x.root_sector} root_size={x.root_size}", file=out)
        for p, st, sz, d in x.walk():
            print(f"{'D' if d else 'F'}\t{sz}\t{st}\t{p}", file=out)
    elif cmd == "extract":
        want = sys.argv[3].replace("\\", "/").lower()
        for p, st, sz, d in x.walk():
            if p.lower() == want and not d:
                x.copy(st, sz, sys.argv[4])
                print(f"extracted {p} ({sz} bytes)")
                return
        raise SystemExit("not found: " + want)
    elif cmd == "extractall":
        for p, st, sz, d in x.walk():
            if not d:
                x.copy(st, sz, os.path.join(sys.argv[3], p))
        print("done")


if __name__ == "__main__":
    main()
