"""Find switch jump tables whose case targets no recompiled function covers.

A `jmp dword ptr [reg*4 + TABLE]` lifts to a direct switch only when the
targets fall inside the function; otherwise the recompiled code dispatches the
target at run time and logs "ICALL Failed to resolve VA ..." when no function
starts there. KNOWN_ISSUES #45: Chapter 1's Halloween Door screen froze on
0x0006A2F8 because sub_0006A2E0 was cut off right after its indirect jump.

Reads game/default.xbe (read-only), build_out/disasm/functions.json and the
disassembly listings. Prints each table, its targets, and which are neither a
function start nor inside the function holding the jump; with --seeds prints
those targets as a JSON list for scripts/icall_seeds.json.
"""
import glob
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XBE = os.path.join(ROOT, "game", "default.xbe")
ANALYSIS = os.path.join(ROOT, "game", "default_analysis.json")
FUNCS = os.path.join(ROOT, "build_out", "disasm", "functions.json")
ASM = os.path.join(ROOT, "build_out", "disasm", "asm")

JMP = re.compile(r"^\s+0x([0-9A-Fa-f]{8})\s+[0-9a-f]+\s+jmp\s+dword ptr \[(\w+)\*4 \+ 0x([0-9a-f]+)\]")


def main():
    data = open(XBE, "rb").read()
    secs = json.load(open(ANALYSIS))["sections"]

    def read32(va):
        for s in secs:
            v = int(s["virtual_addr"], 16)
            if v <= va < v + s["raw_size"]:
                off = int(s["raw_addr"], 16) + va - v
                return struct.unpack_from("<I", data, off)[0]
        return None

    def is_code(va):
        return any(s["executable"] and int(s["virtual_addr"], 16) <= va
                   < int(s["virtual_addr"], 16) + s["virtual_size"] for s in secs)

    funcs = json.load(open(FUNCS))
    ranges = sorted((int(f["start"], 16), int(f["end"], 16) if isinstance(f["end"], str)
                     else f["end"]) for f in funcs)
    starts = {a for a, _ in ranges}
    seeds = json.load(open(os.path.join(ROOT, "scripts", "icall_seeds.json")))
    seeded = {int(x["start"], 16) for x in seeds}

    def owner(va):
        lo, hi = 0, len(ranges)
        while lo < hi:
            mid = (lo + hi) // 2
            if ranges[mid][0] <= va:
                lo = mid + 1
            else:
                hi = mid
        if lo and ranges[lo - 1][0] <= va < ranges[lo - 1][1]:
            return ranges[lo - 1]
        return None

    missing = []
    for path in glob.glob(os.path.join(ASM, "*.asm")):
        for line in open(path, encoding="utf-8", errors="replace"):
            m = JMP.match(line)
            if not m:
                continue
            at, table = int(m.group(1), 16), int(m.group(3), 16)
            own = owner(at)
            targets = []
            for k in range(256):
                t = read32(table + 4 * k)
                if t is None or not is_code(t) or (own and abs(t - at) > 0x10000):
                    break
                targets.append(t)
            bad = [t for t in targets if t not in starts and t not in seeded
                   and not (own and own[0] <= t < own[1])]
            if bad:
                missing.extend(bad)
                print(f"{at:08X} table {table:08X}: {len(targets)} targets, "
                      f"{len(bad)} uncovered: {' '.join(f'{t:08X}' for t in bad[:12])}"
                      f"{' ...' if len(bad) > 12 else ''}  (function "
                      f"{'%08X-%08X' % own if own else 'none'})")
    uniq = sorted(set(missing))
    print(f"\n{len(uniq)} uncovered case targets")
    if "--seeds" in sys.argv:
        print(json.dumps([f"0x{t:08X}" for t in uniq]))


if __name__ == "__main__":
    main()
