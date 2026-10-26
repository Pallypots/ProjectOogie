"""Build tools/binkdiff: run the Bink plane decoder (sub_00232C40) two ways on
the same captured guest state and compare.

  1. The original Xbox machine code, executed natively in a 32-bit process
     (Xbox code is 32-bit x86, so the host CPU is the reference).
  2. The recompiled C for the same functions, copied verbatim from
     port/src/recomp/gen/ (what the game actually runs).

Input: logs/bink_in.bin written by the game with OOGIE_BINK_CAPTURE=N
(recomp_manual.c, sub_00232C40_enter): 8 registers then 64 MB of guest RAM.

    python tools/binkdiff/gen.py
    cd logs && ../tools/binkdiff/build/binkdiff.exe bink_in.bin [bink_out.bin]

Guest addresses must be host addresses for the native run. binkdiff.exe is a
loader based at 0x10000 whose uninitialised ".zpad" section spans to 64 MB, so
Windows cannot place heaps or stacks there; the real work is in
binkdiff_core.dll, based high. The contiguous mirror 0x80000000.. (Bink's frame
buffers live there) is a second copy of the same RAM.

Needs the 32-bit MSVC toolchain (vcvars32.bat). Read-only with respect to the
game files.
"""
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(ROOT, "port", "src", "recomp", "gen")
FUNCS = os.path.join(ROOT, "build_out", "disasm", "functions.json")
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "build")
ENTRY = 0x00232C40
# sub_00232C40 calls one of these through [ebp-0x94] (0x232C8C / 0x232C98).
EXTRA_ROOTS = (0x00231C60, 0x00231F30)


def closure(entry, funcs):
    seen, todo = set(), [entry]
    while todo:
        va = todo.pop()
        if va in seen:
            continue
        seen.add(va)
        todo.extend(int(c, 16) for c in funcs[va]["calls_to"])
    return seen


def extract(names):
    """Function bodies from the generated files, keyed by name."""
    want = set(names)
    bodies = {}
    for path in sorted(glob.glob(os.path.join(GEN, "*.c"))):
        text = open(path, encoding="utf-8", errors="replace").read()
        for m in re.finditer(r"^void (sub_[0-9A-F]{8})\(void\)\s*\{", text, re.M):
            name = m.group(1)
            if name not in want or name in bodies:
                continue
            end = text.index("\n}\n", m.start())
            bodies[name] = text[m.start():end + 3]
    return bodies


PRELUDE = r'''
#define RECOMP_GENERATED_CODE 1
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <windows.h>
#include "recomp_types.h"

RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
RECOMP_TLS uint32_t g_seh_ebp, g_ebp, g_fs_base;
RECOMP_TLS int g_df;
RECOMP_TLS double g_fp_stack[8]; RECOMP_TLS int g_fp_top;
RECOMP_TLS uint16_t g_fp_control_word = 0x027F; RECOMP_TLS uint32_t g_mxcsr = 0x1F80;
RECOMP_TLS int g_fp_cmp; RECOMP_TLS uint16_t g_fp_cc = 0x4000;
RECOMP_TLS RecompXmm g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7;
RECOMP_TLS RecompMmx g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE]; volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count, g_icall_guard_hits, g_icall_guard_misses;
ptrdiff_t g_xbox_mem_offset;
int g_force_return;
uint32_t g_xbox_code_lo = 0x00010000u, g_xbox_code_hi = 0x00460000u;
void recomp_icall_fail_log(uint32_t va) { printf("icall fail %08X\n", va); }
void recomp_unimpl(const char *t, uint32_t va) { printf("UNIMPL %s at %08X\n", t, va); }
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va);
void recomp_icall_not_code_log(uint32_t va) { printf("icall not code %08X\n", va); }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return 0; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; return 0; }
void sub_00232C40_enter(void) {}
'''

CORE = r'''
#undef eax
#undef ecx
#undef edx
#undef esp
#undef ebx
#undef esi
#undef edi
#undef ebp

#define RAM 0x04000000u                 /* 64 MB of guest RAM */
#define MIRROR 0x80000000u              /* contiguous mirror of the same RAM */
#define NGRAN (RAM >> 16)

static uint32_t R[8];                   /* eax ecx edx ebx esp ebp esi edi */
static uint32_t s_host_sp, s_nat_eax;
static unsigned short s_cw = 0x027F;
static unsigned char s_lo_ok[NGRAN], s_hi_ok[NGRAN];

static __declspec(naked) void run_native(void)
{
    __asm {
        pushad
        mov  s_host_sp, esp
        finit
        fldcw word ptr s_cw
        cld
        mov  eax, dword ptr [R+16]
        lea  ecx, landed
        mov  dword ptr [eax], ecx
        mov  dword ptr [eax-4], 00232C40h
        mov  ecx, dword ptr [R+4]
        mov  edx, dword ptr [R+8]
        mov  ebx, dword ptr [R+12]
        mov  ebp, dword ptr [R+20]
        mov  esi, dword ptr [R+24]
        mov  edi, dword ptr [R+28]
        lea  esp, [eax-4]
        mov  eax, dword ptr [R+0]
        ret
    landed:
        mov  s_nat_eax, eax
        mov  esp, s_host_sp
        popad
        ret
    }
}

/* Run one original function from inside the recompiled code: registers come
   from and go back to the g_ register file, esp is the guest stack. The return
   address the recompiled caller pushed is swapped for a landing pad. */
static uint32_t T[8], T_va, s_host_sp2;
static __declspec(naked) void native_call_asm(void)
{
    __asm {
        pushad
        mov  s_host_sp2, esp
        mov  eax, dword ptr [T+16]
        lea  ecx, landed2
        mov  dword ptr [eax], ecx
        mov  ecx, dword ptr [T+4]
        mov  edx, dword ptr [T+8]
        mov  ebx, dword ptr [T+12]
        mov  ebp, dword ptr [T+20]
        mov  esi, dword ptr [T+24]
        mov  edi, dword ptr [T+28]
        mov  esp, dword ptr [T+16]
        mov  eax, dword ptr [T+0]
        jmp  dword ptr [T_va]
    landed2:
        mov  dword ptr [T+0], eax
        mov  dword ptr [T+4], ecx
        mov  dword ptr [T+8], edx
        mov  dword ptr [T+12], ebx
        mov  dword ptr [T+16], esp
        mov  dword ptr [T+20], ebp
        mov  dword ptr [T+24], esi
        mov  dword ptr [T+28], edi
        mov  esp, s_host_sp2
        popad
        ret
    }
}

static void native_call(uint32_t va)
{
    uint32_t slot = g_esp, saved = *(uint32_t *)(uintptr_t)g_esp;
    T[0] = g_eax; T[1] = g_ecx; T[2] = g_edx; T[3] = g_ebx;
    T[4] = g_esp; T[5] = g_ebp; T[6] = g_esi; T[7] = g_edi;
    T_va = va;
    native_call_asm();
    *(uint32_t *)(uintptr_t)slot = saved;
    g_eax = T[0]; g_ecx = T[1]; g_edx = T[2]; g_ebx = T[3];
    g_esp = T[4]; g_ebp = T[5]; g_esi = T[6]; g_edi = T[7];
}

static unsigned char *big(size_t n)
{
    return (unsigned char *)VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN,
                                         PAGE_READWRITE);
}

/* A state is 2 x 64 MB: the low window then the mirror window. */
static void load_state(const unsigned char *s)
{
    uint32_t g;
    for (g = 0; g < NGRAN; g++) {
        if (s_lo_ok[g]) memcpy((void *)(uintptr_t)(g << 16), s + (g << 16), 0x10000);
        if (s_hi_ok[g]) memcpy((void *)(uintptr_t)(MIRROR + (g << 16)), s + RAM + (g << 16), 0x10000);
    }
}

static void save_state(unsigned char *s)
{
    uint32_t g;
    for (g = 0; g < NGRAN; g++) {
        if (s_lo_ok[g]) memcpy(s + (g << 16), (void *)(uintptr_t)(g << 16), 0x10000);
        if (s_hi_ok[g]) memcpy(s + RAM + (g << 16), (void *)(uintptr_t)(MIRROR + (g << 16)), 0x10000);
    }
}

static uint32_t va_of(uint32_t i) { return i < RAM ? i : MIRROR + (i - RAM); }

/* skip: guest stack range (scratch below esp, return slot) */
static int s_quiet;
static uint32_t diff(const char *what, const unsigned char *a, const unsigned char *b,
                     uint32_t skip_lo, uint32_t skip_hi, int show)
{
    if (s_quiet) show = 0;
    uint32_t i, n = 0, runs = 0, start = 0, in_run = 0;
    for (i = 0; i < 2 * RAM; i++) {
        uint32_t va = va_of(i);
        int ok = i < RAM ? s_lo_ok[i >> 16] : s_hi_ok[(i - RAM) >> 16];
        int d = ok && a[i] != b[i] && !(va >= skip_lo && va < skip_hi);
        if (d) { n++; if (!in_run) { in_run = 1; start = i; } }
        else if (in_run) {
            in_run = 0;
            if (runs++ < (uint32_t)show)
                printf("    %08X..%08X (%u bytes) first %02X vs %02X\n",
                       va_of(start), va_of(i), i - start, a[start], b[start]);
        }
    }
    printf("%s: %u bytes differ in %u runs\n", what, n, runs);
    return n;
}

/* Plane view: dst is arg 1, width arg 3, height arg 4, pitch arg 5. Print a
   map of the 8x8 blocks that differ, so the pattern can be compared with the
   speckles on screen. */
static void block_map(const unsigned char *a, const unsigned char *b, uint32_t dst,
                      uint32_t w, uint32_t h, uint32_t pitch)
{
    uint32_t bx, by, x, y, bad = 0;
    uint32_t base = dst >= MIRROR ? RAM + (dst - MIRROR) : dst;
    printf("block map of the plane at %08X (%ux%u, pitch %u), X = differs:\n", dst, w, h, pitch);
    for (by = 0; by < h / 8; by++) {
        char line[256]; uint32_t n = 0;
        for (bx = 0; bx < w / 8 && n < 200; bx++) {
            int d = 0;
            for (y = 0; y < 8 && !d; y++)
                for (x = 0; x < 8; x++) {
                    uint32_t o = base + (by * 8 + y) * pitch + bx * 8 + x;
                    if (a[o] != b[o]) { d = 1; break; }
                }
            line[n++] = d ? 'X' : '.';
            bad += d;
        }
        line[n] = 0;
        if (h / 8 <= 60 || by % 2 == 0) printf("  %s\n", line);
    }
    printf("blocks differing: %u of %u\n", bad, (w / 8) * (h / 8));
}

__declspec(dllexport) int binkdiff_run(int argc, char **argv, uintptr_t pad_lo, uintptr_t pad_hi)
{
    unsigned char *in, *nat, *lif, *out;
    FILE *f;
    uint32_t g, lo_missing = 0, hi_missing = 0, *args;
    DWORD old;
    if (argc < 2) { printf("usage: binkdiff bink_in.bin [bink_out.bin]\n"); return 2; }
    for (g = 0; g < NGRAN; g++) {
        uintptr_t lo = (uintptr_t)g << 16;
        void *p;
        s_lo_ok[g] = lo >= pad_lo && lo + 0x10000 <= pad_hi
                     && VirtualProtect((void *)lo, 0x10000, PAGE_EXECUTE_READWRITE, &old);
        p = VirtualAlloc((void *)(uintptr_t)(MIRROR + lo), 0x10000, MEM_RESERVE | MEM_COMMIT,
                         PAGE_EXECUTE_READWRITE);
        s_hi_ok[g] = p == (void *)(uintptr_t)(MIRROR + lo);
        lo_missing += !s_lo_ok[g];
        hi_missing += !s_hi_ok[g];
    }
    printf("unavailable granules: low %u (pad %08X..%08X), mirror %u\n",
           lo_missing, (unsigned)pad_lo, (unsigned)pad_hi, hi_missing);

    in = big(2 * RAM); nat = big(2 * RAM); lif = big(2 * RAM);
    if (!in || !nat || !lif) { printf("out of memory\n"); return 1; }
    f = fopen(argv[1], "rb");
    if (!f || fread(R, 4, 8, f) != 8 || fread(in, 1, RAM, f) != RAM) {
        printf("cannot read %s\n", argv[1]); return 1;
    }
    fclose(f);
    memcpy(in + RAM, in, RAM);
    printf("regs eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
           R[0], R[1], R[2], R[3], R[4], R[5], R[6], R[7]);
    args = (uint32_t *)(in + R[4]);
    printf("args:"); for (g = 1; g <= 11; g++) printf(" %08X", args[g]); printf("\n");
    if (!s_lo_ok[R[4] >> 16] || !s_lo_ok[0x00232C40u >> 16]) {
        printf("stack or code granule unavailable\n"); return 1;
    }

    /* the native side writes its landing address at [esp] and the entry
       below it; the recompiled side ignores both */
    *(uint32_t *)(in + R[4] - 4) = 0x00232C40u;

    load_state(in);
    run_native();
    save_state(nat);
    printf("native returned eax=%08X\n", s_nat_eax);

    load_state(in);
    g_xbox_mem_offset = 0;
    g_eax = R[0]; g_ecx = R[1]; g_edx = R[2]; g_ebx = R[3];
    g_esp = R[4]; g_ebp = R[5]; g_esi = R[6]; g_edi = R[7];
    g_fp_top = 0; g_fp_control_word = 0x027F;
    sub_00232C40();
    save_state(lif);
    printf("recompiled returned eax=%08X\n", g_eax);

    diff("native vs recompiled", nat, lif, R[4] - 0x10000, R[4] + 4, 40);
    {
        /* one function recompiled at a time, the rest original; the entry
           function is always recompiled (it is the one being called) */
        int i;
        printf("one function recompiled, the rest original:\n");
        s_quiet = 1;
        for (i = 0; i < NFUNCS; i++) {
            uint32_t n;
            int k;
            /* the entry must stay recompiled: original code calls its callees
               directly, never through the dispatchers */
            s_lifted_mask = 1u << i;
            for (k = 0; k < NFUNCS; k++)
                if (s_func_va[k] == 0x00232C40u) s_lifted_mask |= 1u << k;
            load_state(in);
            g_eax = R[0]; g_ecx = R[1]; g_edx = R[2]; g_ebx = R[3];
            g_esp = R[4]; g_ebp = R[5]; g_esi = R[6]; g_edi = R[7];
            g_fp_top = 0;
            sub_00232C40();
            save_state(lif);
            n = diff("", nat, lif, R[4] - 0x10000, R[4] + 4, 0);
            printf("  sub_%08X recompiled: %u bytes differ\n", s_func_va[i], n);
        }
        s_quiet = 0;
        s_lifted_mask = 0xFFFFFFFFu;
        load_state(in);
        g_eax = R[0]; g_ecx = R[1]; g_edx = R[2]; g_ebx = R[3];
        g_esp = R[4]; g_ebp = R[5]; g_esi = R[6]; g_edi = R[7];
        sub_00232C40();
        save_state(lif);
    }
    block_map(nat, lif, args[1], args[3], args[4], args[5]);
    if (argc > 2 && (out = big(2 * RAM)) != NULL && (f = fopen(argv[2], "rb")) != NULL) {
        if (fseek(f, 32, SEEK_SET) == 0 && fread(out, 1, RAM, f) == RAM) {
            memcpy(out + RAM, out, RAM);
            /* the game kept running between the two captures: only the
               bytes either run wrote are meaningful */
            diff("native vs input (what the original code wrote)", nat, in, R[4] - 0x10000, R[4] + 4, 5);
            diff("recompiled-in-harness vs game (sanity)", lif, out, R[4] - 0x10000, R[4] + 0x100, 5);
        }
        fclose(f);
    }
    f = fopen("binkdiff_result.bin", "wb");
    if (f) { fwrite(nat, 1, 2 * RAM, f); fwrite(lif, 1, 2 * RAM, f); fclose(f); }
    return 0;
}
'''

LOADER = r'''
#include <stdio.h>
#include <string.h>
#include <windows.h>
/* Uninitialised, so it costs nothing on disk; it holds the guest range. */
#pragma section(".zpad", read, write, execute)
__declspec(allocate(".zpad")) unsigned char g_pad[0x03FC0000];
typedef int (*run_fn)(int, char **, uintptr_t, uintptr_t);
int main(int argc, char **argv)
{
    char path[MAX_PATH], *slash;
    HMODULE m;
    run_fn run;
    GetModuleFileNameA(NULL, path, sizeof path);
    slash = strrchr(path, '\\');
    strcpy(slash ? slash + 1 : path, "binkdiff_core.dll");
    m = LoadLibraryA(path);
    if (!m) { printf("cannot load %s (%lu)\n", path, GetLastError()); return 1; }
    run = (run_fn)GetProcAddress(m, "binkdiff_run");
    return run(argc, argv, (uintptr_t)g_pad, (uintptr_t)g_pad + sizeof g_pad);
}
'''


def find_vcvars():
    for root in (os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                 os.environ.get("ProgramFiles", r"C:\Program Files")):
        for ed in ("BuildTools", "Community", "Professional", "Enterprise"):
            p = os.path.join(root, "Microsoft Visual Studio", "2022", ed,
                             "VC", "Auxiliary", "Build", "vcvars32.bat")
            if os.path.exists(p):
                return p
    sys.exit("no vcvars32.bat")


def main():
    funcs = {int(f["start"], 16): f for f in json.load(open(FUNCS))}
    vas = set(closure(ENTRY, funcs))
    for r in EXTRA_ROOTS:
        vas |= closure(r, funcs)
    names = [f"sub_{va:08X}" for va in sorted(vas)]
    bodies = extract(names)
    missing = [n for n in names if n not in bodies]
    if missing:
        sys.exit(f"not found in generated C: {missing}")
    os.makedirs(OUT, exist_ok=True)
    src = [PRELUDE]
    src += [f"void {n}(void);" for n in names]
    src.append("static void native_call(uint32_t va);")
    src.append("static uint32_t s_lifted_mask = 0xFFFFFFFFu;  /* bit i: names[i] runs recompiled */")
    src.append(f"#define NFUNCS {len(names)}")
    src.append("static const uint32_t s_func_va[NFUNCS] = {" +
               ", ".join(f"0x{int(n[4:], 16):08X}u" for n in names) + "};")
    for i, n in enumerate(names):
        src.append(bodies[n].replace(f"void {n}(void)", f"static void lifted_{n}(void)", 1))
        src.append(f"void {n}(void) {{ if (s_lifted_mask & (1u << {i})) lifted_{n}(); "
                   f"else native_call(0x{int(n[4:], 16):08X}u); }}")
    src.append("recomp_func_t recomp_lookup(uint32_t va) {\n    switch (va) {")
    src += [f"    case 0x{int(n[4:], 16):08X}u: return {n};" for n in names]
    src.append('    default: printf("lookup %08X\\n", va); return 0; }\n}')
    src.append(CORE)
    with open(os.path.join(OUT, "binkdiff_core.c"), "w") as f:
        f.write("\n".join(src))
    with open(os.path.join(OUT, "binkdiff.c"), "w") as f:
        f.write(LOADER)
    rt = os.path.join(ROOT, "third_party", "xboxrecomp", "templates", "runtime")
    vc = find_vcvars()
    cmds = [
        f'"{vc}" >nul 2>&1 && cl /nologo /O1 /W0 /LD /I"{rt}" binkdiff_core.c '
        f'/link /BASE:0x70000000 /FIXED /DYNAMICBASE:NO',
        f'"{vc}" >nul 2>&1 && cl /nologo /O1 /W0 binkdiff.c '
        f'/link /BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE',
    ]
    rc = 0
    for cmd in cmds:
        r = subprocess.run(cmd, cwd=OUT, shell=True, capture_output=True, text=True)
        if r.returncode:
            print(r.stdout[-3000:], r.stderr[-1500:])
        rc = rc or r.returncode
    print("functions:", " ".join(names))
    sys.exit(rc)


if __name__ == "__main__":
    main()
