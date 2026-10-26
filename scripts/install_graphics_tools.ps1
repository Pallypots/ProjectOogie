# Project Oogie: install the Windows "Graphics Tools" optional feature (the D3D
# debug layer, for OOGIE_D3D_DEBUG=1 runs; docs/COMPATIBILITY.md). Needs an
# elevated PowerShell. Owner approved on 2026-10-19.
$log = Join-Path (Split-Path -Parent $PSScriptRoot) "logs\graphics_tools_install.log"
"start $(Get-Date)" | Set-Content $log
try {
    Add-WindowsCapability -Online -Name "Tools.Graphics.DirectX~~~~0.0.1.0" -ErrorAction Stop | Out-String | Add-Content $log
} catch {
    "FAILED: $($_.Exception.Message)" | Add-Content $log
}
Get-WindowsCapability -Online -Name "Tools.Graphics.DirectX*" | Out-String | Add-Content $log
"end $(Get-Date)" | Add-Content $log
