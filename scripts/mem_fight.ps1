# Project Oogie: Chapter 1 fight (scripts\fight_to_hyde.ps1) with a memory
# snapshot every -Every seconds (scripts\memscan.py, build_out\mem\<Name>_NNN.bin),
# to find values such as health and money by comparing snapshots with the
# window shots. Stops only the game this test started, after -MaxMinutes at most.
param([string]$Name = "mf1", [int]$Minutes = 3, [int]$Every = 10, [int]$MaxMinutes = 8)
$Root = Split-Path -Parent $PSScriptRoot
if (Get-Process oogie_recomp -ErrorAction SilentlyContinue) { "a game is already running; not starting"; exit 1 }
Remove-Item (Join-Path $Root "build_out\mem\$Name`_*.bin") -ErrorAction SilentlyContinue
$job = Start-Job -ArgumentList $Root, $Name, $Minutes {
    param($Root, $Name, $Minutes)
    Set-Location $Root
    powershell -ExecutionPolicy Bypass -File scripts\fight_to_hyde.ps1 -Name $Name -Minutes $Minutes -Xact 0 -NoStop
}
$t0 = Get-Date; $i = 0; $gamePid = $null
while (((Get-Date) - $t0).TotalMinutes -lt $MaxMinutes -and $job.State -eq 'Running') {
    $p = Get-Process oogie_recomp -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p) {
        $gamePid = $p.Id
        $sec = [int]((Get-Date) - $t0).TotalSeconds
        $r = py -3 (Join-Path $Root "scripts\memscan.py") snap ("{0}_{1:D3}" -f $Name, $i) 0x3B2000 0x4000000 2>&1
        "{0,4}s snap {1}: {2}" -f $sec, $i, $r
        $i++
    }
    Start-Sleep -Seconds $Every
}
Stop-Job $job -ErrorAction SilentlyContinue; Receive-Job $job -ErrorAction SilentlyContinue
if ($gamePid) { Stop-Process -Id $gamePid -Force -ErrorAction SilentlyContinue }
"done: $i snaps, game $gamePid stopped"
