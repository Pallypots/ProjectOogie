# Project Oogie: capture the game window as shown on screen (after the
# back end's widescreen and scaling), to logs\live\<Name>.png.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\window_shot.ps1 -Name ws1
param([Parameter(Mandatory = $true)][string]$Name)
$Root = Split-Path -Parent $PSScriptRoot
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class OogieWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
$p = Get-Process oogie_recomp -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p -or $p.MainWindowHandle -eq 0) { "no game window"; exit 1 }
[OogieWin]::SetProcessDPIAware() | Out-Null
$h = $p.MainWindowHandle
[OogieWin]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300
$r = New-Object OogieWin+RECT; [OogieWin]::GetClientRect($h, [ref]$r) | Out-Null
$pt = New-Object OogieWin+POINT; [OogieWin]::ClientToScreen($h, [ref]$pt) | Out-Null
$w = $r.R - $r.L; $hh = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
$out = Join-Path $Root "logs\live\$Name.png"
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
"$out ${w}x${hh}"
