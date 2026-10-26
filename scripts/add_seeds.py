#!/usr/bin/env python3
"""Append unresolved indirect-call targets from a run log to scripts/icall_seeds.json."""
import json, re, sys, os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
db = os.path.join(root, "scripts", "icall_seeds.json")
seeds = json.load(open(db)) if os.path.exists(db) else []
have = {int(s["start"], 16) for s in seeds}
new = []
for log in sys.argv[1:]:
    for m in re.finditer(r"Failed to resolve VA 0x([0-9A-Fa-f]+)", open(log, errors="replace").read()):
        a = int(m.group(1), 16)
        if a < 0xFE000000 and a not in have:
            have.add(a); new.append(a); seeds.append({"start": f"0x{a:08X}", "source": os.path.basename(log)})
json.dump(seeds, open(db, "w"), indent=1)
print("added", [hex(a) for a in new], "total", len(seeds))
