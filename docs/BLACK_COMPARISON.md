# BLACK Port Comparison

Reviewed 2026-10-16. Both repos target XDK 1.0.5849, the **same XDK version as Oogie's Revenge** (Verified from both XBEs' library tables).

## A. portingpete/Black-2006-Recomp (commit df818f3, v0.1.0-alpha.2)
**Approach:** whole-XBE static recompilation with XboxRecomp, including the D3D, DSOUND, XMV, WMADEC library sections. The game's own D3D8 code runs as C and writes real NV2A push buffers; an NV2A "Kelvin" emulator renders them through D3D11. Audio runs the xemu MCPX APU emulation to XAudio2.

**Pipeline:** `scripts/setup.ps1` clones xboxrecomp pinned at **v0.11.0 (766ecef)** and applies `patches/xboxrecomp.patch` (about 46k lines). `scripts/generate.py` runs xbe_parser → disasm (with `--seed-functions` from a hand list plus a runtime indirect-call log) → func_id → abi_analysis → recomp `--all --split 1000 --exclude-manual`, then regex post-passes (`promote`, `repair_sse`, `title_hooks`).

**Reusable for Oogie:** the generic layers inside its patch (Kelvin renderer `src/nv2a/kelvin/*`, APU/DSP fixes, USB/OHCI pad, kernel fixes, `fb_present`), the indirect-call feedback loop, and the post-passes. **BLACK-specific:** `src/black_*`, most of `recomp_manual.c`, address-keyed hooks.

**Important:** the Kelvin renderer is **not in upstream v0.14.0**. Upstream ships its own NV2A→D3D11 path (`nv2a_pgraph_d3d11.c`). If upstream's path does not render Oogie, porting Kelvin from BLACK's patch is plan B.

**License:** MIT; xemu-derived APU/Kelvin files are LGPL-2.1+.

**Bink/XACT:** not present in BLACK (it uses XMV and EA audio). Under the same whole-XBE model, Oogie's Bink and XACT code runs recompiled and reaches the emulated hardware the same way (Hypothesis until tested).

## B. Spark198rus/black-pc (commit 4c84944, v0.2.0, "fully playable")
**Approach:** direct x86 execution. A 32-bit stub exe linked at fixed base 0x10000 reserves the Xbox address range; the XBE is copied over it; 64 MB committed at 0x80000000; Xbox-only `fs:` accesses are rewritten to Windows TLS slots; kernel thunks are patched to C++ HLE functions. XDK D3D/DSOUND/XPP functions are found offline (XbSymbolDatabase + Ghidra) and hooked with 5-byte jumps to HLE code (D3D9 renderer, software DSound mixer to XAudio2).

**Reusable:** loader stub, XBE mapper, fs-rewrite, kernel HLE, hook dispatcher, NV2A shader translators, CLAUDE.md/NOTES/TODO conventions.
**Not reusable as-is:** every address table (must be regenerated for Oogie), D3D HLE tables keyed to BLACK addresses.
**Drawbacks:** 32-bit only, MSVC-only inline asm, locked to one XBE hash, runtime code patching, debugging on machine code.
**License:** GPL-2.0-or-later (cannot be mixed into MIT code without relicensing the port to GPL).

## Decision
Use **Approach A (XboxRecomp whole-XBE)**. Evidence: our XBE parses, disassembles, translates with 0 failures, links into a Windows exe, and runs its own boot code to D3D init on the first try (see `TEST_RESULTS.md`). Approach B stays as a fallback reference for specific HLE ideas, not as a code source.
