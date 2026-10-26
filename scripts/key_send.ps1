# Project Oogie: send real keyboard events to the game window (tests the
# keyboard-as-pad path, which live_press.ps1's virtual pad bypasses).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\key_send.ps1 -Keys "C:2500,D:1500"
#   each entry <key>[:<hold ms>] (default 150); keys are letters, digits or
#   ENTER BACK UP DOWN LEFT RIGHT F1 F11 ESC TAB SPACE DEL PGUP PGDN. The game window is brought to the front.
param([Parameter(Mandatory = $true)][string]$Keys, [int]$GapMs = 300)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class OogieKeys {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    public static void Key(byte vk, bool down) {
        keybd_event(vk, (byte)MapVirtualKey(vk, 0), down ? 0u : 2u, UIntPtr.Zero);
    }
}
"@
$p = Get-Process oogie_recomp -ErrorAction Stop | Select-Object -First 1
[void][OogieKeys]::SetForegroundWindow($p.MainWindowHandle)
Start-Sleep -Milliseconds 300
$named = @{ ENTER = 0x0D; BACK = 0x08; UP = 0x26; DOWN = 0x28; LEFT = 0x25; RIGHT = 0x27; F1 = 0x70; F11 = 0x7A; ESC = 0x1B; TAB = 0x09; SPACE = 0x20; DEL = 0x2E; PGUP = 0x21; PGDN = 0x22 }
foreach ($e in $Keys.Split(',')) {
    $parts = $e.Trim().Split(':')
    $k = $parts[0].ToUpper()
    $hold = if ($parts.Count -gt 1) { [int]$parts[1] } else { 150 }
    $vk = if ($named.ContainsKey($k)) { $named[$k] } else { [int][char]$k }
    [OogieKeys]::Key([byte]$vk, $true)
    Start-Sleep -Milliseconds $hold
    [OogieKeys]::Key([byte]$vk, $false)
    Start-Sleep -Milliseconds $GapMs
}
