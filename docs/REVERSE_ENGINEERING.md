# Reverse Engineering Notes: default.xbe

All facts below are **Verified** from `logs/xbe_parser.log`, `logs/disasm.log` and run logs unless marked **Hypothesis**.

## Source image
| Item | Value |
|---|---|
| Image | `Tim Burton's The Nightmare Before Christmas - Oogie's Revenge (USA).xiso.iso`, 3,498,311,680 bytes |
| Made from Redump | MD5 509aa949d60777ac01bb1c80fd1a3bc0, SHA-1 b39ba1e3f4d0088fdbb7b4261491d580ca535011 (per Vimm's Lair.txt) |
| XDVDFS | partition base 0x0, root dir sector 264, size 200 |
| Files | 723 entries (`docs/game_inventory.tsv`) |

## default.xbe
| Item | Value |
|---|---|
| Size | 3,751,936 bytes |
| SHA-1 | 667c5bd8abc4a551ef6b7cbab3bf3a45e7244605 |
| SHA-256 | 9a8319c7b1cb5ee29781bf641a0cf549c221fdc57effb8f72085608019f41548 |
| MD5 | 9513bdeac7617654666a38817c9f1bb0 |
| Title | "Tim Burton's The Nightmare Before Christ" (truncated in cert), ID 0x42560004, region NA, Retail, disc 0 v1 |
| Build date | 2005-07-25 02:20:04 UTC; cert 2005-08-05 |
| Debug path | `c:\Project\nightmareXbox\xbox\project\xnightmare\xnightmare\Release_US\xnightmare.exe` |
| Base / image size | 0x00010000 / 0x004403C0 |
| Entry point | **0x001BEF07** (raw 0xA8E7B8AC, retail XOR) |
| Kernel thunk table | 0x0025F8E0 (raw 0x5B48B856), 125 imports |
| TLS | index addr 0x0041279C, zero-fill 144 bytes |
| Stack commit | 0x10000 |

### Libraries (all XDK 1.0.5849)
XAPILIB, D3D8, D3DX8, XGRAPHC, DSOUND, XACTENG, XBOXKRNL, LIBCMT, LIBCPMT.
Bink (RAD) and WMADEC/DOLBY code is present as sections but not in the library table.

### Sections (31)
`.text` 0x00012000 (1.75 MB code), D3D 0x001D1B80, D3DX 0x001E4B00, XGRPH 0x00201960, DSOUND 0x00203A60, XACTENG 0x00222380, BINK 0x0022CEA0 plus 15 small BINK* pixel-format sections (BINKYUY2 appears **four times** with the same name), WMADEC 0x0023EEC0, XPP 0x00257F40, .rdata 0x0025F8E0, .data 0x00280340 (1.72 MB virtual), DOLBY 0x00437DC0, BINKDATA 0x0043EF40, $$XTIMAGE, $$XSIMAGE, .XTLID. Full table in `logs/xbe_parser.log`.

### Kernel imports (125)
Av 4, Ex 5, Hal 8, Io 9, Ke 21, Kf 2, Mm 11, Nt 33, Ob 2, Ps 3, Rtl 12, Xbox 8, Xc 4, other 3 (DbgPrint, LaunchDataPage, IdexChannelObject). Notable: `XeLoadSection`/`XeUnloadSection` (dynamic section loading), `KeConnectInterrupt` (GPU interrupt), `MmClaimGpuInstanceMemory`, `IoCreateDevice`/`IoStartPacket` (XAPI drivers), `XcSHA*`/`XcHMAC` (save signing).

## Disassembly (XboxRecomp v0.14.0, all code sections)
- 804,237 instructions, 10,322 functions (5,399 call targets, 2,909 cc boundaries, 908 prologues), 162,191 xrefs, 2,499 strings, 876 jump tables resynced.
- Recursive-descent reachable: 65.6% of swept instructions.
- func_id: 318 thiscall functions detected; categories in `build_out/func_id/summary.json`.

## Runtime-discovered functions (indirect-call targets)
Measured by running the exe (Verified, run1):
| VA | Section | Seen from |
|---|---|---|
| 0x001D4320 | D3D | first D3D device init, after KeConnectInterrupt vector 3 |
| 0x001D4B70 | D3D | same |
| 0x001D59E0 | D3D | same |

## Boot sequence observed (run1, Verified)
1. Entry 0x001BEF07 creates a system thread (routine 0x001BD404, ctx 0x001BEE93).
2. CRT/heap init: NtAllocateVirtualMemory 1 MB reserve then commits.
3. About 90 alternating calls to ordinal 289 `RtlInitAnsiString` (from 0x001C07B5) and 279 `RtlEqualString` (from 0x001C07C5): a string-table lookup loop, not a hang (Verified ordinals; purpose **Hypothesis**).
4. Opens `UDATA\42560004\TitleMeta.xbx`, `TitleImage.xbx`, `SaveImage.xbx` (save metadata), reads partition0 sector, queries partition5 (cache), looks for `TDATA\42560004\$u\contentmeta.xbx` (not found, normal on first run).
5. Allocates 4 KB, 1 MB, 3600 KB, 1200 KB contiguous memory (**Hypothesis**: push buffer and frame buffers), connects GPU interrupt (vector 3, ISR 0x001DB020).
6. Executes `wbinvd` at 0x001D9F34 (D3D section, cache flush; no-op is fine).

## Identified functions and globals (Verified by disassembly)
| Address | What | Evidence |
|---|---|---|
| 0x001E25E8 | XDK D3D global device pointer (`g_pDevice`) | `mov eax,[0x1e25e8]` throughout D3D section; run4 device = 0x001E25F0 |
| 0x001D78D0 | D3D fence wait (`D3D_BlockOnTime`): reads device+0x30 (semaphore ptr) and device+0x2C (write seq) | D3D.asm; matches xboxrecomp d3d8ltcg-device-context.md |
| 0x001D7BE0 | Block until idle: `D3D_BlockOnTime(device->fence(+0x2C), 2)` | D3D.asm |
| 0x001D7810 | Kickoff called by the fence wait when waiting on the newest fence | D3D.asm |
| 0x80001000 | GPU semaphore (device+0x30 target) at run time | run4 log |
| 0x001BD332 | XAPI `SwitchToThread`: `NtYieldExecution() != 0x40000024` | text.asm |
| 0x001BD012 | XAPI `WaitForSingleObjectEx` wrapper (returns 0x102 timeout / 0xC0 APC) | text.asm, sub_001A7BA0 |
| 0x001A7BA0 | "Drain" loop: enter CS [0x40B0E0], D3D call, then `while (SwitchToThread()) WaitForSingleObjectEx([0x40B0E4],0,TRUE)` | text.asm |
| 0x000586E0 | Loader-state step; 0x003E3F64 = state (1 loading, 2 done), worker ctx 0x000584E0 | text.asm, run7c thread ctx |
| 0x001BD404 | Thread start trampoline used for all PsCreateSystemThreadEx calls | run logs |
| 0x001BCE35 | XAPI CreateFile call site (all game file opens come from here) | run logs |
| 0x00208AD1 | DSOUND/APU interrupt service routine (vector 5) | run7c |
| 0x0020EB76 | Interrupt routine for vector 6 (Hypothesis: USB/OHCI) | run7c |
| 0x001DB020 | NV2A GPU interrupt routine (vector 3) | run1 |

## Session 2 additions (Verified by disassembly, `build_out/disasm/asm/`)
| Address | What | Evidence |
|---|---|---|
| 0x001D75F0 | D3D "GPU position" helper `(device, wait)`: with wait != 0 spins until `(*(dev+0x30) << 2 ^ PGRAPH[0x400B10]) & 0x7C == 0` | D3D.asm; win_run1 watchdog return 0x001D79AF |
| 0x001D7670 | D3D kickoff flush: sets bit 0x10000 in `[dev+0x1C28]+0x100410`, spins until cleared | D3D.asm |
| 0x0003DF90 | Boot sequence: starts job 0x0003DD20 via sub_00058540, then `while (sub_00057E90()) { sub_000583C0(); sub_00052520(); }` (Now Loading loop) | text.asm |
| 0x00052520 | Draws one Now Loading frame (3 quads via sub_001A5990), then sub_000586E0 | text.asm; win_run2 stack 0x0005273D |
| 0x00057E90 | `job_running()`: returns `[0x3E3F7C]` | text.asm |
| 0x00058540 | Start background job: state=0, CreateThread(0x000584E0, 0x3E3F64, CREATE_SUSPENDED), job.func=arg, ResumeThread, state=2 | text.asm 0x58587..0x58604 |
| 0x000584E0 | Job worker: calls job.func(job.arg), then clears the job record at 0x3E3F64 (+0x18 = func) and exits via sub_001AD520 | text.asm |
| 0x003E3F64 | Job record: +0 state, +4 thread handle (0x3E3F68), +8 (0x3E3F6C), +0x18 func (0x3E3F7C), +0x1C arg | text.asm |
| 0x001AD720 | Scheduler thread create: XAPI CreateThread(..., flags 4, &tid), fills a 0x18-byte entry in table 0x40EDD8 (+0 handle, +4 tid, +0xC state) | text.asm |
| 0x001AD3D0 | Scheduler resume by tid (sets entry state 2, ResumeThread) | text.asm |
| 0x001AD4B0 / 0x001AD4F0 | Scheduler suspend / resume by tid through table 0x40EDD8 | text.asm |
| 0x001AD260 | Scheduler init: `DuplicateHandle(-2 current thread, options 0)` into table slot 0 | text.asm 0x1AD284 |
| 0x001BD49C | XAPI CreateThread -> PsCreateSystemThreadEx (CreateSuspended = flags bit 2) | text.asm |
| 0x001BD290 / 0x001BD2B6 | XAPI SuspendThread / ResumeThread (NtSuspendThread / NtResumeThread) | text.asm |
| 0x001BD50A | XAPI GetCurrentThreadId: `[fs:[0x28] + 0x12C]` | text.asm |
| 0x001BC71B | XAPI DuplicateHandle -> NtDuplicateObject(src, &dst, options) | text.asm |
| 0x0025F8E0 | Kernel thunk table base (slot n = 0xFE000000 + 4n at run time) | default_analysis.json |
| 0x001AD3A0 | Scheduler "should this thread stop?": 1 if the caller's ID is not in table 0x40EDD8 or its entry state is 3 | text.asm |
| 0x001AD440 | Scheduler kill by tid (entry state = 3, signal its event) | text.asm; win_run8 logs |
| 0x001AD520 / 0x001AD540 | Scheduler thread exits (unregister, then XAPI ExitThread) | text.asm |
| 0x0019D8E0 | Game-logic thread: sets game modes via sub_001A0D30, exits when sub_001AD3A0 says so | text.asm, win_run7 |
| 0x0003E0F0 | Main loop after boot: `for (;;) { input; update; sub_00049470; sub_000ED350; ...; sub_0019C4D0; sub_001C7EE1 }` | text.asm, win_run6 samples |
| 0x001A0660 / 0x001A0970 | Movie player: fade, then `while (sub_001A7DC0()) { draw frame sub_001A7E30; input; scheduler }` | text.asm |
| 0x001A7DC0 | Movie frame step: bink = [0x40B148]; decode (sub_0022E730); stop at last frame unless looping ([0x403974]); next frame (sub_0022E980) | text.asm; win_run12 counts |
| 0x0040B148 | Current Bink handle; +8 frame count, +0xC current frame (mv_00: 6775 frames) | win_run12 log |
| 0x001A7C10 | Movie open (calls Bink 0x0022D300 etc.) | text.asm |
| BINK section 0x0022CEA0..0x00237000 | Statically linked Bink; worker threads start at 0x0022F360 | section table, win_run9 |
| 0x00234AF0 | Bink frame planes: calls the plane decoder 3x (Y, then two chroma planes); caller passes 11 cdecl args | BINK.asm 0x234B6D/0x234BDB/0x234C30 |
| 0x00232C40 | Bink plane decoder (7781 bytes). Args seen (bcap2, Y plane): dst 0x82FD1000, previous frame 0x82F60000, width 640, height 480, pitch 640, per-block state 0x011D6AA0 (1200 bytes = 40x30 16x16 blocks), ... Reaches only BINK-section code; calls 0x231C60 or 0x231F30 through [ebp-0x94] | BINK.asm, bcap2, tools/binkdiff |
| 0x00235BA0 | Bink 8x8 inverse DCT + add to prediction (columns pass, then rows with +0x7F, >>8, byte add); stdcall, `ret 0x14`; multiplier table 0x0027DC00 + arg*256 (Hypothesis: dequantisation by quality level) | BINK.asm, binkdiff (#35) |
| 0x00235470, 0x002357A0, 0x00235F30, 0x00236840 | Other Bink block transforms in the same family (movsx of bp at 0x235508, 0x235848, 0x236162...) | BINK.asm |

## Open questions
- Is BINKDATA code or data? It is flagged W and was swept as code.
- The 106 "unimplemented" instructions (aas, arpl, outsb, bound...) are almost certainly data mis-decoded as code. Confirm none are reached.

## .stm archives and menu art (2026-10-23, Verified by reading game/stm_data/sbuty_u.stm)
- `.stm`: 32-byte entries. Entry 0 is the header: name "@STM", then u32 data offset (0x580 in sbuty_u.stm) and u32 root entry count (6). Root entries follow at 0x20. Each entry: 24-byte name, u32 offset, u32 size; size with bit 31 set is a folder whose (size & 0x7FFFFFFF) entries start at offset.
- `.pig` files inside are plain DDS images ("DDS " magic, 124-byte header), e.g. bg/pig/sbback00.pig 512x512, common/pig/button00.pig (button glyphs), common/pig/exword00.pig (prompt words), menu/pig/sbmenu01..12.pig (pause menu frames, cursor).
- `.anb` (magic 0x01235678 then "ANB") hold the sprite layout/animation for the .pig sheets: format not decoded yet.
- `.bms` are message tables (hint_u.bms, item_m_u.bms, item_n_u.bms in sbuty_u.stm).
- Source of the idea: the rival port's menu uses this art (title_u.stm, sbuty_u.stm, subtitle font and loading scarecrow in default.xbe); the format above was read from the files here, not from its code.
- Verified 2026-10-23 (decoded with a script, read-only): all 15 .pig sheets in sbuty_u.stm are DXT5. Pieces used by the F1 menu, measured from their alpha: sbmenu03 rough rectangle frame (6,180,115x73), hook cursor pointing left (2,82,51x43), vine divider (2,145,140x30); sbmenu12 ornamental vine (73,1,183x37); button00 B (1,1,19x22), A (23,1,19x22), Y (45,2,19x20), X (67,2,19x21), Start (105,2,19x20). exword00 holds the prompt words (BACK, EXIT, DEFAULT, CONFIRM, USE, BUY, PLAY, SWITCH, DETAILS), sbmenu01 the pause menu words (ITEM, MAP, OPTIONS, ACTION, INDEX, EXIT), sbmenu02 the digits and COLLECTION, sbback00/01 the Spiral Hill painting. The .anb layouts were not needed.
