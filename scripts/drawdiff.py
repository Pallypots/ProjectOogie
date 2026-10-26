"""Compare the draw records of a flash frame with the frames around it.

Reads logs/drawdiff_<frame>.txt (written by the D3D11 back end under
OOGIE_FLASH_MEASURE=1, KNOWN_ISSUES #46): three frames of per-draw state
(target, texture, format, content hash, combiner hash, fog colour, blend,
vertex program, constants hash or (5A + start slot + length) without
OOGIE_FLASH_VC, index count, misc state, fog mode). The
display buffers rotate, so each frame's own buffer is called B and the one
before it P. The draw lists are aligned (difflib) and the differences of the
flash frame against both neighbours are printed, with the draws that differ
from both marked.
Usage: python scripts/drawdiff.py logs/drawdiff_<frame>.txt [--all]
"""
import difflib
import re
import sys

FIELDS = ["tgt", "tex", "texfmt", "texhash", "rc", "fogc", "blend", "vs", "vc", "n", "misc", "fog"]
DISPLAY = {0x104000, 0x35C000, 0x230000}


def load(path):
    frames, cur = [], None
    for line in open(path, encoding="latin-1"):
        m = re.match(r"-- frame (\d+)", line)
        if m:
            cur = (int(m.group(1)), [])
            frames.append(cur)
            continue
        m = re.match(r"(\d+): (.*)", line)
        if m and cur is not None:
            cur[1].append([int(x, 16) for x in m.group(2).split()])
    return frames


def normalise(frame, prev_b):
    recs = frame[1]
    counts = {}
    for r in recs:
        if r[0] in DISPLAY:
            counts[r[0]] = counts.get(r[0], 0) + 1
    b = max(counts, key=counts.get) if counts else None

    def name(v):
        key = v & 0x0FFFFFFF
        if key == b:
            return "B"
        if key == prev_b:
            return "P"
        if key in DISPLAY:
            return "PP"
        return "%X" % v

    out = []
    for r in recs:
        tex = name(r[1]) if r[1] in DISPLAY else "%X" % r[1]
        th = name(r[3] & 0x0FFFFFFF) if (r[3] & 0xF0000000) == 0xF0000000 else "%X" % r[3]
        out.append((name(r[0]) if r[0] in DISPLAY else "%X" % r[0], tex, "%X" % r[2], th,
                    "%X" % r[4], "%X" % r[5], "%X" % r[6], "%X" % r[7], "%X" % r[8],
                    "%X" % r[9], "%X" % r[10], "%X" % r[11]))
    return out, b


def diff(a, b, label):
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    changed = set()
    lines = []
    for op, i1, i2, j1, j2 in sm.get_opcodes():
        if op == "equal":
            continue
        lines.append(f"  {label}: {op} before[{i1}:{i2}] flash[{j1}:{j2}]")
        for k in range(j1, j2):
            changed.add(k)
        for k in range(max(i1, i2 - 3), i2) if op != "insert" else []:
            pass
        n = min(i2 - i1, j2 - j1)
        for t in range(n):
            x, y = a[i1 + t], b[j1 + t]
            d = [f"{FIELDS[f]} {x[f]}->{y[f]}" for f in range(len(x)) if x[f] != y[f]]
            if len(lines) < 120:
                lines.append(f"    draw {j1 + t}: " + ", ".join(d))
        for t in range(n, j2 - j1):
            if len(lines) < 120:
                lines.append(f"    draw {j1 + t} only in flash: {b[j1 + t]}")
        for t in range(n, i2 - i1):
            if len(lines) < 120:
                lines.append(f"    draw {i1 + t} only in {label}: {a[i1 + t]}")
    return changed, lines


def main():
    frames = load(sys.argv[1])
    print(open(sys.argv[1], encoding="latin-1").readline().strip())
    if len(frames) < 3:
        print("needs three frames")
        return
    before, _ = normalise(frames[0], None)
    flash, fb = normalise(frames[1], max(set(r[0] for r in frames[0][1] if r[0] in DISPLAY),
                                         key=lambda k: sum(1 for r in frames[0][1] if r[0] == k), default=None))
    after, _ = normalise(frames[2], fb)
    c1, l1 = diff(before, flash, "before")
    c2, l2 = diff(after, flash, "after")
    both = sorted(c1 & c2)
    print(f"draws: before {len(before)}, flash {len(flash)}, after {len(after)}")
    print(f"flash draws differing from before: {len(c1)}, from after: {len(c2)}, from both: {len(both)}")
    print("differing from both:", both[:80])
    for k in both[:40]:
        print(f"  flash draw {k}: {flash[k]}")
    if "--all" in sys.argv:
        print("\n".join(l1))
        print("\n".join(l2))


if __name__ == "__main__":
    main()
