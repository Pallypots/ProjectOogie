# Project Oogie: turn "host frames (exe offsets): +82FB +D6D92A ..." lines from
# a log ([PURECALL], [ICALL]) into function names, using the build's PDB.
# The offsets only match the exe they came from: use the pdb of that build.
#   py -3 scripts\symbolize.py LOGFILE [PDB]   (default bin\oogie_recomp.pdb)
import ctypes, ctypes.wintypes as wt, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
log = sys.argv[1]
pdb = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'bin', 'oogie_recomp.pdb')
exe = os.path.splitext(pdb)[0] + '.exe'

dbg = ctypes.WinDLL('dbghelp')
BASE = 0x10000000


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', ctypes.c_char * 512)]


dbg.SymInitialize.argtypes = [wt.HANDLE, ctypes.c_char_p, wt.BOOL]
dbg.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64,
                                wt.DWORD, ctypes.c_void_p, wt.DWORD]
dbg.SymLoadModuleEx.restype = ctypes.c_uint64
dbg.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFO)]
h = wt.HANDLE(0x1234)
dbg.SymSetOptions(0x2 | 0x10)                      # undecorated names, load lines off
if not dbg.SymInitialize(h, os.path.dirname(pdb).encode(), False):
    sys.exit('SymInitialize failed')
size = os.path.getsize(exe) if os.path.exists(exe) else 0x8000000
if not dbg.SymLoadModuleEx(h, None, exe.encode(), None, BASE, size, None, 0):
    sys.exit('could not load symbols for ' + exe)


def name(off):
    si = SYMBOL_INFO()
    si.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - 512 + 1
    si.MaxNameLen = 511
    disp = ctypes.c_uint64()
    if dbg.SymFromAddr(h, BASE + off, ctypes.byref(disp), ctypes.byref(si)):
        return '%s+0x%X' % (si.Name.decode(errors='replace'), disp.value)
    return '?'


for line in open(log, encoding='utf-8', errors='replace'):
    if 'host frames (exe offsets)' in line:
        offs = [int(x, 16) for x in re.findall(r'\+([0-9A-Fa-f]+)', line)]
        print(line.split(']')[0] + ']  ' + '  <-  '.join(name(o) for o in offs))
    elif 'guest stack' in line or 'is not code' in line or '[PURECALL] #' in line:
        print(line.rstrip())
