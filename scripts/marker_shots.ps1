# Project Oogie: where does the "talk" marker over a character land in
# widescreen, and which voice lines play (GitHub #1, Bzshizzle 2026-10-28)?
# Starts Chapter -Chapter with -Settings, walks Jack with the presses in -Walk
# ("waitN" pauses N ms), then saves window shots logs\live\<Name>_N.png every
# second for -Shots seconds, pressing -Every every -EveryN seconds.
# -Wav: the game's sound output to logs\<Wav>.wav (RECOMP_AUDIO_WAV).
param([string]$Name = "mk1", [int]$Chapter = 2, [string]$Level = '0000', [string]$Settings = 'settings_mk_on',
      [string]$Walk = "ldown:900", [int]$Shots = 4, [int]$Settle = 8, [string]$Every = "", [int]$EveryN = 3,
      [string]$Wav = "")
$Root = Split-Path -Parent $PSScriptRoot
$env:OOGIE_START_CHAPTER = "$Chapter"
$env:OOGIE_TEST_SETTINGS = (Resolve-Path (Join-Path $Root "logs\$Settings.ini")).Path
if ($Wav) { $env:RECOMP_AUDIO_WAV = Join-Path $Root "logs\$Wav.wav" }
& (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name $Name -Cap 60 -Keep -TimeoutSec 5 -LevelFile ("stm_data\\" + $Level + "\.lz") -Marker no_marker_xyz | Out-Null
Remove-Item Env:OOGIE_START_CHAPTER, Env:OOGIE_TEST_SETTINGS, Env:RECOMP_AUDIO_WAV -ErrorAction SilentlyContinue
Start-Sleep -Seconds $Settle
$pad = Join-Path $Root "logs\pad_live.txt"
foreach ($p in $Walk.Split(',')) { if ($p -match '^wait(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]) } else { Add-Content -Path $pad -Value $p.Trim() } }
for ($i = 1; $i -le $Shots; $i++) {
    Start-Sleep -Seconds 1
    if ($Every -and ($i % $EveryN) -eq 0) { Add-Content -Path $pad -Value $Every }
    powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "window_shot.ps1") -Name "${Name}_$i" | Out-Null
}
& (Join-Path $PSScriptRoot "stop_test_game.ps1") | Out-Null
"shots: logs\live\${Name}_1..$Shots.png"
