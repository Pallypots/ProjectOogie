# Project Oogie: start a chapter with the test cheats settings (Infinite
# Health, so the run is not cut short) and run scripts\hitscan.py while it is
# played. Stops the game it started.
#   powershell -File scripts\hitscan.ps1 [-Chapter 1] [-Seconds 150]
#   -Scene "2,0": start in that scene instead (OOGIE_START_SCENE, boss tests);
#   -Shot: save a game frame 20 s in (logs\live\f*.bmp, then logs\<Name>.png)
param([int]$Chapter = 1, [string]$Name = "hs1", [int]$Seconds = 150, [string]$Settings = "logs\settings_cheats.ini",
      [string]$Record = "",   # e.g. "0x3F6000:0x3F7000 0x420E00:0x421400": scripts\hitrec.py instead of hitscan.py
      [string]$Scene = "", [string]$World = "", [switch]$Shot, [switch]$NoBot, [string]$ShotAt = "")   # -ShotAt 40,70: frames that many seconds in, logs\<Name>_<s>.png   # -World "0,9,2,0": OOGIE_START_WORLD
$Root = Split-Path -Parent $PSScriptRoot
if (Get-Process oogie_recomp -ErrorAction SilentlyContinue) { "a game is already running; not starting"; exit 1 }
$env:OOGIE_TEST_SETTINGS = (Resolve-Path (Join-Path $Root $Settings)).Path
$level = @{ 1 = '0000'; 2 = '0000'; 3 = '0100'; 4 = '0100'; 5 = '0100'; 6 = '0100'; 7 = '0200'; 10 = '0302'; 13 = '0[1-9]\d\d'; 15 = '0[1-9]\d\d'; 16 = '0500' }
$lf = $level[$Chapter]
if ($World) { $env:OOGIE_START_WORLD = $World; $env:OOGIE_WARP_LOG = "1"; $lf = '\d{4}' }
if ($Scene) { $env:OOGIE_START_SCENE = $Scene; $env:OOGIE_WARP_LOG = "1"; $lf = '\d{4}' }
if ($Chapter -ge 2) { $env:OOGIE_START_CHAPTER = "$Chapter" }
& (Join-Path $PSScriptRoot "timing_cutscene.ps1") -Name $Name -Cap 60 -Keep -TimeoutSec 5 -LevelFile ("stm_data\\" + $lf + "\.lz") -Marker "no_marker_xyz" | Out-Null
$env:OOGIE_START_CHAPTER = $null; $env:OOGIE_START_SCENE = $null; $env:OOGIE_START_WORLD = $null; $env:OOGIE_WARP_LOG = $null
$p = Get-Process oogie_recomp -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "game not running"; exit 1 }
Start-Sleep -Seconds 5
if ($Shot) {
    Start-Sleep -Seconds 15
    Get-ChildItem (Join-Path $Root "logs\live") -Filter "f*.bmp" -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^f\d+\.bmp$' } | Remove-Item
    $fl = Join-Path $Root "logs\flipdump.flag"; Set-Content $fl "1"; Start-Sleep -Milliseconds 250; Remove-Item $fl
    Start-Sleep -Seconds 1
    $b = Get-ChildItem (Join-Path $Root "logs\live") -Filter "f*.bmp" | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($b) { Add-Type -AssemblyName System.Drawing; $i = [System.Drawing.Image]::FromFile($b.FullName); $i.Save((Join-Path $Root "logs\$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
}
$at = @($ShotAt -split ',' | Where-Object { $_ -match '^\d+$' } | ForEach-Object { [int]$_ })
if ($at.Count -and -not $NoBot -and -not $Record) {
    # the bot as its own process, frames taken from here meanwhile
    Add-Type -AssemblyName System.Drawing
    $bot = Start-Process py -ArgumentList "-3", (Join-Path $PSScriptRoot "hitscan.py"), "$Seconds" -NoNewWindow -PassThru
    $t0 = Get-Date
    foreach ($s in $at) {
        while (((Get-Date) - $t0).TotalSeconds -lt $s -and -not $bot.HasExited) { Start-Sleep -Milliseconds 200 }
        Get-ChildItem (Join-Path $Root "logs\live") -Filter "f*.bmp" -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^f\d+\.bmp$' } | Remove-Item
        $fl = Join-Path $Root "logs\flipdump.flag"; Set-Content $fl "1"; Start-Sleep -Milliseconds 250; Remove-Item $fl; Start-Sleep -Seconds 1
        $b = Get-ChildItem (Join-Path $Root "logs\live") -Filter "f*.bmp" | Sort-Object LastWriteTime | Select-Object -Last 1
        if ($b) { $i = [System.Drawing.Image]::FromFile($b.FullName); $i.Save((Join-Path $Root "logs\$($Name)_$s.png"), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
    }
    $bot.WaitForExit()
}
elseif ($NoBot) { Start-Sleep -Seconds $Seconds }
elseif ($Record -eq "move") { py -3 (Join-Path $PSScriptRoot "moverec.py") $Name $Seconds }
elseif ($Record) { py -3 (Join-Path $PSScriptRoot "hitrec.py") $Name $Seconds $Record.Split(' ') }
else { py -3 (Join-Path $PSScriptRoot "hitscan.py") $Seconds }
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
"done, game $($p.Id) stopped"
