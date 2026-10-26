# Project Oogie: stop only the game processes the test scripts started
# (their IDs are in logs\test_pids.txt, written by live_start.ps1 and
# boot_like_play.ps1), never a game the owner started with play.bat.
# 2026-10-22: a test's "Stop-Process -Name oogie_recomp" closed the owner's
# game. Every script calls this instead.
$Root = Split-Path -Parent $PSScriptRoot
$list = Join-Path $Root "logs\test_pids.txt"
if (-not (Test-Path $list)) { return }
$keep = @()
foreach ($line in Get-Content $list) {
    $id = 0
    if (-not [int]::TryParse($line.Trim(), [ref]$id)) { continue }
    $p = Get-Process -Id $id -ErrorAction SilentlyContinue
    if ($p -and ($p.ProcessName -eq 'oogie_recomp' -or $p.ProcessName -eq 'Project Oogie')) {
        Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
        # Wait until it is gone: a game still closing when the list is
        # cleared looks like the owner's to the next test's guard.
        Wait-Process -Id $id -Timeout 15 -ErrorAction SilentlyContinue
    }
}
Set-Content -Path $list -Value ""
