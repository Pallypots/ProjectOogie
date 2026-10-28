"""List every D3D-section function whose constants include given NV2A method
headers (default: the draw-delivery methods), with its stack bytes and
callers. Usage: py -3 scripts/d3d_find_methods.py [0x17FC 0x1800 ...]"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, "build_out", "disasm", "asm", "D3D.asm")
want = {int(x, 16) for x in sys.argv[1:]} or {0x17FC, 0x1800, 0x1808, 0x1810, 0x1818}
NAMES = {0x17FC: "BEGIN_END", 0x1800: "ARRAY_ELEMENT16", 0x1808: "ARRAY_ELEMENT32",
         0x1810: "DRAW_ARRAYS", 0x1818: "INLINE_ARRAY"}
cur, called_by, hits, rets = None, {}, {}, {}
for line in open(ASM, encoding="utf-8", errors="replace"):
    m = re.match(r"^; Called by: (.*)", line)
    if m:
        pending_callers = m.group(1).strip()
        continue
    m = re.match(r"^sub_([0-9A-F]{8}):", line)
    if m:
        cur = int(m.group(1), 16)
        called_by[cur] = locals().get("pending_callers", "")
        continue
    if cur is None:
        continue
    m = re.match(r"^\s+0x[0-9A-F]{8}\s+[0-9a-f]+\s+(\S+)\s*(.*?)\s*(;.*)?$", line)
    if not m:
        continue
    if m.group(1) == "ret":
        rets.setdefault(cur, set()).add(m.group(2) or "0")
    for h in re.findall(r"0x([0-9a-f]{4,8})\b", m.group(2)):
        v = int(h, 16)
        meth = v & 0x1FFC
        if meth in want and not v & 3 and (v >> 18) & 0x7FF >= 1 and v >> 29 in (0, 2):
            hits.setdefault(cur, set()).add("%s(%s)" % (NAMES.get(meth, hex(meth)), "loop" if v >> 30 else (v >> 18) & 0x7FF))
for f in sorted(hits):
    print("sub_%08X ret %-8s %s\n    called by: %s" % (f, ",".join(sorted(rets.get(f, {"?"}))), " ".join(sorted(hits[f])), called_by.get(f, "")[:160]))
