# Project Oogie: work out the European one-byte text code -> character table
# from the game's own message files (stm_data\message.stm, message2.stm):
# every compiled X_<lang>.bms is lined up with its source X_<lang>.txt.
# Read-only on game\. Usage: py -3 scripts\eu_charmap.py
# Prints code -> characters seen (with counts) and conflicts.
import os, re, struct, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def stm_entries(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 0x1C)[0]
    out = {}
    for i in range(n):
        o = 0x20 + i * 32
        name = d[o:o + 24].split(b'\0')[0].decode('latin-1')
        off, size = struct.unpack_from('<II', d, o + 24)
        size &= 0x7fffffff
        if size & 0x80000000 == 0 and off + size <= len(d) and '.' in name:
            out[name] = d[off:off + size]
    return d, out


def nested(path):
    # message2.stm holds a group ("save") of items
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 0x1C)[0]
    out = {}
    for i in range(n):
        o = 0x20 + i * 32
        off, cnt = struct.unpack_from('<II', d, o + 24)
        cnt &= 0x7fffffff
        for j in range(cnt):
            e = off + j * 32
            name = d[e:e + 24].split(b'\0')[0].decode('latin-1')
            io, isz = struct.unpack_from('<II', d, e + 24)
            out[name] = d[io:io + (isz & 0x7fffffff)]
    return out


def bms_messages(b):
    n = struct.unpack_from('<I', b, 0)[0]
    offs = list(struct.unpack_from('<%dI' % n, b, 4)) + [len(b)]
    return [b[offs[i]:offs[i + 1]] for i in range(n)]


def txt_messages(t):
    t = re.sub(r'<!--.*?-->', '', t, flags=re.S)
    return [m.strip() for m in re.findall(r'<MES>(.*?)</MES>', t, flags=re.S | re.I)]


def text_chars(m):
    m = re.sub(r'<[^>]*>', '', m)
    m = re.sub(r'\s+', '', m)          # layout whitespace is not text; spaces are written as _
    return m.replace('_', ' ')


def code_seq(msg):
    out, i = [], 0
    while i < len(msg):
        c = msg[i]
        if c >= 0x80 and i + 1 < len(msg):
            out.append((c << 8) | msg[i + 1]); i += 2
        else:
            out.append(c); i += 1
    return out


def all_items(path):
    # every named item of an @STM archive: top-level files, and groups
    # (size word with the top bit set = item count) one level down
    d = open(path, 'rb').read()
    if d[:4] != b'@STM':
        return {}
    n = struct.unpack_from('<I', d, 0x1C)[0]
    out = {}
    for i in range(n):
        o = 0x20 + i * 32
        name = d[o:o + 24].split(b'\0')[0].decode('latin-1')
        off, sz = struct.unpack_from('<II', d, o + 24)
        if sz & 0x80000000:
            for j in range(sz & 0x7fffffff):
                e = off + j * 32
                if e + 32 > len(d):
                    break
                iname = d[e:e + 24].split(b'\0')[0].decode('latin-1')
                io, isz = struct.unpack_from('<II', d, e + 24)
                if io + (isz & 0x7fffffff) <= len(d):
                    out[iname] = d[io:io + (isz & 0x7fffffff)]
        elif off + sz <= len(d):
            out[name] = d[off:off + sz]
    return out


def all_files():
    files = {}
    sd = os.path.join(ROOT, 'game', 'stm_data')
    for f in sorted(os.listdir(sd)):
        if f.endswith('.stm'):
            for k, v in all_items(os.path.join(sd, f)).items():
                files.setdefault(k, v)
    return files


def main():
    files = all_files()
    seen = collections.defaultdict(collections.Counter)
    aligned = skipped = 0
    for name, b in sorted(files.items()):
        m = re.match(r'(.+)_([bfgis])\.bms$', name)
        if not m:
            continue
        src = files.get(m.group(1) + '_' + m.group(2) + '.txt')
        if src is None:
            continue
        txt = src.decode('utf-8', 'replace')
        bm, tm = bms_messages(b), txt_messages(txt)
        if len(bm) != len(tm):
            skipped += 1
            continue
        for cb, ct in zip(bm, tm):
            codes = [c for c in code_seq(cb) if c >= 0x08]
            chars = text_chars(ct)
            if len(codes) != len(chars):
                skipped += 1
                continue
            aligned += 1
            for c, ch in zip(codes, chars):
                seen[c][ch] += 1
    print('aligned messages', aligned, 'skipped', skipped)
    for c in sorted(seen):
        print('%04X' % c, ' '.join('%r:%d' % (k, v) for k, v in seen[c].most_common(4)))


if __name__ == '__main__':
    main()
