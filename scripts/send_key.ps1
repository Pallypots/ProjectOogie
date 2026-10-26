# Project Oogie: send a key press to the test game's window (WM_KEYDOWN and
# WM_KEYUP posted to it), for keyboard features the virtual pad cannot press
# (F12 screenshot, F1 menu, F11 fullscreen). Only the game whose PID is in
# logs\test_pids.txt, never the owner's.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\send_key.ps1 -Key F12
param([Parameter(Mandatory = $true)][string]$Key)
$Root = Split-Path -Parent $PSScriptRoot
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class OogieKeys {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
$vk = @{ F1 = 0x70; F11 = 0x7A; F12 = 0x7B; Escape = 0x1B; Enter = 0x0D; Q = 0x51; E = 0x45;
         Up = 0x26; Down = 0x28; Left = 0x25; Right = 0x27 }[$Key]
if (-not $vk) { "unknown key $Key"; exit 1 }
$ids = @(Get-Content (Join-Path $Root "logs\test_pids.txt") -ErrorAction SilentlyContinue | Where-Object { $_ -match '^\d+$' })
$p = $ids | ForEach-Object { Get-Process -Id ([int]$_) -ErrorAction SilentlyContinue } | Where-Object { $_.ProcessName -eq 'oogie_recomp' -and $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { "no test game window"; exit 1 }
[void][OogieKeys]::PostMessage($p.MainWindowHandle, 0x100, [IntPtr]$vk, [IntPtr]1)
Start-Sleep -Milliseconds 80
[void][OogieKeys]::PostMessage($p.MainWindowHandle, 0x101, [IntPtr]$vk, [IntPtr]([int64]0xC0000001))
"sent $Key to pid $($p.Id)"
