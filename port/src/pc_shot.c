/* Project Oogie: screenshots (F12). The present step hands over a copy of the
 * shown picture (BGRA rows); this file writes it as a PNG with the Windows
 * Imaging Component on a thread of its own, so the game does not wait for the
 * encoder. Files go to screenshots\Oogie_<date>_<time>.png next to the game. */
#define COBJMACROS
#include <windows.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_shot.h"

typedef struct {
    unsigned char *px;      /* 24-bit BGR rows, tightly packed */
    unsigned w, h;
    char path[MAX_PATH];
} ShotJob;

static int write_png(const ShotJob *j)
{
    IWICImagingFactory *f = NULL;
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    WCHAR wpath[MAX_PATH];
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    HRESULT hr;
    int ok = 0;

    MultiByteToWideChar(CP_ACP, 0, j->path, -1, wpath, MAX_PATH);
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&f);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(f, &st);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(st, wpath, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatPng, NULL, &enc);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(enc, &fr, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(fr, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(fr, j->w, j->h);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(fr, &fmt);
    if (SUCCEEDED(hr) && !IsEqualGUID(&fmt, &GUID_WICPixelFormat24bppBGR)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels(fr, j->h, j->w * 3, j->w * 3 * j->h, j->px);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(fr);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
    ok = SUCCEEDED(hr);
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IWICStream_Release(st);
    if (f) IWICImagingFactory_Release(f);
    if (!ok) DeleteFileA(j->path);
    return ok;
}

static DWORD WINAPI shot_thread(LPVOID p)
{
    ShotJob *j = (ShotJob *)p;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int ok = write_png(j);
    fprintf(stderr, ok ? "[SHOT] saved %s (%ux%u)\n" : "[SHOT] could not save %s (%ux%u)\n", j->path, j->w, j->h);
    fflush(stderr);
    if (SUCCEEDED(co)) CoUninitialize();
    free(j->px);
    free(j);
    return 0;
}

void pc_shot_save_bgra(const void *bgra, unsigned w, unsigned h, unsigned pitch)
{
    ShotJob *j;
    SYSTEMTIME t;
    unsigned x, y;
    HANDLE th;
    if (!bgra || !w || !h) return;
    j = (ShotJob *)calloc(1, sizeof *j);
    if (!j) return;
    j->px = (unsigned char *)malloc((size_t)w * h * 3);
    if (!j->px) { free(j); return; }
    j->w = w; j->h = h;
    for (y = 0; y < h; y++) {
        const unsigned char *s = (const unsigned char *)bgra + (size_t)y * pitch;
        unsigned char *d = j->px + (size_t)y * w * 3;
        for (x = 0; x < w; x++, s += 4, d += 3) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
    }
    CreateDirectoryA("screenshots", NULL);
    GetLocalTime(&t);
    snprintf(j->path, sizeof j->path, "screenshots\\Oogie_%04u%02u%02u_%02u%02u%02u_%03u.png",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    th = CreateThread(NULL, 0, shot_thread, j, 0, NULL);
    if (th) CloseHandle(th);
    else { free(j->px); free(j); }
}

/* ---- texture packs (dev cycle 4): PNG with alpha, written by one worker ----
 * Texture dumps can be hundreds of files at a level load: one thread works
 * through a queue, so the game never waits for the encoder. */
typedef struct PngJob {
    struct PngJob *next;
    unsigned char *px;      /* BGRA rows, tightly packed */
    unsigned w, h;
    char path[MAX_PATH];
} PngJob;

static CRITICAL_SECTION s_q_cs;
static HANDLE s_q_ev;
static PngJob *s_q_head, *s_q_tail;
static volatile LONG s_q_state, s_q_len;

static int write_png32(const PngJob *j)
{
    IWICImagingFactory *f = NULL;
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    WCHAR wpath[MAX_PATH];
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    HRESULT hr;
    int ok;
    MultiByteToWideChar(CP_ACP, 0, j->path, -1, wpath, MAX_PATH);
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(f, &st);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(st, wpath, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatPng, NULL, &enc);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(enc, &fr, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(fr, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(fr, j->w, j->h);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(fr, &fmt);
    if (SUCCEEDED(hr) && !IsEqualGUID(&fmt, &GUID_WICPixelFormat32bppBGRA)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels(fr, j->h, j->w * 4, j->w * 4 * j->h, j->px);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(fr);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
    ok = SUCCEEDED(hr);
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IWICStream_Release(st);
    if (f) IWICImagingFactory_Release(f);
    if (!ok) DeleteFileA(j->path);
    return ok;
}

static DWORD WINAPI png_worker(LPVOID p)
{
    (void)p;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (;;) {
        PngJob *j;
        WaitForSingleObject(s_q_ev, INFINITE);
        for (;;) {
            EnterCriticalSection(&s_q_cs);
            j = s_q_head;
            if (j) { s_q_head = j->next; if (!s_q_head) s_q_tail = NULL; }
            LeaveCriticalSection(&s_q_cs);
            if (!j) break;
            write_png32(j);
            InterlockedDecrement(&s_q_len);
            free(j->px);
            free(j);
        }
    }
}

int pc_png_queue_bgra(const char *path, const void *bgra, unsigned w, unsigned h, unsigned pitch)
{
    PngJob *j;
    unsigned y;
    if (!w || !h) return 0;
    if (s_q_state != 2) {
        if (InterlockedCompareExchange(&s_q_state, 1, 0) == 0) {
            HANDLE t;
            InitializeCriticalSection(&s_q_cs);
            s_q_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
            t = CreateThread(NULL, 0, png_worker, NULL, 0, NULL);
            if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
            InterlockedExchange(&s_q_state, 2);
        }
        while (s_q_state != 2) Sleep(0);
    }
    if (s_q_len > 512) return 0;                /* a backlog: skip, it is dumped next time */
    j = (PngJob *)calloc(1, sizeof *j);
    if (!j) return 0;
    j->px = (unsigned char *)malloc((size_t)w * h * 4);
    if (!j->px) { free(j); return 0; }
    for (y = 0; y < h; y++) memcpy(j->px + (size_t)y * w * 4, (const unsigned char *)bgra + (size_t)y * pitch, (size_t)w * 4);
    j->w = w; j->h = h;
    snprintf(j->path, sizeof j->path, "%s", path);
    InterlockedIncrement(&s_q_len);
    EnterCriticalSection(&s_q_cs);
    if (s_q_tail) s_q_tail->next = j; else s_q_head = j;
    s_q_tail = j;
    LeaveCriticalSection(&s_q_cs);
    SetEvent(s_q_ev);
    return 1;
}

/* A PNG (or any image WIC reads) as BGRA rows; free() the result. */
unsigned char *pc_image_load_bgra(const char *path, unsigned *w, unsigned *h)
{
    IWICImagingFactory *f = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cv = NULL;
    WCHAR wpath[MAX_PATH];
    unsigned char *px = NULL;
    UINT iw = 0, ih = 0;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED), hr;
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateDecoderFromFilename(f, wpath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(dec, 0, &fr);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(f, &cv);
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppBGRA,
                                                           WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = IWICBitmapSource_GetSize((IWICBitmapSource *)cv, &iw, &ih);
    if (SUCCEEDED(hr) && iw && ih && iw <= 16384 && ih <= 16384) {
        px = (unsigned char *)malloc((size_t)iw * ih * 4);
        if (px && FAILED(IWICBitmapSource_CopyPixels((IWICBitmapSource *)cv, NULL, iw * 4, iw * 4 * ih, px))) {
            free(px); px = NULL;
        }
    }
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (f) IWICImagingFactory_Release(f);
    if (co == S_OK || co == S_FALSE) CoUninitialize();
    if (px) { *w = iw; *h = ih; }
    return px;
}
