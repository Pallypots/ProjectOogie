"""Find glitch frames from the brightness numbers in a log (KNOWN_ISSUES #46).

Needs a run with OOGIE_FOG_TRACE=1 and OOGIE_FLASH_MEASURE=1: each [FOG] line
then carries pre= (the scene before post-processing) and final= (what was
shown). A frame whose final brightness jumps away from both neighbours by more
than --jump while they agree is a flash; for each one the script says whether
the scene itself (pre) was already off, and prints the three [FOG] lines.
Usage: python scripts/flash_log.py logs/<run>.err.log [--jump 8] [--lines]
"""
import re
import sys

RX = re.compile(r"\[FOG\] f=(\d+) .*? pre=(-?[0-9.]+) final=(-?[0-9.]+)")


def main():
    path = sys.argv[1]
    jump = float(sys.argv[sys.argv.index("--jump") + 1]) if "--jump" in sys.argv else 8.0
    rows, lines = [], []
    with open(path, "rb") as f:
        for raw in f:
            s = raw.decode("latin-1")
            m = RX.search(s)
            if m:
                rows.append((int(m.group(1)), float(m.group(2)), float(m.group(3))))
                lines.append(s.strip())
    print(f"{len(rows)} measured frames")
    n = 0
    for i in range(1, len(rows) - 1):
        a, b, c = rows[i - 1][2], rows[i][2], rows[i + 1][2]
        if abs(b - a) > jump and abs(b - c) > jump and abs(a - c) < jump / 2:
            n += 1
            pa, pb, pc = rows[i - 1][1], rows[i][1], rows[i + 1][1]
            where = ("scene already off before post-processing"
                     if pb >= 0 and pa >= 0 and abs(pb - pa) > jump / 2 and abs(pb - pc) > jump / 2
                     else "scene normal, changed by post-processing" if pb >= 0 else "no pre value")
            print(f"frame {rows[i][0]}: final {a:.1f} / {b:.1f} / {c:.1f}, "
                  f"pre {pa:.1f} / {pb:.1f} / {pc:.1f}: {where}")
            if "--lines" in sys.argv:
                for k in (i - 1, i, i + 1):
                    print("   ", lines[k][:900])
    print(f"{n} flash frames")


if __name__ == "__main__":
    main()
