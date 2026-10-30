# Project Oogie: Chapter 1 from a new game (Easy), attack presses for
# -Minutes to clear the first battle and reach Mr. Hyde (the save point,
# GitHub #1 voice lines). OOGIE_XACT_TRACE=2 logs every XACT call; a window
# shot every 10 s to logs\live\<Name>_NNN.png.
param([string]$Name = "hy1", [int]$Minutes = 4, [string]$Settings = 'settings_mk_43', [string]$Xact = "2", [switch]$Dsv,
      [string]$After = "", [int]$AfterShots = 0, [switch]$NoStop)   # presses after the fight ("waitN" pauses), then a shot every 2 s
$Root = Split-Path -Parent $PSScriptRoot
$env:OOGIE_XACT_TRACE = $Xact
if ($Dsv) { $env:OOGIE_DSV_TRACE = "1" }   # voice-list tracer (#47)
$env:RECOMP_AUDIO_WAV = Join-Path $Root "logs\$Name.wav"
$env:OOGIE_TEST_SETTINGS = (Resolve-Path (Join-Path $Root "logs\$Settings.ini")).Path
& (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name $Name -Cap 60 -Keep -TimeoutSec 120 | Out-Null   # waits for rsd_3mhd.lz: the opening scene hands over to play
Remove-Item Env:OOGIE_XACT_TRACE, Env:OOGIE_DSV_TRACE, Env:RECOMP_AUDIO_WAV, Env:OOGIE_TEST_SETTINGS -ErrorAction SilentlyContinue
$pad = Join-Path $Root "logs\pad_live.txt"
$seq = @("x:120", "x:120", "x:120", "lleft:150", "x:120", "x:120", "x:120", "lright:150", "x:120", "x:120", "a:120", "lup:150", "x:120", "x:120", "x:120", "ldown:150")
$t0 = Get-Date; $k = 0; $shot = 0; $next = 10
while (((Get-Date) - $t0).TotalMinutes -lt $Minutes -and (Get-Process oogie_recomp -ErrorAction SilentlyContinue)) {
    Add-Content -Path $pad -Value $seq[$k % $seq.Count] -ErrorAction SilentlyContinue; $k++
    Start-Sleep -Milliseconds 350
    if (((Get-Date) - $t0).TotalSeconds -ge $next) {
        $shot++; $next += 10
        powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "window_shot.ps1") -Name ("{0}_{1:D3}" -f $Name, $shot) | Out-Null
    }
}
if ($After -and (Get-Process oogie_recomp -ErrorAction SilentlyContinue)) {
    foreach ($p in $After.Split(',')) { if ($p -match '^wait(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]) } else { Add-Content -Path $pad -Value $p.Trim() -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 400 } }
    for ($i = 1; $i -le $AfterShots; $i++) {
        Start-Sleep -Seconds 2
        powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "window_shot.ps1") -Name ("{0}_t{1:D3}" -f $Name, $i) | Out-Null
    }
}
if (-not $NoStop) { & (Join-Path $PSScriptRoot "stop_test_game.ps1") | Out-Null }
"done: $shot shots"
