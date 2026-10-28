"""Project Oogie, HLE renderer stage 1: what each D3D8 entry point does.

For every entry point in docs/D3D8_API_INVENTORY.md, reads its instructions
from build_out/disasm/asm/D3D.asm and decodes the constants that are NV2A
push-buffer method headers ((count << 18) | (subchannel << 13) | method),
naming the methods from third_party/xboxrecomp/src/nv2a/nv2a_regs.h; does
the same one call level down inside the D3D section. Also reports the
stack bytes the function pops (ret N) and its call rate from a log with
[D3DCALLS] lines (OOGIE_D3D_CALLS=1).

Usage: py -3 scripts/d3d_identify.py [log with D3DCALLS lines] > out.txt
"""
import collections
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, "build_out", "disasm", "asm", "D3D.asm")
REGS = os.path.join(ROOT, "third_party", "xboxrecomp", "src", "nv2a", "nv2a_regs.h")
INV = os.path.join(ROOT, "docs", "D3D8_API_INVENTORY.md")
D3D_LO, D3D_HI = 0x001D1B80, 0x001E4AE8

names = {}
for m in re.finditer(r"#\s*define\s+NV097_([A-Z0-9_]+)\s+0x([0-9A-Fa-f]+)", open(REGS, encoding="utf-8", errors="replace").read()):
    v = int(m.group(2), 16)
    if v < 0x2000 and v not in names:
        names[v] = m.group(1)


def method_name(m):
    if m in names:
        return names[m]
    best = max((k for k in names if k <= m and m - k < 0x40), default=None)   # arrays: BASE + i*stride
    return "%s+0x%X" % (names[best], m - best) if best is not None else None


# functions: name -> list of (addr, mnemonic, operands)
funcs = collections.OrderedDict()
cur = None
ins = re.compile(r"^\s+0x([0-9A-F]{8})\s+[0-9a-f]+\s+(\S+)\s*(.*?)\s*(;.*)?$")
for line in open(ASM, encoding="utf-8", errors="replace"):
    m = re.match(r"^sub_([0-9A-F]{8}):", line)
    if m:
        cur = int(m.group(1), 16)
        funcs[cur] = []
        continue
    m = ins.match(line)
    if m and cur is not None:
        funcs[cur].append((int(m.group(1), 16), m.group(2), m.group(3)))


def methods_in(fva):
    out = []
    for _a, mn, ops in funcs.get(fva, []):
        if mn not in ("mov", "push", "or", "lea"):
            continue
        for h in re.findall(r"0x([0-9a-f]{5,8})\b", ops):
            v = int(h, 16)
            count, sub, meth = (v >> 18) & 0x7FF, (v >> 13) & 7, v & 0x1FFC
            if v & 3 or v >> 29 not in (0, 2) or not 1 <= count <= 128 or meth < 0x100:
                continue
            n = method_name(meth)
            if n:
                out.append("%s(%d)" % (n, count))
    return out


def callees(fva):
    out = []
    for _a, mn, ops in funcs.get(fva, []):
        m = re.search(r"0x([0-9a-f]{5,8})", ops) if mn == "call" else None
        if m and D3D_LO <= int(m.group(1), 16) < D3D_HI:
            out.append(int(m.group(1), 16))
    return out


def ret_bytes(fva):
    rets = {ops for _a, mn, ops in funcs.get(fva, []) if mn == "ret"}
    return ",".join(sorted(r or "0" for r in rets)) or "?"


rates = {}
if len(sys.argv) > 1:
    last = [l for l in open(sys.argv[1], encoding="utf-8", errors="replace") if l.startswith("[D3DCALLS]")]
    if last:
        for va, r in re.findall(r"([0-9A-F]{8}):([0-9.]+)", last[-1]):
            rates[int(va, 16)] = float(r)

entries = [int(x, 16) for x in re.findall(r"^\| 0x([0-9A-F]{8}) \|", open(INV, encoding="utf-8").read(), re.M)]
for e in entries:
    own = methods_in(e)
    deep = []
    for c in callees(e):
        deep += methods_in(c)
    ins_n = len(funcs.get(e, []))
    rate = rates.get(e)
    print("0x%08X  ret %-5s %4d insns  %s  own: %s | callees: %s" % (
        e, ret_bytes(e), ins_n, ("%8.1f/frame" % rate) if rate else "        -     ",
        ", ".join(collections.OrderedDict.fromkeys(own)) or "-",
        ", ".join(collections.OrderedDict.fromkeys(deep)) or "-"))
