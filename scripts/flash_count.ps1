# Project Oogie: capture N consecutive presented frames of the running game
# and count "flash" frames (mean brightness far from both neighbours), e.g.
# the fogged-out frames of KNOWN_ISSUES #46.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\flash_count.ps1 -Tag t [-Count 60]
param([Parameter(Mandatory = $true)][string]$Tag, [int]$Count = 60)
$Root = Split-Path -Parent $PSScriptRoot
$lines = powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "live_flips.ps1") -Count $Count -Tag $Tag -TimeoutSec 60
$m = @()
foreach ($l in $lines) { if ($l -match 'mean=([0-9.]+)') { $m += [double]$Matches[1] } }
$flash = @()
for ($i = 1; $i -lt $m.Count - 1; $i++) {
    if ([math]::Abs($m[$i] - $m[$i - 1]) -gt 12 -and [math]::Abs($m[$i] - $m[$i + 1]) -gt 12 -and [math]::Abs($m[$i - 1] - $m[$i + 1]) -lt 6) {
        $flash += "$i($($m[$i]) vs $($m[$i-1]))"
    }
}
"$Tag : $($m.Count) frames, $($flash.Count) flash frames: $($flash -join ' ')"
