cmake_minimum_required(VERSION 3.20)

# ============================================================================
# YOUR_GAME_NAME - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(oogie_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# Oogie: link XInput 1.4 (ships with Windows 8/10/11) instead of 1.3
# (DirectX 2010 redistributable only). Overrides the upstream target's
# link list from here so the xboxrecomp checkout stays unmodified.
# MSVC: the Windows SDK's xinput.lib already targets XINPUT1_4.dll, so
# only MinGW (whose libxinput.a is 1.3) needs the override.
if(MINGW AND TARGET xbox_input)
    set_property(TARGET xbox_input PROPERTY LINK_LIBRARIES platform xinput1_4)
    set_property(TARGET xbox_input PROPERTY INTERFACE_LINK_LIBRARIES platform xinput1_4)
endif()
# Oogie: build-time patches to the runtime (see port/patches/README.md).
# The xboxrecomp checkout stays unmodified. For each listed source file of a
# toolkit target, CMake copies it into the build tree, applies the exact text
# replacements port/patches/<name>.old -> <name>.new, and compiles the copy in
# place of the original. A replacement that does not match exactly once stops
# configuration, so a patch can never silently stop applying.
function(oogie_patch_source target srcdir file)
    set(_src "${srcdir}/${file}")
    file(READ "${_src}" _text)
    string(REPLACE "\r\n" "\n" _text "${_text}")
    foreach(_name IN LISTS ARGN)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/patches/${_name}.old" _old)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/patches/${_name}.new" _new)
        string(REPLACE "\r\n" "\n" _old "${_old}")
        string(REPLACE "\r\n" "\n" _new "${_new}")
        string(FIND "${_text}" "${_old}" _first)
        string(FIND "${_text}" "${_old}" _last REVERSE)
        if(_first EQUAL -1 OR NOT _first EQUAL _last)
            message(FATAL_ERROR "Oogie patch '${_name}' does not match exactly once in ${_src}")
        endif()
        string(REPLACE "${_old}" "${_new}" _text "${_text}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${CMAKE_CURRENT_SOURCE_DIR}/patches/${_name}.old"
                     "${CMAKE_CURRENT_SOURCE_DIR}/patches/${_name}.new")
    endforeach()
    set(_out "${CMAKE_BINARY_DIR}/oogie_patched/${file}")
    file(WRITE "${_out}.tmp" "${_text}")
    configure_file("${_out}.tmp" "${_out}" COPYONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}")
    get_target_property(_srcs ${target} SOURCES)
    list(FIND _srcs "${file}" _idx)
    if(_idx EQUAL -1)
        message(FATAL_ERROR "Oogie patch: ${file} is not a source of ${target}")
    endif()
    list(REMOVE_AT _srcs ${_idx})
    list(APPEND _srcs "${_out}")
    set_property(TARGET ${target} PROPERTY SOURCES ${_srcs})
    target_include_directories(${target} PRIVATE "${srcdir}")
    message(STATUS "Oogie: patched ${file} (${ARGN})")
endfunction()

# KNOWN_ISSUES #23: 8 guest thread stacks are too few for Chapter 1.
oogie_patch_source(xbox_kernel "${XBOXRECOMP_DIR}/src/kernel" xbox_memory_layout.c
                   thread_stacks
                   quiet_gpu_trace       # OOGIE_QUIET_GPU=1: no per-kick-off log lines
                   pb_segment_wrap      # #46: follow the ring's JUMP on a wrap
                   contig_quarantine_a contig_quarantine_b contig_quarantine_c
                   contig_quarantine_d contig_quarantine_e   # #56: freed contiguous pages wait 3 s before reuse
                   pb_ring_reset pb_ring_reset_label        # #66: a ring reset (GET+PUT) is not a wrap
                   pb_history_a pb_history_b)               # #66: last ring steps, printed when the semaphore goes back
# KNOWN_ISSUES #24: CLEAR_SURFACE ignored its per-channel bits; alpha-only
# clears blanked the level.
oogie_patch_source(xbox_kernel "${XBOXRECOMP_DIR}/src/kernel" nv2a_pb_exec.c
                   clear_channels_32 clear_channels_16
                   vertex_current_value
                   swizzled_surfaces_a swizzled_surfaces_b swizzled_surfaces_c
                   quiet_draw_progress
                   parallel_vsh         # vertex programs of big batches on helper threads
                   cache_verbose_getenv # a getenv that ran on every draw
                   backend_extra_stages # back end can read texture stages 1-3 and the combiners
                   backend_vertex_extra_a backend_vertex_extra_b backend_vertex_extra_c
                                        # specular, fog, texcoords 1-3 per vertex (#36)
                   backend_gpu_vsh      # vertex-program batches to the back end (#37)
                   diag_skip_batches    # inert unless OOGIE_DIAG_SKIP_* is set
                   diag_trace_d0        # only prints under RECOMP_FRAME_TRACE
                   pb_method_ring       # #51 diagnostic: last methods, OOGIE_FRAME_LOG
                   sem_drop_test        # #54 test switch: OOGIE_TEST_DROP_SEM=N
                   clear_rect           # #57: hand the clear rectangle to the back end
                   palette_decode       # palettised textures decode with their CLUT
                   sem_offset_decl sem_offset   # #66: a release goes to the word SET_SEMAPHORE_OFFSET names
                   flip_done)           # #51/#57: the buffer each frame finished in, for the back end
# The two-second kernel call summary, off under OOGIE_QUIET_GPU (frame hitches).
oogie_patch_source(xbox_kernel "${XBOXRECOMP_DIR}/src/kernel" kernel_bridge.c
                   kernel_summary_quiet kernel_read_quiet   # no [READ] line per disc read unless OOGIE_READ_LOG=1
                   ke_wait_d3d_gpu      # #58: D3D's GPU-far-behind wait spun forever (figure viewer hang)
                   guest_cpu_held       # lets port code release the guest CPU lock while it waits
                   usb_dpc_lock_a usb_dpc_lock_b
                   guest_cpu_fair
                   thread_suspended_b thread_suspended_a thread_suspended_c   # #61: CreateSuspended honoured   # #43: a USB DPC waits for a thread at DISPATCH_LEVEL
                   wait_log_bounded wait_multi_log_bounded
                   guest_priority_table guest_cpu_priority guest_cpu_safepoint)   # dev cycle 4: the guest CPU goes to the highest-priority waiter   # 2026-10-23: a wrapped log counter wrote a 10 GB play.log
# Mods: replacement disc files from mods\<name>\files (dev cycle 4).
oogie_patch_source(xbox_kernel "${XBOXRECOMP_DIR}/src/kernel" kernel_path.c
                   mods_folder_a mods_folder_b)
# vblank_frame_lock / vblank_timer_1ms (port/patches): tried for #51, not used:
# they did not help, and the intro film then took 4 minutes (mt2-mt4).
# KNOWN_ISSUES #43: the emulated USB controller's stale ED write-back.
oogie_patch_source(xbox_usb "${XBOXRECOMP_DIR}/src/usb" ohci.c
                   ohci_ed_head_a ohci_ed_head_b
                   pad_rumble
                   ohci_td_validate ohci_head_writeback_check ohci_head_repair
                   usb_frame_read usb_frame_after_a usb_frame_after_b)  # #43: no freed TDs, no garbage heads, repair a -1 head
# Controllers: port/src/pc_input.c replaces xbox_InputGetState (PS4/PS5 pads
# through SDL2.dll, rebinding from settings.ini); the runtime's is kept as
# xbox_InputGetState_xinput for OOGIE_INPUT_OLD=1.
oogie_patch_source(xbox_input "${XBOXRECOMP_DIR}/src/input" xinput_device.c
                   pad_wrapper)
# KNOWN_ISSUES #48: music routing (mixbin levels under RECOMP_APU_BINS=1).
oogie_patch_source(xbox_apu "${XBOXRECOMP_DIR}/src/apu" apu_dsp.c
                   apu_bin_trace
                   apu_mixdown_stereo    # centre/effect sends to both sides
                   apu_doorbell_latch)   # #43: never follow a changed scratch table
oogie_patch_source(xbox_apu "${XBOXRECOMP_DIR}/src/apu" apu_vp.c
                   apu_route_trace
                   apu_fecv_trace_c apu_fecv_trace_a apu_fecv_trace_b)
# Master volume and mute from the settings menu (pc_menu.c).
oogie_patch_source(xbox_apu "${XBOXRECOMP_DIR}/src/apu" apu_xaudio2.c
                   audio_volume)
# The program and constants for the GPU vertex-program path (#37).
oogie_patch_source(xbox_kernel "${XBOXRECOMP_DIR}/src/kernel" nv2a_vsh_interp.c
                   vsh_access)
# KNOWN_ISSUES #55: name the holder of a guest lock waited on for 100 ms.
oogie_patch_source(xbox_kernel "${XBOXRECOMP_DIR}/src/kernel" kernel_rtl.c
                   cs_wait_report)
# port/src/oogie_gpu_vsh.h, included by the backend_gpu_vsh patch.
target_include_directories(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# KNOWN_ISSUES #28: the segment walk followed JUMP/CALL targets past guest RAM.
oogie_patch_source(xbox_kernel "${XBOXRECOMP_DIR}/src/kernel" nv2a_pb_scan.c
                   pb_segment_bounds_a pb_segment_bounds_b
                   pb_segment_ack       # acknowledge kickoffs during long segments
                   pb_segment_carry     # #46: methods split by PUT, ring wrap target
                   pb_note_off          # method inventory off under OOGIE_QUIET_GPU
                   pb_where_decl pb_where   # #66: where each method sits (for the release report)
                   pb_call_log              # #66: report CALLs into the ring
                   pb_carry_log             # #66: report parameters carried across segments
                   pb_ring_target_a pb_ring_target_b pb_reread)   # #66: jumps into the ring elsewhere than its start are bad

if(MINGW)
    set(OOGIE_XINPUT xinput1_4)
else()
    set(OOGIE_XINPUT xinput)
endif()
# Static libgcc/winpthread so the exe needs no MinGW DLLs beside it.
if(MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc")
endif()

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/d3d11_backend.c     # Direct3D 11 render back end (OOGIE_D3D11=1)
    src/pc_settings.c       # settings.ini
    src/pc_input.c          # controllers: XInput + SDL2 (PS4/PS5), rebinding, rumble
    src/pc_menu.c           # in-game settings menu (F1 / Back+Start)
    src/pc_stall.c          # [STALL] call stacks when no frame for 200 ms
    src/pc_log.c            # log lines written by their own thread (no disk waits)
    src/pc_shot.c           # F12 screenshots (PNG through WIC)
    src/pc_setup.c          # first run: copy the game from the player's disc image
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Every direct guest call reports to recomp_call_profile_hit (recomp_types.h):
# the guest CPU lock's safepoint (recomp_manual.c, dev cycle 4).
target_compile_definitions(${PROJECT_NAME} PRIVATE RECOMP_CALL_PROFILE=1)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
    ${XBOXRECOMP_DIR}/src/kernel      # nv2a_backend.h for the D3D11 back end
    ${XBOXRECOMP_DIR}/src/input       # xinput_xbox.h, keyboard.h for pc_input.c
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    d3dcompiler
    ${OOGIE_XINPUT}
    winmm
    windowscodecs   # PNG screenshots (pc_shot.c)
    ole32
    bcrypt          # SHA-1 of default.xbe in first-run setup (pc_setup.c)
    comdlg32        # the "choose your disc image" dialog
    synchronization # WaitOnAddress: the task runner's poll sleeps (recomp_manual.c sub_001AA550)
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        # Oogie: always write a PDB so the crash handler (main.c, dbghelp)
        # can name the faulting function. /DEBUG alone gives every function's
        # name (public symbols); /OPT keeps the Release image the same size.
        /DEBUG /OPT:REF /OPT:ICF
    )
    # Line numbers for the runtime kernel (GPU executor, kernel bridge), where
    # host-side crashes have happened (KNOWN_ISSUES #28). Not for the 1.4 M
    # generated lines, which would slow the build a lot.
    target_compile_options(xbox_kernel PRIVATE /Zi)
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )

# SDL2.dll (third_party/SDL2, scripts/get_sdl2.ps1) goes beside the exe; pc_input.c
# loads it at run time, so a build without it still runs (Xbox pads only).
set(OOGIE_SDL2_DLL "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/SDL2/lib/x64/SDL2.dll")
if(EXISTS "${OOGIE_SDL2_DLL}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OOGIE_SDL2_DLL}" "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
endif()
