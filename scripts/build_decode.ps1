# Build & run the lifted DPCM sprite-decoder harness (Phase 4).
# Regenerates the lifted C, compiles against recomp16, runs on a real sprite.
#   scripts\build_decode.ps1 [ACT path] [sprite index]
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

python tools\lift_dinopark.py

$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
$cl = "cl /nologo /W2 /I src /I src\recomp src\decode_harness.c src\recomp\cpu.c src\recomp\gen\dino_decode.c /Fe:work\dino_decode.exe /Fo:work\"
cmd /c "`"$vcvars`" >nul 2>&1 && $cl"

$act = if ($args.Count -ge 1) { $args[0] } else { "original\ALBERT.ACT" }
$idx = if ($args.Count -ge 2) { $args[1] } else { "0" }
& .\work\dino_decode.exe $act $idx
