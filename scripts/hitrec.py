# Project Oogie: record small blocks of guest memory ten times a second while
# the bot attacks (test tool, GitHub #6 one-shot kill), with the current combo
# (0x37EEBE), to build_out\mem\NAME_rec.npz for offline reading.
#   py -3 scripts\hitrec.py NAME SECONDS START:END [START:END ...]
import os, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from memscan import Guest, ROOT

name, secs = sys.argv[1], float(sys.argv[2])
ranges = [tuple(int(x, 0) for x in r.split(':')) for r in sys.argv[3:]]
g = Guest()
pad = os.path.join(ROOT, 'logs', 'pad_live.txt')
seq = ['x:100', 'x:100', 'x:100', 'lleft:250', 'x:100', 'x:100', 'lright:250', 'x:100', 'y:100', 'x:100', 'lup:250', 'x:100']
rows, combos, times = [], [], []
t0 = time.time(); k = 0; nxt = 0
while time.time() - t0 < secs:
    now = time.time() - t0
    if now >= nxt:
        with open(pad, 'a') as f:
            f.write(seq[k % len(seq)] + '\n')
        k += 1; nxt += 0.3
    parts = [g.read(a, b - a) or bytes(b - a) for a, b in ranges]
    rows.append(np.frombuffer(b''.join(parts), dtype='<i4').copy())
    c = g.read(0x37EEBE, 2)
    combos.append(int.from_bytes(c, 'little', signed=True) if c else 0)
    times.append(now)
    time.sleep(0.1)
va = np.concatenate([np.arange(a, b, 4) for a, b in ranges])
out = os.path.join(ROOT, 'build_out', 'mem', name + '_rec.npz')
np.savez(out, m=np.stack(rows), combo=np.array(combos), t=np.array(times), va=va)
print('saved', out, len(rows), 'samples, combo rises', int((np.diff(combos) > 0).sum()))
