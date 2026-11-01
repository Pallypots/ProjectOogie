# Project Oogie: read the running game's Xbox memory from outside (test tool).
# Finds where the guest's 64 MB sit in the host process (the XBE header at
# Xbox address 0x10000) and saves snapshots, so values such as health and
# money can be found by comparing snapshots taken at different moments.
#   py -3 scripts\memscan.py snap NAME [START END]   -> build_out\mem\NAME.bin
#   py -3 scripts\memscan.py read ADDR [COUNT]       dwords at an Xbox address
#   py -3 scripts\memscan.py watch ADDR [SECS]       print a dword once a second
#   py -3 scripts\memscan.py write ADDR VALUE        (tests only)
import ctypes, ctypes.wintypes as wt, os, struct, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
K = ctypes.WinDLL('kernel32', use_last_error=True)

class MBI(ctypes.Structure):
    _fields_ = [('Base', ctypes.c_void_p), ('AllocBase', ctypes.c_void_p), ('AllocProtect', wt.DWORD),
                ('pad0', wt.DWORD), ('Size', ctypes.c_size_t), ('State', wt.DWORD), ('Protect', wt.DWORD),
                ('Type', wt.DWORD), ('pad1', wt.DWORD)]

K.OpenProcess.restype = wt.HANDLE
K.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
K.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
K.WriteProcessMemory.argtypes = K.ReadProcessMemory.argtypes


def game_pid():
    out = subprocess.run(['tasklist', '/fi', 'imagename eq oogie_recomp.exe', '/fo', 'csv', '/nh'],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split('","')
        if len(parts) > 1:
            return int(parts[1])
    sys.exit('the game is not running')


class Guest:
    def __init__(self):
        self.h = K.OpenProcess(0x0010 | 0x0020 | 0x0008 | 0x0400, False, game_pid())
        self.base = self._find_base()

    def raw(self, addr, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not K.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
            return None
        return buf.raw[:got.value]

    def _find_base(self):
        a, m = 0, MBI()
        while K.VirtualQueryEx(self.h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m)):
            b, s = m.Base or 0, m.Size
            # the header is 0x10000 into the guest mapping, which can start a region
            for h in (b, b + 0x10000):
                if m.State == 0x1000 and h + 0x108 <= b + s and self.raw(h, 4) == b'XBEH':
                    if struct.unpack('<I', self.raw(h + 0x104, 4))[0] == 0x10000:
                        return h - 0x10000
            if b + s <= a:
                break
            a = b + s
        sys.exit('guest memory not found')

    def read(self, va, n):
        return self.raw(self.base + va, n)

    def write(self, va, data):
        got = ctypes.c_size_t()
        return K.WriteProcessMemory(self.h, ctypes.c_void_p(self.base + va), data, len(data), ctypes.byref(got))


def main():
    g = Guest()
    cmd = sys.argv[1]
    if cmd == 'snap':
        start = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x3B2000
        end = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x4000000
        out = bytearray()
        for va in range(start, end, 0x10000):   # unreadable pieces stay zero
            out += g.read(va, 0x10000) or bytes(0x10000)
        d = os.path.join(ROOT, 'build_out', 'mem')
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, sys.argv[2] + '.bin'), 'wb') as f:
            f.write(struct.pack('<II', start, end) + out)
        print('snap', sys.argv[2], hex(start), hex(end))
    elif cmd == 'read':
        va, n = int(sys.argv[2], 0), int(sys.argv[3], 0) if len(sys.argv) > 3 else 8
        data = g.read(va, n * 4)
        for i in range(n):
            v = struct.unpack_from('<I', data, i * 4)[0]
            f = struct.unpack_from('<f', data, i * 4)[0]
            print('%08X: %08X %11d %g' % (va + i * 4, v, v if v < 0x80000000 else v - 0x100000000, f))
    elif cmd == 'watch':
        va, secs = int(sys.argv[2], 0), int(sys.argv[3]) if len(sys.argv) > 3 else 30
        for _ in range(secs):
            print(time.strftime('%H:%M:%S'), struct.unpack('<I', g.read(va, 4))[0], flush=True)
            time.sleep(1)
    elif cmd == 'write':
        va, v = int(sys.argv[2], 0), int(sys.argv[3], 0)
        print('ok' if g.write(va, struct.pack('<I', v & 0xFFFFFFFF)) else 'failed')
    else:
        print('host base of guest memory: 0x%X' % g.base)


if __name__ == '__main__':
    main()
