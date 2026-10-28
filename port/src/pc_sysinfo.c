/* Project Oogie: what the program runs on, at the top of logs\play.log.
 *
 * 0.2: reports from Reddit and GitHub arrive with a log and little else. The
 * first lines now say which build it is, the Windows version (or the Wine /
 * Proton version and the Linux kernel under it), the processor and how many
 * logical processors the program may use, and the memory. The graphics card
 * follows a little later ([D3D11] GPU: ...). */
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>
#include "pc_version.h"

typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
typedef const char *(CDECL *WineVersionFn)(void);
typedef void (CDECL *WineHostFn)(const char **sysname, const char **release);

void pc_sysinfo_log(void)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    OSVERSIONINFOW ov;
    MEMORYSTATUSEX ms;
    int r[4], i;
    char cpu[49];
    DWORD_PTR pm = 0, sm = 0;
    int allowed = 0;

    fprintf(stderr, "[OOGIE] Project Oogie %s (built %s %s)\n", OOGIE_VERSION, __DATE__, __TIME__);

    memset(&ov, 0, sizeof ov);
    ov.dwOSVersionInfoSize = sizeof ov;
    if (nt) {
        RtlGetVersionFn gv = (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion");
        WineVersionFn wv = (WineVersionFn)(void *)GetProcAddress(nt, "wine_get_version");
        WineHostFn wh = (WineHostFn)(void *)GetProcAddress(nt, "wine_get_host_version");
        if (gv) gv(&ov);
        if (wv) {
            const char *sys = "?", *rel = "?";
            if (wh) wh(&sys, &rel);
            fprintf(stderr, "[OOGIE] system: Wine %s on %s %s (Proton / Steam Deck / Linux), reports Windows %lu.%lu.%lu\n",
                    wv(), sys, rel, ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber);
        } else {
            fprintf(stderr, "[OOGIE] system: Windows %lu.%lu build %lu\n",
                    ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber);
        }
    }

    memset(cpu, 0, sizeof cpu);
    __cpuid(r, (int)0x80000000);
    if ((unsigned)r[0] >= 0x80000004u) {
        for (i = 0; i < 3; i++) {
            __cpuid(r, (int)(0x80000002u + (unsigned)i));
            memcpy(cpu + 16 * i, r, 16);
        }
    }
    for (i = 0; cpu[i] == ' '; i++)
        ;
    if (GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm))
        for (; pm; pm &= pm - 1) allowed++;
    fprintf(stderr, "[OOGIE] processor: %s, %lu logical processors (%d for this program)\n",
            cpu[i] ? cpu + i : "unknown", GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), allowed);

    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms))
        fprintf(stderr, "[OOGIE] memory: %.1f GB, %.1f GB free\n",
                (double)ms.ullTotalPhys / (1024.0 * 1024 * 1024), (double)ms.ullAvailPhys / (1024.0 * 1024 * 1024));
    fflush(stderr);
}
