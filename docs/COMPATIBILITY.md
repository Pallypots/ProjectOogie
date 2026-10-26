# Xbox Runtime Compatibility Status

Status words: **Working** (seen in a run log), **Reached** (game calls it, outcome not yet confirmed), **Untested**.

| Subsystem | Provided by | Status for Oogie | Evidence |
|---|---|---|---|
| XBE load, section map, 0x10000 base | xboxrecomp kernel | Working | run1: 31/31 sections loaded |
| Kernel thunks (125 imports) | xboxrecomp kernel_bridge | Working | run1: 125 bridged, 2 extra stubbed |
| Threads (PsCreateSystemThreadEx) | kernel | Working | run1: main thread created |
| Heap / virtual memory | kernel | Working | run1: NtAllocateVirtualMemory OK |
| Contiguous memory (GPU) | kernel | Working | run1: 4 allocs, 5.8 MB |
| Save-data paths (UDATA/TDATA) | kernel file layer | Reached | run1: TitleMeta/TitleImage/SaveImage opened |
| GPU interrupt (KeConnectInterrupt vec 3) | kernel | Reached | run1 |
| D3D8 (statically linked XDK code) | recompiled game code | Reached | device init runs, 640x480 surface |
| NV2A push buffer execution | xboxrecomp nv2a_pb_exec (RECOMP_PB_EXEC=1) | Reached | run3: 245 methods seen, 0 draws |
| D3D fence wait | Oogie entry hook on sub_001D78D0 | Working | run4: semaphore 0x80001000 registered, frames flip |
| Rendering (GPU) | Oogie Direct3D 11 back end `port/src/d3d11_backend.c` (`OOGIE_D3D11=1`, default in play.bat) | Working | d3d3: menus, movies (greyscale), Chapter 1 cutscenes and gameplay; 7-8 fps in a level |
| Rendering | PB executor CPU rasteriser | Working (slow) | run4: disc-error screen + Now Loading animation rendered correctly |
| Present / display window | xboxrecomp fb_present (`RECOMP_FB_WINDOW`) | Working on Windows | win_run5: 35 fps, 3,373 flips |
| MCPX APU (audio chip) | xboxrecomp apu (xemu), `RECOMP_AC97_READY=1` | Reached | run7c: title starts APU, DSP handshake, ISR 0x00208AD1 claims interrupts; no audible output yet |
| XACT engine | recompiled game code on APU | Reached | run7c: loads 9 stream banks + se banks + effect.bin |
| Controller (XPP / USB) | xboxrecomp OHCI + usb_gamepad (`RECOMP_USB=1`); host pads through `port/src/pc_input.c`: XInput (Xbox 360/One/Series) and SDL2 (PS4, PS5, Switch Pro, others); keyboard; rebinding in `settings.ini`; rumble forwarded | Working (scripted pad, real keyboard) | 2026-10-18 in1/in2: keyboard via settings.ini Verified; scripted pad boot to chapter Verified. Real pads not yet tried (none connected). See docs/PC_FEATURES.md |
| Bink video | recompiled Bink | Decodes, renders black | win_run12: frames advance; YUV planes drawn as LIN_L8 textures, colour pass missing (KNOWN_ISSUES #17) |
| Title screen and menus | recompiled game code | Working | win_run19 |
| Game data reads | kernel file layer, `game/` = D: | Working | XACT_data, data.inf, stm_data read OK (async + APC) |
| SwitchToThread | Oogie override sub_001BD332 | Working | run8 |
| Suspended thread creation | Oogie hook sub_000584E0 (runtime ignores CreateSuspended) | Working for the job system | win_run3 |
| Thread suspend/resume | runtime + Oogie hook sub_001BC71B (SAME_ACCESS) | Working | win_run5: 0 failures |
| Thread IDs (GetCurrentThreadId) | Oogie hooks sub_001BD49C / sub_001BD404 | Working | win_run5 || Saving (XcSHA/HMAC signing) | kernel crypto | Untested | |


## Keyboard layout (testing, 2026-10-17)

Owner request: arrow keys as the face buttons in the pad's diamond, until the options menu gets rebinding. Down = A, Right = B, Left = X, Up = Y; T/F/G/H = D-pad; Enter = Start, Backspace = Back, WASD = left stick, IJKL = right stick, 1/3 = triggers, Q/E = White/Black; Z X C V still work. Implemented in the D3D11 window (`key_target` in `port/src/d3d11_backend.c`); `OOGIE_ARROW_FACE=0` restores arrows = D-pad.

# Running on other PCs (cross-machine checklist)

Started 2026-10-19 (owner: "this PC port will run on a lot of different machines, Nvidia, AMD etc. ... make sure it just works"). Status words as above, plus **To do**.

## Requirements (what the code needs)

| Item | Need | Notes |
|---|---|---|
| OS | Windows 10 or 11, 64-bit | D3D 11.1 runtime for partial clears (ClearView); without it a clear covers the whole surface |
| Direct3D | feature level 10.0 or newer | Shaders are SM 4.0. The GPU vertex-program path uses a StructuredBuffer and needs 11.0; below it each such draw is declined and the executor runs the program on the CPU (slower) |
| No GPU / broken driver | WARP (Windows' software D3D11) | Used automatically when no hardware device can be made; slow but runs |
| Controllers | XInput built in; SDL2.dll next to the exe for PlayStation and other pads | Without SDL2.dll, Xbox pads and keyboard still work (To do: check) |
| Audio | XAudio2 2.9 (Windows 10+) | |
| Disk | write access to the game folder (saves\, logs\, settings.ini) and %LOCALAPPDATA%\ProjectOogie (shader cache) | OOGIE_SHADER_CACHE=0 turns the cache off |

## Device creation (port/src/d3d11_backend.c, init_device)

- Hardware with feature levels 11.0, 10.1, 10.0; then WARP. The log's `[D3D11] GPU:` line names the adapter, vendor/device IDs, video memory, driver version, feature level, WARP and debug-layer state: the first line to read in a report from another PC.
- Nothing works: a message box asks for a driver update.
- `OOGIE_D3D_WARP=1` forces WARP (stand-in for a GPU we have not tested; catches Intel-only assumptions).
- `OOGIE_D3D_DEBUG=1`: D3D debug layer (needs the Windows "Graphics Tools" optional feature); warnings and errors go to the log as `[D3D11 debug]` lines (at most 300). Not installed: the device is made without it.

## Shader cache

Compiled DXBC keyed by a hash of HLSL source, entry point and target. DXBC is vendor-neutral (each driver compiles it at load), so the cache is safe across GPUs and drivers; no per-driver key is needed. Delete %LOCALAPPDATA%\ProjectOogie\shaders to rebuild.

## Checklist

| Check | Status | Evidence |
|---|---|---|
| Hardware device, Intel Arc 140V (this laptop, driver 32.0.101.8724, feature level 11.0) | Working | every log since 2026-10-17; dbg1 `[D3D11] GPU:` line |
| Feature-level fallback 11.0 -> 10.1 -> 10.0 | To do (code in) | |
| WARP run: boot, title, intro film | Working | warp1 (2026-10-19): "Microsoft Basic Render Driver ... WARP (software)", intro film at 60 fps |
| Debug-layer run: no ERROR/CORRUPTION through boot, menus, Town Square | To do | dbg1: layer not installed here (0x887A002D, DXGI_ERROR_SDK_COMPONENT_MISSING); the fallback without it worked. Needs the Windows optional feature "Graphics Tools" (owner's decision to install) |
| NVIDIA GPU | To do | needs a tester |
| AMD GPU | To do | needs a tester |
| Laptop with two GPUs | To do | default adapter; Windows' per-app GPU setting decides |
| High-DPI (125-200 %) | Working | per-monitor DPI awareness (fl1, 2880x1800) |
| Second monitor | To do | |
| Refresh rates other than 60 Hz | Untested | frame cap paces 30/60 with a timer when vsync cannot |
| Non-English Windows, user names with spaces or non-ASCII | To do | %LOCALAPPDATA% read with the ANSI API |
| Antivirus scanning the growing log | Known cost | log writes can stall (KNOWN_ISSUES #55) |

## Known risks across machines

- Thread timing: the game's threads run truly in parallel on a PC, and core count and speed change the timing. Races found so far: #48, #50, #61 (open). The main risk on other machines.
- GPU speed: a slow GPU makes the game wait in Swap and starves other threads (#62); lower Render Resolution on weak GPUs.
