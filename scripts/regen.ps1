# Project Oogie: XBE -> C -> oogie_recomp.exe on Windows (MSVC).
# Run from any folder:  powershell -ExecutionPolicy Bypass -File C:\ProjectOogie\scripts\regen.ps1
# Options: -SkipDisasm (reuse build_out analysis; use after editing recomp_manual.c only)
#          -SkipBuild  (generate C only)
param([switch]$SkipDisasm, [switch]$SkipBuild, [string]$Config = "Release")
# "Continue": Windows PowerShell 5.1 turns redirected native stderr into errors under "Stop".
# Failures are caught by checking $LASTEXITCODE after each tool instead.
$ErrorActionPreference = "Continue"
$Root  = Split-Path -Parent $PSScriptRoot
$XR    = if ($env:XBOXRECOMP) { $env:XBOXRECOMP } else { Join-Path $Root "third_party\xboxrecomp" }
$Xbe   = Join-Path $Root "game\default.xbe"
$Out   = Join-Path $Root "build_out"
$Logs  = Join-Path $Root "logs"
$Gen   = Join-Path $Root "port\src\recomp\gen"
$Seeds = Join-Path $Root "scripts\icall_seeds.json"
$PyExe = "python"; $PyPre = @()
if (Get-Command py -ErrorAction SilentlyContinue) { $PyExe = "py"; $PyPre = @("-3") }
New-Item -ItemType Directory -Force -Path $Logs, $Out | Out-Null

$expected = "667C5BD8ABC4A551EF6B7CBAB3BF3A45E7244605"
$actual = (Get-FileHash -Algorithm SHA1 $Xbe).Hash
if ($actual -ne $expected) { throw "default.xbe SHA-1 mismatch: $actual (expected $expected)" }

function Run-Py([string]$log, [string[]]$PyArgs) {
    Push-Location $XR
    try {
        & $PyExe @PyPre @PyArgs *> (Join-Path $Logs $log)
        if ($LASTEXITCODE -ne 0) { throw "python $($PyArgs[1]) failed, see logs\$log" }
    } finally { Pop-Location }
}

$Analysis = Join-Path $Root "game\default_analysis.json"
if (-not (Test-Path $Analysis)) { Run-Py "xbe_parser.log" @("-m","tools.xbe_parser",$Xbe,"--json",$Analysis) }

if (-not $SkipDisasm) {
    $seedArg = @(); if ((Test-Path $Seeds) -and (Get-Item $Seeds).Length -gt 2) { $seedArg = @("--seed-functions",$Seeds) }
    Write-Host "disasm (about 5 min)..."
    Run-Py "disasm.log" (@("-m","tools.disasm",$Xbe,"--analysis-json",$Analysis,"-o","$Out\disasm","--force") + $seedArg)
    Run-Py "func_id.log" @("-m","tools.func_id",$Xbe,"--functions","$Out\disasm\functions.json","--strings","$Out\disasm\strings.json","--xrefs","$Out\disasm\xrefs.json","-o","$Out\func_id")
    Run-Py "abi.log" @("-m","tools.abi_analysis",$Xbe,"--disasm-dir","$Out\disasm","--func-id-dir","$Out\func_id","--output-dir","$Out\abi")
}

Write-Host "recomp (about 2 min)..."
New-Item -ItemType Directory -Force -Path $Gen | Out-Null
Get-ChildItem $Gen -Filter "recomp_*.c" -ErrorAction SilentlyContinue | Remove-Item
# scripts\oogie_recomp.py = tools.recomp plus Oogie's in-memory lifter fixes
# (KNOWN_ISSUES #35); the checkout is not modified.
Run-Py "recomp.log" @((Join-Path $PSScriptRoot "oogie_recomp.py"),$Xbe,"--all","--split","1000","--game-name","OogiesRevenge",
    "--disasm-dir","$Out\disasm","--func-id-dir","$Out\func_id","--abi-dir","$Out\abi",
    "--exclude-manual",(Join-Path $Root "port\src\recomp_manual.c"),"--gen-dir",$Gen,"-o","$Out\recomp")
Select-String -Path (Join-Path $Logs "recomp.log") -Pattern "functions \(|unresolved" | ForEach-Object { $_.Line }

if (-not $SkipBuild) {
    $Build = Join-Path $Root "port\build"
    if (-not (Test-Path (Join-Path $Build "CMakeCache.txt"))) {
        cmake -S (Join-Path $Root "port") -B $Build -G "Visual Studio 17 2022" -A x64 "-DXBOXRECOMP_DIR=$XR" *> (Join-Path $Logs "configure.log")
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed, see logs\configure.log" }
    }
    Write-Host "build..."
    cmake --build $Build --config $Config --parallel *> (Join-Path $Logs "build.log")
    if ($LASTEXITCODE -ne 0) { throw "Build failed, see logs\build.log" }
    $Exe = Join-Path $Build "$Config\oogie_recomp.exe"
    New-Item -ItemType Directory -Force -Path (Join-Path $Root "bin") | Out-Null
    Copy-Item $Exe (Join-Path $Root "bin\oogie_recomp.exe") -Force
    # The PDB lets the crash handler name the faulting function.
    $Pdb = [IO.Path]::ChangeExtension($Exe, ".pdb")
    if (Test-Path $Pdb) { Copy-Item $Pdb (Join-Path $Root "bin\oogie_recomp.pdb") -Force }
    # SDL2.dll: PlayStation and other non-Xbox pads (port/src/pc_input.c, scripts\get_sdl2.ps1).
    $Sdl = Join-Path $Root "third_party\SDL2\lib\x64\SDL2.dll"
    if (Test-Path $Sdl) { Copy-Item $Sdl (Join-Path $Root "bin\SDL2.dll") -Force }
    else { Write-Host "note: no SDL2.dll (run scripts\get_sdl2.ps1): Xbox pads and keyboard only" }
    Get-Item (Join-Path $Root "bin\oogie_recomp.exe") | Format-List Name, Length, LastWriteTime
}
