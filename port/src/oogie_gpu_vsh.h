/*
 * Project Oogie: vertex programs on the GPU (KNOWN_ISSUES #37, #26).
 *
 * The runtime's executor runs every NV2A vertex program in a CPU interpreter
 * (nv2a_vsh_interp.c) and hands the back end screen-space triangles. In a
 * level that interpreter is the largest cost of the GPU thread, and the game
 * steps its logic once per frame, so a slow frame is slow motion. With the
 * build-time patches vsh_access and backend_gpu_vsh the executor can instead
 * hand a whole vertex-program batch to the D3D11 back end: the raw vertex
 * inputs, the program and its constants. The back end translates the program
 * to an HLSL vertex shader (same rules as the interpreter) and draws.
 *
 * Shared by port/patches/backend_gpu_vsh.new (compiled into xbox_kernel) and
 * port/src/d3d11_backend.c.
 */
#ifndef OOGIE_GPU_VSH_H
#define OOGIE_GPU_VSH_H

#include <stdint.h>
#include "nv2a_backend.h"

typedef struct {
    const uint32_t (*program)[4];   /* from the start slot; ninsn, the last FINAL */
    uint32_t ninsn;
    const float (*constants)[4];    /* c[0..191] */
    /* Inputs the program reads, nverts x ninputs float4, in attribute order;
     * bit a of input_mask: attribute a is one of them. */
    const float *inputs;
    uint32_t nverts, ninputs, input_mask;
    const uint32_t *indices;        /* triangle list into the batch's vertices */
    uint32_t nindices;
    float aa_sx, aa_sy;             /* surface scale (anti-aliasing) */
    uint32_t fog_enable, fog_mode;
    float fog_param[2];
    float tscale[4][2];             /* per stage: coordinate -> 0..1 */
} OogieVshBatch;

/* The back end's entry point: returns 1 if it drew the batch, 0 to let the
 * executor run it on the CPU instead. tex0: stage 0's texture or NULL. */
typedef int (*OogieVshDrawFn)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                              const Nv2aTexture *tex0, const OogieVshBatch *b);

/* Defined by the backend_gpu_vsh patch; NULL turns the GPU path off. */
void oogie_gpu_vsh_register(OogieVshDrawFn fn);

#endif
