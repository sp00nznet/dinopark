# Render a full-screen DinoPark .PIC via the lifted RLE blitter (Phase 4).
#   scripts\build_pic.ps1 [PIC path]
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

python tools\lift_dinopark.py | Out-Null

$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
$cl = "cl /nologo /W2 /I src /I src\recomp src\decode_pic.c src\recomp\cpu.c src\recomp\video.c src\recomp\gen\dino_decode.c /Fe:work\dino_pic.exe /Fo:work\ user32.lib gdi32.lib"
cmd /c "`"$vcvars`" >nul 2>&1 && $cl" | Out-Null

$pic = if ($args.Count -ge 1) { $args[0] } else { "original\AUCTION.PIC" }
& .\work\dino_pic.exe $pic
python tools\render_pic.py
