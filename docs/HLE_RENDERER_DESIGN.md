# High-level (HLE) Direct3D 8 renderer: stage 0 findings and design

Goal (owner, 2026-10-27): a renderer for weak PCs that works at the level of the game's
Direct3D 8 calls, as XboxXBERecomp does (docs/XBE_RECOMP_RESEARCH.md), instead of only
executing the NV2A command stream the recompiled D3D library writes.

## Stage 0 findings (Verified)
- **API surface**: 70 functions in the D3D section (0x1D1B80-0x1E4AE8) are called from outside
  it: 64 from the game's code, 10 from D3DX (scripts/d3d_api_inventory.py,
  docs/D3D8_API_INVENTORY.md).
- **Call rates** (OOGIE_D3D_CALLS=1, per presented frame; Chapter 3 / Casino Clash):

| Function | Calls per frame (ch3 / ch17) | What it is (from its code) |
|---|---|---|
| 0x1D4350 | 5,688 / 2,346 | Writes one raw NV2A method and value into the push buffer ([0x1E25F0], limit [0x1E25F4]); the XDK's "set render state simple" pattern. Called from 26 game functions |
| 0x1D7B90 | 3,488 / 1,166 | Push buffer: make room (internal) |
| 0x1D5AC0 | 3,061 / 845 | A draw: writes NV097 method 0x17FC (SET_BEGIN_END) and the vertices; called from 12 game functions |
| 0x1DDEE0 | 3,061 / 845 | Lazy state flush before every draw (internal, from 0x1D5AC0) |
| 0x1D26C0 | 1,740 / 1,538 | Takes a reference on a resource (refcount +0x80000): likely SetTexture / SetStreamSource |

- **What this means**: the game drives a large share of its render state as raw NV2A methods
  through 0x1D4350, not through high-level D3D calls. A pure API-level renderer would still have
  to understand those methods. So for this game the useful design is a **hybrid**.
- On a quiet system, two low-power cores already give about 49 fps in Casino Clash on Low
  (TEST_RESULTS kk runs); the two D3D busy-waits found there are fixed.

## Design: hybrid renderer
Keep the NV2A method executor for state (it already understands every method the game
writes, raw or through D3D). Take over the **draws** at the D3D level:

1. **Draw interception**: hook the draw entry points (0x1D5AC0 and the other draw variants,
   to be identified: indexed, user-pointer, inline). Instead of letting D3D write thousands of
   inline vertex words into the push buffer for the executor to read back one by one
   (fetch_attr, imm_vertex_method), issue the D3D11 draw directly from the game's vertex and
   index memory, with the state the executor has tracked up to that point.
2. **Ordering**: the hooked draw must see exactly the state the GPU would. Either flush the
   executor up to the current push buffer position before the draw (simple, keeps ordering),
   or record draws into the same ordered stream (a marker method). Start with the flush.
3. **Vertex data on the GPU**: upload the game's vertex buffers once and keep them on the
   GPU, keyed by address and a change hash (as textures are), so a draw costs a buffer bind,
   not a copy.
4. **Then state at the API level**, only where profiling shows the executor is still the cost:
   SetTexture, SetVertexShader, SetStreamSource.
5. **Switch**: OOGIE_HLE_DRAW=1 at first (opt-in), the old path stays the default until the
   whole game passes the chapter sweep and soaks with it on.

## Stages and checks
| Stage | Work | Done when |
|---|---|---|
| 0 | Inventory and call rates | Done (this file) |
| 1 (in progress) | Name the 70 entry points: draw variants, SetTexture, SetStreamSource, SetVertexShader, SetIndices, Present, Clear, Swap; record each with the NV2A methods it writes | Every draw entry point known |
| 2 | Draw interception for the commonest draw (0x1D5AC0), with an executor flush before it; pixel comparison against the current renderer (OOGIE_POST_SPLIT-style A/B) | Chapter 3 and Casino Clash identical, faster on two cores |
| 3 | The other draw variants; vertex buffers kept on the GPU | Whole-game sweep identical |
| 4 | API-level state where it still pays | Measured win on two cores |
| 5 | Default on | Sweep, soaks, owner play test, a real weak-PC report |

Risks: identifying the draw variants exactly (arguments, vertex formats, primitive types);
keeping draw/state ordering identical; the game also writes raw methods, so some draws may
bypass D3D entirely (the executor keeps handling those).

## Stage 1 result so far (2026-10-27)
The commonest draw (0x1D5AC0, about 3,000 per frame in Chapter 3) copies every vertex from the game's vertex streams into the push buffer as INLINE_ARRAY data, which the executor then reads back word by word. Stage 2 target: hook 0x1D5AC0, flush the executor to the current push-buffer position, and draw straight from the stream memory (the stream table at [dev+0xBD4] gives dwords and stride per stream; the vertex format comes from the executor's tracked SET_VERTEX_DATA_ARRAY_FORMAT state). Full table: D3D8_API_INVENTORY.md.

## Stage 2 investigation: stopped before coding (2026-10-27)
Two findings from the executor (port/build/oogie_patched/nv2a_pb_exec.c), both **Verified** by reading it:
1. **Inline draws already become array draws.** INLINE_ARRAY words are buffered (s_gpu.inline_buf)
   and, at BEGIN_END(0), draw_inline_array derives the packed layout from the attribute formats and
   runs the same draw_primitive path as DRAW_ARRAYS. The inline path's only extra cost is the two
   copies of the vertex words (the game thread's rep movsd in 0x1D5AC0, the executor's buffer append),
   which do not show in any profile.
2. **Pointing the GPU at the game's vertex memory is unsafe for this draw.** 0x1D5AC0 copies the
   vertices into the push buffer because the caller may overwrite its vertex memory as soon as the
   call returns (dynamic, user-pointer data). The executor runs behind the game thread; reading the
   source later instead of the copy would draw whatever the game wrote there since (the next
   object's vertices). A safe version must snapshot the data at call time, which is what the
   inline copy already does. Synchronising the executor at every draw instead (about 3,000 per
   frame) would cost far more than the copy.

Conclusion: stage 2 as planned would add a real risk of corrupted geometry for no measurable gain,
so it was not built. The executor's own decoding of the inline words is already the cheap array
path. If weak-PC reports on 0.2 show the GPU thread is the limit there, the next candidates are
inside the executor (per-vertex attribute fetch, fetch_attr) rather than a D3D-level draw hook.