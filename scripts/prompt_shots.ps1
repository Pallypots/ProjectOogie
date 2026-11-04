# Project Oogie: frames of the pause menu's button prompts for each Button
# Prompts style (0.2.7 texture packs). For each logs\settings_tp<N>.ini:
# starts chapter 1, opens the pause menu, saves logs\prompt_<N>.png, stops
# the game it started.
#   powershell -File scripts\prompt_shots.ps1 [-Styles 1,2,3]
param([string]$Styles = "1,2,3")
$Root = Split-Path -Parent $PSScriptRoot
$pad = Join-Path $Root "logs\pad_live.txt"
Add-Type -AssemblyName System.Drawing
foreach ($st in ($Styles -split ',')) {
    if (Get-Process oogie_recomp -ErrorAction SilentlyContinue) { "a game is already running; not starting"; exit 1 }
    $env:OOGIE_TEST_SETTINGS = (Resolve-Path (Join-Path $Root "logs\settings_tp$st.ini")).Path
    & (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name "prompt_$st" -Cap 60 -Keep -TimeoutSec 5 -LevelFile "stm_data\\0000\.lz" -Marker "no_marker_xyz" | Out-Null
    $env:OOGIE_TEST_SETTINGS = $null
    $p = Get-Process oogie_recomp -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { "game not running"; continue }
    Start-Sleep -Seconds 45                     # past the chapter's opening scene
    Add-Content -Path $pad -Value "start:150"; Start-Sleep -Seconds 3
    Get-ChildItem (Join-Path $Root "logs\live") -Filter "f*.bmp" -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^f\d+\.bmp$' } | Remove-Item
    $fl = Join-Path $Root "logs\flipdump.flag"; Set-Content $fl "1"; Start-Sleep -Milliseconds 250; Remove-Item $fl; Start-Sleep -Seconds 1
    $b = Get-ChildItem (Join-Path $Root "logs\live") -Filter "f*.bmp" | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($b) { $i = [System.Drawing.Image]::FromFile($b.FullName); $i.Save((Join-Path $Root "logs\prompt_$st.png"), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    "style ${st}: " +((Select-String -Path (Join-Path $Root "logs\prompt_$st.err.log") -Pattern "\[TEXPACK\] \d+ repl" | Select-Object -Last 1).Line)
}
