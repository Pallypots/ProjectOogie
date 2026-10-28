/* Project Oogie: the HLSL compiler, loaded at run time.
 *
 * The back end and the settings menu compile their shaders while the game
 * runs (the game's vertex and combiner programs become HLSL). Linked as an
 * import, d3dcompiler_47.dll came from the system. Under Wine and Proton
 * (Steam Deck, SteamOS, Linux) that is Wine's own compiler, which rejected
 * the vertex program shaders ("Unknown modifier StructuredBuffer", 0.2 Wine
 * test), so those draws failed. Microsoft's compiler is redistributable with
 * an application (Windows SDK, Redist\D3D\x64), and the release ships it
 * beside the program; it is loaded from there by full path first, and from
 * the system when it is missing. */
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include "pc_d3dc.h"

static pD3DCompile s_compile;
static HRESULT (WINAPI *s_create_blob)(SIZE_T, ID3DBlob **);

static void load(void)
{
    static volatile LONG done;
    char path[MAX_PATH], *s;
    HMODULE m = NULL;
    if (InterlockedCompareExchange(&done, 1, 0)) return;
    if (GetModuleFileNameA(NULL, path, sizeof path) && (s = strrchr(path, '\\')) != NULL) {
        snprintf(s + 1, sizeof path - (size_t)(s + 1 - path), "d3dcompiler_47.dll");
        m = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (!m) {
        m = LoadLibraryA("d3dcompiler_47.dll");
        snprintf(path, sizeof path, "d3dcompiler_47.dll (system)");
    }
    if (m) {
        s_compile = (pD3DCompile)(void *)GetProcAddress(m, "D3DCompile");
        s_create_blob = (HRESULT (WINAPI *)(SIZE_T, ID3DBlob **))(void *)GetProcAddress(m, "D3DCreateBlob");
    }
    fprintf(stderr, "[D3D11] shader compiler: %s%s\n", m ? path : "none found", s_compile ? "" : " (D3DCompile missing)");
}

HRESULT WINAPI oogie_D3DCompile(LPCVOID src, SIZE_T len, LPCSTR name, const D3D_SHADER_MACRO *defs,
                                ID3DInclude *inc, LPCSTR entry, LPCSTR target, UINT f1, UINT f2,
                                ID3DBlob **code, ID3DBlob **err)
{
    load();
    if (!s_compile) { if (code) *code = NULL; if (err) *err = NULL; return E_NOTIMPL; }
    return s_compile(src, len, name, defs, inc, entry, target, f1, f2, code, err);
}

HRESULT WINAPI oogie_D3DCreateBlob(SIZE_T size, ID3DBlob **blob)
{
    load();
    if (!s_create_blob) { if (blob) *blob = NULL; return E_NOTIMPL; }
    return s_create_blob(size, blob);
}
