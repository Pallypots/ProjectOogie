/* Project Oogie: the HLSL compiler loaded at run time (pc_d3dc.c). Include
 * after <d3dcompiler.h>: D3DCompile and D3DCreateBlob then go through the
 * copy shipped beside the program (Proton / Steam Deck) or the system's. */
#ifndef PC_D3DC_H
#define PC_D3DC_H
#include <d3dcompiler.h>
#ifdef __cplusplus
extern "C" {
#endif
HRESULT WINAPI oogie_D3DCompile(LPCVOID src, SIZE_T len, LPCSTR name, const D3D_SHADER_MACRO *defs,
                                ID3DInclude *inc, LPCSTR entry, LPCSTR target, UINT f1, UINT f2,
                                ID3DBlob **code, ID3DBlob **err);
HRESULT WINAPI oogie_D3DCreateBlob(SIZE_T size, ID3DBlob **blob);
#ifdef __cplusplus
}
#endif
#define D3DCompile oogie_D3DCompile
#define D3DCreateBlob oogie_D3DCreateBlob
#endif
