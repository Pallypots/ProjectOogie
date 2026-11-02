# Project Oogie: find values that fall when Jack's hits land (test tool,
# GitHub #6 one-shot kill). Run while a chapter is being played (the game
# started by scripts\hitscan.ps1): reads the current combo (0x37EEBE, u16,
# sub_00025D50 adds each hit) and a block of guest memory a few times a
# second, presses attack on the live pad, and scores every dword by how often
# it went down in an interval where the combo went up, against how often it
# went down otherwise. Prints the best candidates with their history.
#   py -3 scripts\hitscan.py SECONDS [START END]
import os, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from memscan import Guest, ROOT

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 120
start = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x3B2000
end = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x2000000
g = Guest()
pad = os.path.join(ROOT, 'logs', 'pad_live.txt')
seq = ['x:100', 'x:100', 'x:100', 'lleft:250', 'x:100', 'x:100', 'lright:250', 'x:100', 'y:100', 'x:100', 'lup:250', 'x:100']

def grab():
    parts = []
    for va in range(start, end, 0x10000):
        b = g.read(va, 0x10000)
        parts.append(b if b and len(b) == 0x10000 else bytes(0x10000))
    return np.frombuffer(b''.join(parts), dtype='<i4').copy()

def combo():
    b = g.read(0x37EEBE, 2)
    return int.from_bytes(b, 'little', signed=True) if b else 0

first = grab()
n = len(first)
hit_i = np.zeros(n, np.int32); miss_i = np.zeros(n, np.int32)
hit_f = np.zeros(n, np.int32); miss_f = np.zeros(n, np.int32)
prev, pc = first, combo()
hits = intervals = 0
t0 = time.time(); k = 0
hist_t = []
while time.time() - t0 < secs:
    with open(pad, 'a') as f:
        f.write(seq[k % len(seq)] + '\n')
    k += 1
    time.sleep(0.25)
    cur, cc = grab(), combo()
    intervals += 1
    di = cur < prev
    pf, cf = prev.view('<f4'), cur.view('<f4')
    with np.errstate(all='ignore'):
        df = np.isfinite(pf) & np.isfinite(cf) & (cf < pf) & (pf > 0) & (pf < 1e5)
    if cc > pc:
        hits += 1; hit_i += di; hit_f += df
    else:
        miss_i += di; miss_f += df
    prev, pc = cur, cc
va = start + np.arange(n, dtype=np.int64) * 4
print('intervals %d, with hits %d' % (intervals, hits))
for name, h, m in (('int', hit_i, miss_i), ('float', hit_f, miss_f)):
    score = h - 3 * m
    best = np.argsort(-score)[:25]
    print('==', name)
    for i in best:
        if h[i] < 2: continue
        v0, v1 = first[i], prev[i]
        if name == 'float':
            v0, v1 = round(float(first[i:i+1].view('<f4')[0]), 2), round(float(prev[i:i+1].view('<f4')[0]), 2)
        print('%08X  down with hits %d, without %d, first %s, last %s' % (va[i], h[i], m[i], v0, v1))
