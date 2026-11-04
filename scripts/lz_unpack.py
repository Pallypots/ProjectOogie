"""Pure-Python decompressor for Oogie's Revenge (Xbox) game\\stm_data\\*.lz files.

Translated from the game's own code (default.xbe, text section):
  sub_00031430(strm, dst, callback, arg)  reads the header, then calls
  sub_00031120(strm, mode, dst, size)     the LZ decoder
  sub_000310B0(n)                         reads n bits
Bit reader: 32-bit little-endian words, bits taken from bit 0 upwards, so the
stream is LSB-first over bytes. Multi-bit values are assembled LSB-first.

File layout (all bit fields LSB-first, continuous):
  8 bits   mode (every disc file has 2; 0 = stored, 1 = fixed 10-bit offsets)
  32 bits  unpacked size
  tokens until 'size' bytes are written:
    1 bit flag
      1 -> literal: 8 bits, one output byte
      0 -> match:
           offset: mode 1: 10 bits; mode 2: k = count of 0 bits before a 1,
                   then WIDTH[k] bits                       (WIDTH at 0x260A00)
           length: k = count of 0 bits before a 1, then WIDTH[k] bits
           copy 'length' bytes from out[pos - offset - 1], dword at a time
           first (rep movsd) then the remaining 0..3 bytes (rep movsb).
WIDTH = {2, 4, 6, 10} (dwords at 0x260A00 in the XBE).

Usage: lz_unpack.py <in.lz> <out.bin>      or   lz_unpack.py --dir <in_dir> <out_dir>
"""
import os, sys, struct

WIDTH = (2, 4, 6, 10)


class LzError(Exception):
    pass


def unpack(data):
    """Decompress one .lz file's bytes; returns bytes."""
    n = len(data)
    if n < 5:
        raise LzError("short file")
    mode = data[0]
    size = struct.unpack_from("<I", data, 1)[0]
    if mode == 0:
        # Stored (Hypothesis, no disc file uses it): sub_00031430 copies raw bytes
        # from the stream's current dword pointer, which is src+4 after the
        # 40 header bits.
        return bytes(data[4:4 + size])
    src = bytes(data) + b"\0" * 16          # the reader may look past the end
    out = bytearray(size)
    p = 40                                  # bit position
    pos = 0
    frombytes = int.from_bytes
    W = WIDTH
    while pos < size:
        q = p >> 3
        w = frombytes(src[q:q + 8], "little") >> (p & 7)   # >= 57 valid bits
        if w & 1:
            out[pos] = (w >> 1) & 0xFF
            pos += 1
            p += 9
            continue
        # match: decode from a fresh wide window (worst case 1+4+10+4+10 bits)
        w = frombytes(src[q:q + 16], "little") >> (p & 7)
        b = 1
        if mode == 1:
            dist = (w >> b) & 0x3FF
            b += 10
        else:
            t = w >> b
            k = (t & -t).bit_length() - 1 if t else 99
            if k > 3:
                raise LzError("bad offset prefix at bit %d" % (p + b))
            b += k + 1
            dist = (w >> b) & ((1 << W[k]) - 1)
            b += W[k]
        t = w >> b
        k = (t & -t).bit_length() - 1 if t else 99
        if k > 3:
            raise LzError("bad length prefix at bit %d" % (p + b))
        b += k + 1
        ln = (w >> b) & ((1 << W[k]) - 1)
        b += W[k]
        p += b
        s = pos - dist - 1
        if s < 0:
            raise LzError("offset before start at out %d" % pos)
        end = pos + ln
        if end > size:
            # The game copies past 'size' (its buffer is size+7 rounded); keep
            # only what fits.
            out.extend(b"\0" * (end - size + 4))
        o = dist + 1
        if o >= ln:
            out[pos:end] = out[s:s + ln]
        elif o >= 4:
            # overlapping, but each dword read is already written: byte-wise
            # forward copy gives the same result as rep movsd
            i = 0
            while i < ln:
                c = min(o, ln - i)
                out[pos + i:pos + i + c] = out[s + i:s + i + c]
                i += c
        else:
            # o < 4: rep movsd reads a dword that overlaps bytes it has not yet
            # written; emulate exactly (dword reads, then byte tail).
            nd = ln >> 2
            for i in range(nd):
                out[pos + 4 * i:pos + 4 * i + 4] = out[s + 4 * i:s + 4 * i + 4]
            for i in range(nd * 4, ln):
                out[pos + i] = out[s + i]
        pos = end
    del out[size:]
    return bytes(out)


def unpack_file(path):
    with open(path, "rb") as f:
        return unpack(f.read())


def _one(args):
    src, dst = args
    data = unpack_file(src)
    with open(dst, "wb") as f:
        f.write(data)
    return os.path.basename(src), len(data)


def main(argv):
    if len(argv) == 3 and argv[0] == "--dir":
        import glob, multiprocessing
        ind, outd = argv[1], argv[2]
        os.makedirs(outd, exist_ok=True)
        jobs = []
        for fp in sorted(glob.glob(os.path.join(ind, "*.lz"))):
            dst = os.path.join(outd, os.path.splitext(os.path.basename(fp))[0] + ".bin")
            if not os.path.exists(dst):
                jobs.append((fp, dst))
        with multiprocessing.Pool() as pool:
            for name, n in pool.imap_unordered(_one, jobs):
                print(name, n, flush=True)
        return 0
    if len(argv) == 2:
        print(_one((argv[0], argv[1])))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
