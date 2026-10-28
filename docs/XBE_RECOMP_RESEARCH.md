# "Xbox XBE Recomp": research for 0.2 (2026-10-27)

A Reddit commenter (MDM-808) said ports made with "Xbox XBE Recomp" run better on weak PCs
(Celeron N3060, i3 3220) than XboxRecomp ones; another (blondejefazo) suggested it plus a
Direct3D 12 back end. Findings from web research (sources linked; not yet checked by
building anything):

## What it is
- **TGP482/XboxXBERecomp**, https://github.com/TGP482/XboxXBERecomp: "a tool for statically
  recompiling original Xbox games into native executables", born from Splinter Cell: Double
  Agent. **MIT licence** (https://github.com/TGP482/XboxXBERecomp/blob/main/LICENSE).
  About 129 commits, no tagged release. A separate codebase from sp00nznet/xboxrecomp.
- Ports with it: Splinter Cell: Double Agent, BLACK (https://github.com/TGP482/BLACK-XBERecomp),
  007: From Russia With Love, Darkwatch (https://allrecomps.com/consoles/xbox/).
- The commenter's other examples are not XBE Recomp ports: NoRain211's doa3-re / doaxvb-re are
  GPL-3.0 (own lifter plus a hand-written runtime), and parkerallan's doa3-recomp is listed as an
  XboxRecomp port.

## How it differs from our toolchain
| | XboxRecomp (ours) | XboxXBERecomp |
|---|---|---|
| Code | x86 lifted to C, registers as globals | x86 lifted to C++ per function, built with Clang; register handling not documented |
| Graphics | the game's own D3D8 library runs (recompiled) and writes NV2A push buffers; we execute those and translate to Direct3D 11 | **HLE**: Direct3D 8 reimplemented at the API level (library functions found by signatures), a native Direct3D 12 renderer on its own thread, vertex programs and combiners to HLSL |
| Sound | the game's DSOUND/XACT run against an MCPX APU model, out through XAudio2 | **HLE** DirectSound mixed in software through XAudio2 |
| Kernel | HLE | HLE (kernel and XDK libraries at the API level) |

Probable reason for the speed gap (inference): we run the console's driver code plus a
hardware model under it; HLE skips both layers. BLACK-XBERecomp's minimum: a 2-core CPU with
SSE4.2, a D3D12 GPU (GT 630, HD 7750, Intel HD 510), 4 GB RAM. No published benchmarks found.

## What this means for Project Oogie
1. **D3D8 at the API level for the hot paths** (draws, render/texture state, Present) via hooks in
   recomp_manual.c instead of push-buffer building and parsing. Biggest win, biggest job: it means
   reimplementing the parts of D3D8 the game uses. Fact-backed (both faster tools do this).
2. **DirectSound / XACT HLE** instead of the APU model. Fact-backed. Would also remove the
   register-trap and voice-stop classes of bugs (#55, #64, #72, #78).
3. **Rendering on its own thread** so game code never waits on translation. We partly have this
   (the GPU executor thread); measure first.
4. **Clang with high optimisation**, locals instead of globals for registers. Inference.
5. **Measure before rewriting**: scripts/weak_pc_test.ps1 (two low-power cores, Low preset).

Licence: XboxXBERecomp is MIT, so ideas and code are compatible with our MIT code (with
attribution). The project rule "ideas only" was set for the GPL Oogie port; reusing MIT code
here is the owner's call. NoRain211's DOA ports are GPL-3: ideas only.

## Measured on our port (2026-10-27)
Two low-power cores, Casino Clash, Low preset: about 49 fps mean on a quiet system. The busiest thread on two cores was the game's own D3D KickOff spin, not GPU translation; both D3D spin-waits (vblank acknowledge, KickOff flush) are now answered at once. An HLE renderer stays an option, to be decided on reports from real weak PCs.
