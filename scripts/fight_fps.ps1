# Project Oogie: frame rate during the fight soak, sampled from the window
# title every 2 s after the first 70 s (boot and the walk to the skeletons).
# Extra environment for the run: -Extra "RECOMP_GUEST_LOCK=1;OOGIE_X=1".
# Usage: powershell -ExecutionPolicy Bypass -File scripts\fight_fps.ps1 -Name ff1 [-Extra "..."] [-Samples 40]
param([Parameter(Mandatory = $true)][string]$Name, [string]$Extra = "", [int]$Samples = 40)
$j = Start-Job -ArgumentList $Name, $Extra -ScriptBlock {
    param($n, $x)
    if ($x) { foreach ($kv in $x -split ';') { $p = $kv -split '=', 2; Set-Item "Env:$($p[0])" $p[1] } }
    powershell -ExecutionPolicy Bypass -File C:\ProjectOogie\scripts\soak_hits.ps1 -Name $n -Minutes 2
}
Start-Sleep 70
$s = @()
for ($i = 0; $i -lt $Samples; $i++) {
    $p = Get-Process oogie_recomp -ErrorAction SilentlyContinue
    if ($p -and $p.MainWindowTitle -match '\| ([0-9.]+) fps') { $s += [int][double]$Matches[1] }
    Start-Sleep 2
}
Wait-Job $j -Timeout 240 | Out-Null
Remove-Job $j -Force
$avg = if ($s.Count) { ($s | Measure-Object -Average).Average } else { 0 }
"{0}: fps every 2 s: {1}  (mean {2:N1})" -f $Name, ($s -join ' '), $avg
