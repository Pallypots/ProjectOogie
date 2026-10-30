# Project Oogie: GitHub #2 check. Start a chapter, move and attack for a few
# minutes, and report deflect-switch markers, unresolved jumps and crashes.
param([string]$Name = "df1", [int]$Chapter = 3, [string]$Level = "0100", [int]$Minutes = 6)
$Root = Split-Path -Parent $PSScriptRoot
$lp = Join-Path $PSScriptRoot "live_press.ps1"
function Press([string]$p) { powershell -ExecutionPolicy Bypass -File $lp -Press $p -Wait 0 | Out-Null }
$log = Join-Path $Root "logs\$Name.err.log"
$env:OOGIE_START_CHAPTER = "$Chapter"
& (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name $Name -Cap 60 -Keep -TimeoutSec 5 -LevelFile ("stm_data\\" + $Level + "\.lz") -Marker "no_marker_xyz" | Out-Null
$env:OOGIE_START_CHAPTER = $null
$me = Get-Process oogie_recomp -ErrorAction SilentlyContinue | Sort-Object StartTime | Select-Object -Last 1
$seq = @("x:120", "x:120", "x:120", "lleft:400", "x:120", "x:120", "lright:400", "x:120", "a:120", "lup:600", "x:120", "x:120", "x:120", "ldown:400", "x:120", "lup:900")
$t0 = Get-Date; $k = 0
while (((Get-Date) - $t0).TotalMinutes -lt $Minutes) {
    if (-not $me -or $me.HasExited) { break }
    Press $seq[$k % $seq.Count]; $k++
    Start-Sleep -Milliseconds 250
}
$alive = $me -and -not $me.HasExited
if ($alive) { Stop-Process -Id $me.Id -Force }   # only the game this script started
"alive at end: $alive"
Select-String -Path $log -Pattern "\[SWITCH\]|ICALL Failed|\[CRASH\]|KeBugCheck" | ForEach-Object { $_.Line }
