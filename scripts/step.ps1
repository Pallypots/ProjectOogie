# Project Oogie: one manual step in a running test game: send presses
# ("waitN" pauses) to the live pad, wait -Settle ms, save logs\live\<Name>.png.
param([string]$Press = "", [string]$Name = "step", [int]$Settle = 600)
$Root = Split-Path -Parent $PSScriptRoot
$pad = Join-Path $Root "logs\pad_live.txt"
foreach ($p in $Press.Split(',')) {
    if (-not $p) { continue }
    if ($p -match '^wait(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]) }
    else { for ($k = 0; $k -lt 20; $k++) { try { Add-Content -Path $pad -Value $p.Trim() -ErrorAction Stop; break } catch { Start-Sleep -Milliseconds 50 } }; if ($p -match ':(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1] + 150) } }
}
Start-Sleep -Milliseconds $Settle
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "window_shot.ps1") -Name $Name | Out-Null
Join-Path $Root "logs\live\$Name.png"
