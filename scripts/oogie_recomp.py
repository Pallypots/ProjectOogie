"""Run XboxRecomp's `tools.recomp` with Oogie's lifter fixes applied in memory.

The third_party/xboxrecomp checkout stays unmodified (CLAUDE.md); this is the
code-generation counterpart of the build-time runtime patches in port/patches/.
scripts/regen.ps1 runs it from the checkout folder in place of
`python -m tools.recomp`, with the same arguments.

Fixes (each is an upstream bug, written up in docs/KNOWN_ISSUES.md):

  #35  movsx r32, bp / movsx r32, sp lifted as a ZERO extension. Lifter.
       _lift_movsx wraps the 16-bit source in SX16() only for ax/bx/cx/dx/si/
       di; bp and sp fall through to the plain LO16() read. Verified with
       tools/binkdiff: in the Bink inverse DCT sub_00235BA0, `movsx ebx, bp`
       (0x235C49) became `ebx = LO16(ebp)`, so negative coefficients turned
       into large positive ones: the speckled 8x8 blocks in every movie.
       17 sites in this XBE (8 Bink, 1 WMA decoder, 8 game code).
"""
import os
import runpy
import sys

# Run as a script, Python puts scripts/ first on the path, not the checkout
# folder regen.ps1 runs us from.
sys.path.insert(0, os.getcwd())
from tools.recomp import lifter  # noqa: E402

_orig_movsx = lifter.Lifter._lift_movsx


def _lift_movsx(self, insn, ops):
    if len(ops) >= 2 and ops[1].type == "reg" and ops[1].reg in ("bp", "sp"):
        src = f"SX16({lifter._fmt_operand_read(ops[1])})"
        return [lifter._fmt_operand_write(ops[0], src)]
    return _orig_movsx(self, insn, ops)


lifter.Lifter._lift_movsx = _lift_movsx

if __name__ == "__main__":
    sys.argv[0] = "tools.recomp"
    runpy.run_module("tools.recomp", run_name="__main__", alter_sys=True)
