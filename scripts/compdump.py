# Project Oogie: list the component records of game objects (test tool,
# GitHub #6 one-shot kill). An object keeps a list at [obj+0x40] of records
# {+0 id, +8 next}; sub_0001D960(obj, id) finds one. Prints each record's id
# and the function pointers a hit goes through (+0x2C: on hit, +0x29C: the
# enemy's own damage routine, called from sub_00112AA0).
#   py -3 scripts\compdump.py OBJ [OBJ ...]
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from memscan import Guest

g = Guest()
def r32(a):
    b = g.read(a, 4)
    return struct.unpack('<I', b)[0] if b else None
for o in sys.argv[1:]:
    obj = int(o, 0)
    e, n = r32(obj + 0x40), 0
    print('object %08X' % obj)
    while e and n < 32:
        cid, nxt = r32(e), r32(e + 8)
        print('  record %08X id %3d  +0x2C %08X  +0x29C %08X  next %08X' % (e, cid or 0, r32(e + 0x2C) or 0, r32(e + 0x29C) or 0, nxt or 0))
        e, n = nxt, n + 1
