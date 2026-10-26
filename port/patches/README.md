# Build-time patches to the XboxRecomp runtime

`third_party/xboxrecomp` stays an unmodified v0.14.0 checkout. Where the runtime
itself is wrong for this game, `port/CMakeLists.txt` (`oogie_patch_source`)
copies the affected source file into the build tree, applies exact text
replacements from this folder, and compiles the copy instead of the original.

Each replacement is a pair of files, `<name>.old` and `<name>.new`. The `.old`
text must occur exactly once in the original file (line endings are
normalised), otherwise configuration stops with an error, so a patch can never
silently stop applying after an upgrade.

| Patch | File | Why | Evidence |
|---|---|---|---|
| `thread_stacks` | src/kernel/xbox_memory_layout.c | Raise the guest thread-stack pool from 8 to 32. Chapter 1 keeps more than 8 task threads alive; the 9th ran inline on the main thread and froze the game | logs/live1.err.log "out of thread stacks"; KNOWN_ISSUES #23 |
| `clear_channels_32`, `clear_channels_16` | src/kernel/nv2a_pb_exec.c | Honour the per-channel CLEAR_SURFACE bits (R 0x10, G 0x20, B 0x40, A 0x80). The game clears alpha only (0x80) after drawing the scene; the runtime cleared the whole pixel and the level showed black | logs/live6.err.log frame trace "clear 80 surf 00230000"; KNOWN_ISSUES #24 |

| `vertex_current_value` | src/kernel/nv2a_pb_exec.c | A vertex-program input not supplied by an array takes the current value set with SET_VERTEX_DATA* (as on the NV2A and in xemu), not 0,0,0,1. The level's fade quad got opaque black and covered every frame | logs/live14.err.log frame trace: last pass `d0 0 0 0 1`, full screen; KNOWN_ISSUES #25 |
| `swizzled_surfaces_a/_b/_c` | src/kernel/nv2a_pb_exec.c | Swizzled render targets (SET_SURFACE_FORMAT type 2): kept in row order while drawn, re-ordered to swizzled layout when drawing moves elsewhere or at a flip. The game's 128x128 glow targets were sampled scrambled: dark stripes over gameplay | logs/live22.err.log: format 0x07070228 for surfaces 0x03206000..0x03236000; KNOWN_ISSUES #30 |
| `pb_segment_bounds_a/_b` | src/kernel/nv2a_pb_scan.c | The segment walk stops at addresses past the 64 MB contiguous window instead of reading them (the DMA walk already refuses them) | logs/d3d2.err.log: crash in pb_walk_segment+0x5F reading 0x8F000000; KNOWN_ISSUES #28 |
| `quiet_gpu_trace` (xbox_memory_layout.c), `quiet_draw_progress` (nv2a_pb_exec.c) | | `OOGIE_QUIET_GPU=1` stops `RECOMP_PB_EXEC` from also logging every kick-off and every 200th draw; with the D3D11 back end the GPU thread spent ~45% of its time writing these lines | tools/sampler on d3d3 |
| `pb_segment_ack` | src/kernel/nv2a_pb_scan.c | The segment walk acknowledges D3D kickoffs every 256 words, as the DMA walk does, so the game thread is not blocked in its kickoff spin for the whole execution of a segment | tools/sampler: game thread ~50% in sub_001D76F0's spin on 0x100410 |
| `parallel_vsh` | src/kernel/nv2a_pb_exec.c | Vertex programs of batches with 512+ vertices run on a pool of helper threads (`OOGIE_XF_THREADS`, default cores-2, max 6; 0 = off) | tools/sampler: 58% of the GPU thread in nv2a_vsh_run in Town Square |
| `cache_verbose_getenv` | src/kernel/nv2a_pb_exec.c | `getenv("RECOMP_PB_EXEC_VERBOSE")` in draw_primitive ran on every draw; cached | tools/sampler with caller offsets: ~13% of the GPU thread |
| `backend_extra_stages` | src/kernel/nv2a_pb_exec.c | Lets the back end read texture stages 1-3, the combiner stage count and the whole register-combiner program plus fog colour (`oogie_backend_stage_texture`, `oogie_backend_combiner_stages`, `oogie_backend_combiner`) | Bink YUV movies (mov2); KNOWN_ISSUES #17, #36 |
| `backend_vertex_extra_a/_b/_c` | src/kernel/nv2a_pb_exec.c | Per back-end vertex, the data `Nv2aVertex` leaves out (specular, fog factor, stage 0 q, texture coordinates 1-3 normalised) in a parallel array (`oogie_backend_vertex_extra`), so the D3D11 back end can run the combiners | KNOWN_ISSUES #36; pause menu and HUD at full brightness (rc3) |
| `vsh_access` | src/kernel/nv2a_vsh_interp.c | Accessors for the loaded vertex program and the constant file (`oogie_vsh_program`, `oogie_vsh_constants`) | needed by `backend_gpu_vsh` |
| `backend_gpu_vsh` | src/kernel/nv2a_pb_exec.c | A back end that registers a draw function (`oogie_gpu_vsh_register`, `port/src/oogie_gpu_vsh.h`) receives each vertex-program batch whole (inputs the program reads, program, constants, triangle list) and runs the program on the GPU; programs that write c[] stay on the CPU | KNOWN_ISSUES #26, #37: Town Square 38 -> 60 fps (gv3) |
| `ohci_ed_head_a/_b` | src/usb/ohci.c | The emulated USB controller writes an ED head pointer back only if the driver has not changed it during the pass (compare-and-swap) | KNOWN_ISSUES #43 (hypothesis; did not stop every crash) |
| `diag_skip_batches`, `diag_trace_d0` | src/kernel/nv2a_pb_exec.c | Diagnostics only: `OOGIE_DIAG_SKIP_EQ` / `OOGIE_DIAG_SKIP_TEX` drop batches; the frame trace prints vertex 0's colours | inert unless enabled |
| `pb_segment_carry` | src/kernel/nv2a_pb_scan.c | The segment scan finishes a method whose parameters were cut by PUT in the next segment (it used to drop them and read them as headers), and a top-level walk that ends on a JUMP records its target (`nv2a_pb_scan_segment_wrap`). Includes the `pb_segment_bounds`/`pb_segment_ack` changes to the same function | KNOWN_ISSUES #46: white/dark flash frames (fx1-fx4, fv1-fv2: 0) |
| `pb_segment_wrap` | src/kernel/xbox_memory_layout.c | On a pushbuffer ring wrap, walk the old piece to D3D's JUMP and continue from its target, instead of stopping at the highest PUT seen so far | KNOWN_ISSUES #46 |
| `apu_mixdown_stereo` | src/apu/apu_dsp.c | The stub DSP's mixdown sends speaker mixbins to their side, centre/LFE to both at -3 dB and effect sends to both at -6 dB (was: even bins left, odd right) | KNOWN_ISSUES #48: 3-5 dB left lean (au17: 0.2 dB) |
| `apu_doorbell_latch` | src/apu/apu_dsp.c | The DSP doorbell ack keeps the first GP scratch page instead of following a changed SGE table (it cleared dwords in memory it no longer owned) | KNOWN_ISSUES #43 (defence) |
| `apu_bin_trace`, `apu_route_trace`, `apu_fecv_trace_a/_b/_c` | src/apu/apu_dsp.c, apu_vp.c | Diagnostics only (`RECOMP_APU_BINS=1`): mixbin levels, voice routing changes, cross-thread voice programming | inert unless enabled |
| `pad_wrapper` | src/input/xinput_device.c | Renames the Windows `xbox_InputGetState` to `xbox_InputGetState_xinput`; `port/src/pc_input.c` provides the real one (XInput + SDL2 pads, rebinding) and falls back to it under `OOGIE_INPUT_OLD=1` | docs/PC_FEATURES.md (port feature, not an upstream bug) |
| `pad_rumble` | src/usb/ohci.c | The game's rumble packets (OUT on endpoint 2) go to `oogie_pad_rumble` in pc_input.c instead of being dropped | docs/PC_FEATURES.md (missing upstream feature) |

All of these (except the diagnostics) are upstream bugs and should be reported to XboxRecomp.
