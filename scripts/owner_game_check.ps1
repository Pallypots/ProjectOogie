# Project Oogie: is a game running that the test scripts did not start (the
# owner's)? Prints its ID and exits 1 if so, 0 otherwise. live_start.ps1
# refuses to start a test while one is running.
$Root = Split-Path -Parent $PSScriptRoot
$list = Join-Path $Root "logs\test_pids.txt"
$ours = @()
if (Test-Path $list) { $ours = @(Get-Content $list | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
$other = @(Get-Process oogie_recomp -ErrorAction SilentlyContinue | Where-Object { $ours -notcontains "$($_.Id)" })
if ($other.Count) { "owner game running: pid $($other.Id -join ', ')"; exit 1 }
exit 0
