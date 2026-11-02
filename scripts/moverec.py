# Project Oogie: record Jack's character object (the pointer at 0x420E98;
# position at +0x60/+0x64/+0x68, sub_000CFA60 saves it) ten times a second
# while the live pad walks him around and stops (test tool, GitHub #6 super
# speed), to build_out\mem\NAME_move.npz.
#   py -3 scripts\moverec.py NAME SECONDS [BYTES]
import os, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from memscan import Guest, ROOT

name, secs = sys.argv[1], float(sys.argv[2])
size = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x1000
g = Guest()
pad = os.path.join(ROOT, 'logs', 'pad_live.txt')
# hold a direction ~1.2 s, then stand still ~1.2 s
seq = [('lleft:1200', 1), ('', 0), ('lright:1200', 1), ('', 0), ('lup:1200', 1), ('', 0), ('ldown:1200', 1), ('', 0)]
rows, moving, times, ptrs = [], [], [], []
t0 = time.time(); k = 0; nxt = 0; cur = 0
while time.time() - t0 < secs:
    now = time.time() - t0
    if now >= nxt:
        cmd, cur = seq[k % len(seq)]
        if cmd:
            with open(pad, 'a') as f:
                f.write(cmd + '\n')
        k += 1; nxt += 1.3
    p = int.from_bytes(g.read(0x420E98, 4) or bytes(4), 'little')
    b = g.read(p, size) if 0x10000 <= p < 0x4000000 or 0x80000000 <= p < 0x84000000 else None   # 0x8xxxxxxx: contiguous memory, mapped as is
    rows.append(np.frombuffer(b or bytes(size), dtype='<i4').copy())
    moving.append(cur); times.append(now); ptrs.append(p)
    time.sleep(0.1)
out = os.path.join(ROOT, 'build_out', 'mem', name + '_move.npz')
np.savez(out, m=np.stack(rows), moving=np.array(moving), t=np.array(times), ptr=np.array(ptrs))
print('saved', out, len(rows), 'samples, pointers', sorted(set(ptrs))[:4])
