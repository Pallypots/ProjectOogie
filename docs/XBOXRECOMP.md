# XboxRecomp Usage for Oogie's Revenge

Version: **v0.14.0** (commit eb0aa1d, released 2026-10-16). MIT. Earlier tags present: v0.9.0, v0.13.1.

## What it provides (per README, checked against `src/`)
Python pipeline (xbe_parser → disasm → func_id → abi_analysis → recomp) plus runtime libraries: xbox_kernel (all 371 ordinals routed), xbox_d3d8 (D3D8→D3D11), xbox_dsound, xbox_apu (xemu MCPX APU, LGPL), xbox_nv2a (push-buffer → D3D11), xbox_input (XInput + keyboard), xbox_usb (OHCI), xbox_video (Media Foundation FMV). Game project comes from `templates/new-game`.

## Exact commands used (see `scripts/regen.sh`)
```
python3 -m tools.xbe_parser game/default.xbe --json game/default_analysis.json
python3 -m tools.disasm game/default.xbe --analysis-json game/default_analysis.json -o build_out/disasm --force [--seed-functions scripts/icall_seeds.json]
python3 -m tools.func_id game/default.xbe --functions ... --strings ... --xrefs ... -o build_out/func_id
python3 -m tools.abi_analysis game/default.xbe --disasm-dir build_out/disasm --func-id-dir build_out/func_id --output-dir build_out/abi
python3 -m tools.recomp game/default.xbe --all --split 1000 --game-name OogiesRevenge --disasm-dir ... --func-id-dir ... --abi-dir ... --exclude-manual port/src/recomp_manual.c --gen-dir port/src/recomp/gen -o build_out/recomp
cmake -S port -B port/build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=<xboxrecomp>/cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build port/build-win -j2
```
Template edits: `port/src/main.c` entry point `0x001BEF07`, XBE path `game/default.xbe`, data dir `game`; `port/CMakeLists.txt` project `oogie_recomp`, `XBOXRECOMP_DIR` path.

## Results on our XBE (2026-10-16)
| Stage | Result | Time (2 cores) |
|---|---|---|
| xbe_parser | OK, 31 sections, 125 imports | <1 s |
| disasm (all code sections) | 804,237 instr, 10,322 functions | 4.5 min |
| func_id | OK | 4 s |
| abi_analysis | 10,322 entries, 318 thiscall | 2 s |
| recomp | **10,367/10,367 translated, 0 failed**, 1,400,178 lines, 11 files; 19 unresolved call targets stubbed; 106 unimplemented instructions (30 mnemonics, almost all junk like aas/arpl/outsb, i.e. data decoded as code) | 2.2 min |
| MinGW build | **links**, `oogie_recomp.exe` 14.7 MB PE32+ x86-64, 10,385 `sub_` symbols | about 8 min |

## Known tool quirks seen
- Duplicate section name BINKYUY2 (4 sections): disasm writes `BINKYUY2.asm` four times (the listing file is overwritten; analysis itself covers all four).
- Exe imports `libwinpthread-1.dll`; ship it beside the exe or link statically.
