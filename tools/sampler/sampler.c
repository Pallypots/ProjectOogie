/*
 * Project Oogie: tiny sampling profiler for a running oogie_recomp.exe.
 *
 *   sampler <pid> <tid> [samples=2000] [interval_ms=5] [-callers]
 *
 * Suspends the thread, reads its instruction pointer, resumes it, and after
 * all samples prints the functions it was in most often (names from the PDB
 * next to the exe, via dbghelp). Reads only; changes nothing in the target
 * beyond the brief suspensions.
 */
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char name[128]; unsigned n; } Hit;

static int cmp(const void *a, const void *b)
{
    return (int)((const Hit *)b)->n - (int)((const Hit *)a)->n;
}

int main(int argc, char **argv)
{
    DWORD pid, tid;
    int samples = 2000, interval = 5, i, nh = 0, unknown = 0, with_callers = 0;
    HANDLE proc, th;
    Hit *hits;
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;

    if (argc < 3) { fprintf(stderr, "usage: sampler pid tid [samples] [interval_ms]\n"); return 1; }
    pid = (DWORD)atoi(argv[1]); tid = (DWORD)atoi(argv[2]);
    if (argc > 3) samples = atoi(argv[3]);
    if (argc > 4) interval = atoi(argv[4]);
    with_callers = argc > 5 && !strcmp(argv[5], "-callers");
    proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!proc || !th) { fprintf(stderr, "open failed (%lu)\n", GetLastError()); return 1; }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!SymInitialize(proc, NULL, TRUE)) { fprintf(stderr, "SymInitialize failed\n"); return 1; }
    hits = (Hit *)calloc(4096, sizeof *hits);
    for (i = 0; i < samples; i++) {
        CONTEXT c;
        DWORD64 disp = 0;
        const char *name = "?";
        int k;
        char chain[128];
        DWORD64 pcs[3] = { 0, 0, 0 };
        int depth = 0, d;
        STACKFRAME64 sf;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_FULL;
        if (SuspendThread(th) == (DWORD)-1) break;
        GetThreadContext(th, &c);
        /* Walk two callers up (with -callers), while the thread is stopped. */
        memset(&sf, 0, sizeof sf);
        sf.AddrPC.Offset = c.Rip; sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrStack.Offset = c.Rsp; sf.AddrStack.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = c.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
        while (depth < (with_callers ? 3 : 1)
               && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &c, NULL,
                              SymFunctionTableAccess64, SymGetModuleBase64, NULL))
            pcs[depth++] = sf.AddrPC.Offset;
        ResumeThread(th);
        if (!depth) pcs[depth++] = c.Rip;
        chain[0] = 0;
        for (d = 0; d < depth; d++) {
            const char *nm = "?";
            memset(buf, 0, sizeof buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            if (SymFromAddr(proc, pcs[d], &disp, sym)) nm = sym->Name; else if (!d) unknown++;
            if (d) strncat(chain, " <- ", sizeof chain - strlen(chain) - 1);
            {
                IMAGEHLP_MODULE64 mi;
                memset(&mi, 0, sizeof mi);
                mi.SizeOfStruct = sizeof mi;
                if (SymGetModuleInfo64(proc, pcs[d], &mi)) {
                    strncat(chain, mi.ModuleName, sizeof chain - strlen(chain) - 1);
                    if (d && !_stricmp(mi.ModuleName, "oogie_recomp")) {
                        char rva[24];   /* exact caller, for dumpbin */
                        snprintf(rva, sizeof rva, "+%llX", (unsigned long long)(pcs[d] - mi.BaseOfImage));
                        strncat(chain, rva, sizeof chain - strlen(chain) - 1);
                    }
                    strncat(chain, "!", sizeof chain - strlen(chain) - 1);
                }
            }
            strncat(chain, nm, sizeof chain - strlen(chain) - 1);
        }
        name = chain;
        for (k = 0; k < nh && strcmp(hits[k].name, name); k++)
            ;
        if (k == nh && nh < 4096) { strncpy(hits[nh].name, name, 127); nh++; }
        if (k < 4096) hits[k].n++;
        Sleep(interval);
    }
    qsort(hits, nh, sizeof *hits, cmp);
    printf("thread %lu: %d samples, %d without symbol\n", tid, samples, unknown);
    for (i = 0; i < nh && i < 30; i++)
        printf("%6.1f%%  %s\n", 100.0 * hits[i].n / samples, hits[i].name);
    return 0;
}
