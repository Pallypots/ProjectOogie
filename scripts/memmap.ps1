# Project Oogie: summarise the game process's committed memory by kind and by
# allocation, to find what grows (VMMap-style, no extra tools).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\memmap.ps1 [-Top 12]
param([int]$Top = 12)
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class OogieMem {
    [StructLayout(LayoutKind.Sequential)]
    public struct MBI { public IntPtr Base, AllocBase; public uint AllocProtect; public uint pad0;
        public IntPtr Size; public uint State, Protect, Type, pad1; }
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint a, bool i, int pid);
    [DllImport("kernel32.dll")] public static extern IntPtr VirtualQueryEx(IntPtr h, IntPtr a, out MBI m, IntPtr l);
    public static List<long[]> Scan(int pid) {
        var r = new List<long[]>();
        IntPtr h = OpenProcess(0x0410, false, pid);
        long a = 0; MBI m;
        while (VirtualQueryEx(h, new IntPtr(a), out m, new IntPtr(Marshal.SizeOf(typeof(MBI)))) != IntPtr.Zero) {
            if (m.State == 0x1000) r.Add(new long[] { m.AllocBase.ToInt64(), m.Size.ToInt64(), m.Type, m.Protect });
            long next = m.Base.ToInt64() + m.Size.ToInt64();
            if (next <= a) break; a = next;
        }
        return r;
    }
}
"@
$p = Get-Process oogie_recomp | Select-Object -First 1
$regions = [OogieMem]::Scan($p.Id)
$byType = $regions | Group-Object { switch ($_[2]) { 0x20000 { "private" } 0x40000 { "mapped" } 0x1000000 { "image" } default { "other" } } } |
    ForEach-Object { [pscustomobject]@{ Kind = $_.Name; MB = [int](($_.Group | ForEach-Object { $_[1] } | Measure-Object -Sum).Sum / 1MB); Regions = $_.Count } }
$byType | Format-Table -AutoSize
$allocs = $regions | Where-Object { $_[2] -eq 0x20000 } | Group-Object { $_[0] } |
    ForEach-Object { [pscustomobject]@{ Base = ('0x{0:X}' -f [long]$_.Name); MB = [math]::Round((($_.Group | ForEach-Object { $_[1] } | Measure-Object -Sum).Sum / 1MB), 1) } } |
    Sort-Object MB -Descending
"private allocations: $($allocs.Count); size histogram (MB: count):"
$allocs | Group-Object { if ($_.MB -ge 64) { ">=64" } elseif ($_.MB -ge 4) { "4-64" } elseif ($_.MB -ge 1) { "1-4" } else { "<1" } } | ForEach-Object { "  $($_.Name): $($_.Count) = $([int](($_.Group | Measure-Object MB -Sum).Sum)) MB" }
$allocs | Select-Object -First $Top | Format-Table -AutoSize
