# Project Oogie: compare memory snapshots from scripts\memscan.py snap.
#   py -3 scripts\memdiff.py NAME [kind]
# Loads build_out\mem\NAME_*.bin in order and lists 4-byte values (int and
# float) that change only a few times, with their history, by kind:
#   up    only ever goes up (money, score)
#   down  goes down, maybe back up to its first value (health)
#   all   any value that changes 1..6 times
# Optional: LO HI (Xbox address range) and FIRST (first snapshot to use).
import glob, os, struct, sys
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
name = sys.argv[1]
kind = sys.argv[2] if len(sys.argv) > 2 else 'all'
files = sorted(glob.glob(os.path.join(ROOT, 'build_out', 'mem', name + '_*.bin')))
lo = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
hi = int(sys.argv[4], 0) if len(sys.argv) > 4 else 1 << 32
first = int(sys.argv[5]) if len(sys.argv) > 5 else 0
files = files[first:]
snaps, start = [], None
for f in files:
    raw = open(f, 'rb').read()
    start = struct.unpack_from('<I', raw)[0]
    snaps.append(np.frombuffer(raw, dtype='<i4', offset=8))
n = min(len(s) for s in snaps)
m = np.stack([s[:n] for s in snaps])          # [snap, dword]
d = np.diff(m, axis=0)
changes = (d != 0).sum(axis=0)
va_all = start + np.arange(n, dtype=np.int64) * 4
cand = (changes >= 1) & (changes <= 6) & (va_all >= lo) & (va_all < hi)
# plausible small integers only (pointers and floats stay out of the int list)
small = (np.abs(m).max(axis=0) < 1000000)
if kind == 'up':
    sel = cand & small & (d >= 0).all(axis=0) & (m[-1] > m[0])
elif kind == 'down':
    sel = cand & small & (m.min(axis=0) < m[0]) & (m.max(axis=0) <= m[0])
else:
    sel = cand & small
idx = np.nonzero(sel)[0]
print('%d snapshots, %d candidates' % (len(files), len(idx)))
for i in idx[:400]:
    va = start + i * 4
    hist, last = [], None
    for k, v in enumerate(m[:, i]):
        if v != last:
            hist.append('%d@%d' % (v, k))
            last = v
    print('%08X  %s' % (va, ' '.join(hist)))
