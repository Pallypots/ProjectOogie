"""Seeds for code the recompiler never covered, found through switch tables.

scripts/scan_switch_tables.py lists switch jumps whose case targets no function
covers. Many sit in a gap after a function the disassembler ended too early
(KNOWN_ISSUES #45: sub_000622A0 recorded to 0x637D3 while its code runs on;
the Halloween Door freeze, 0x0006A2F8, was one of these). For each such switch
this proposes the gap start (the end of the function before it, when that
address decodes as code) and every uncovered case target, keeps only addresses
that decode cleanly for a few instructions, and prints them in the
scripts/icall_seeds.json format. Nothing is written unless --apply is given.
"""
import json
import os
import struct
import subprocess
import sys

import capstone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XBE = os.path.join(ROOT, "game", "default.xbe")
ANALYSIS = os.path.join(ROOT, "game", "default_analysis.json")
FUNCS = os.path.join(ROOT, "build_out", "disasm", "functions.json")
SEEDS = os.path.join(ROOT, "scripts", "icall_seeds.json")


def main():
    data = open(XBE, "rb").read()
    secs = json.load(open(ANALYSIS))["sections"]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def off(va):
        for s in secs:
            v = int(s["virtual_addr"], 16)
            if s["executable"] and v <= va < v + s["raw_size"]:
                return int(s["raw_addr"], 16) + va - v
        return None

    def decodes(va):
        o = off(va)
        if o is None:
            return False
        n = 0
        for ins in md.disasm(data[o:o + 64], va):
            if ins.mnemonic in ("int3", "hlt", "(bad)") or ins.bytes == b"\x00\x00":
                return False
            n += 1
            if n >= 4 or ins.mnemonic in ("ret", "jmp"):
                return True
        return False

    funcs = json.load(open(FUNCS))
    rng = sorted((int(f["start"], 16), int(f["end"], 16) if isinstance(f["end"], str) else f["end"])
                 for f in funcs)
    starts = {a for a, _ in rng}
    have = {int(x["start"], 16) for x in json.load(open(SEEDS))}

    out = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "scan_switch_tables.py")],
                         capture_output=True, text=True).stdout
    cand = set()
    for line in out.splitlines():
        if " uncovered: " not in line:
            continue
        at = int(line.split()[0], 16)
        targets = [int(t, 16) for t in line.split("uncovered: ")[1].split("(")[0].split()
                   if t != "..." and len(t) == 8]
        cand.update(targets)
        if "(function none)" in line:
            # end of the function before the jump = start of the gap
            prev = [e for s, e in rng if e <= at]
            if prev:
                cand.add(max(prev))
    good = sorted(v for v in cand if v not in starts and v not in have and decodes(v))
    bad = sorted(v for v in cand if v not in starts and v not in have and not decodes(v))
    print(f"{len(good)} seeds proposed, {len(bad)} rejected (do not decode as code):"
          f" {' '.join('%08X' % v for v in bad)}")
    new = [{"start": f"0x{v:08X}", "source": "scan_switch_tables (KNOWN_ISSUES #45)"} for v in good]
    if "--apply" in sys.argv:
        seeds = json.load(open(SEEDS))
        seeds.extend(new)
        json.dump(seeds, open(SEEDS, "w"), indent=1)
        print(f"added to {SEEDS}")
    else:
        print(json.dumps(new[:5], indent=1), "...")


if __name__ == "__main__":
    main()
