# Render a full-screen DinoPark .PIC via the lifted RLE blitter (Phase 4).
#   scripts\build_pic.ps1 [PIC path]
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

python tools\lift_dinopark.py | Out-Null

# MSVC. $env:VCVARS overrides it; otherwise take the first install that is
# there, so this is not pinned to one edition on one machine.
$vcvars = $env:VCVARS
if (-not $vcvars) {
    $vcvars = @("Enterprise","Professional","Community","BuildTools") |
        ForEach-Object { "${env:ProgramFiles}\Microsoft Visual Studio\2022\$_\VC\Auxiliary\Build\vcvars64.bat" } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $vcvars) { Write-Output "no vcvars64.bat found; set $env:VCVARS"; exit 1 }
$cl = "cl /nologo /W2 /I src /I src\recomp src\decode_pic.c src\recomp\cpu.c src\recomp\video.c src\recomp\gen\dino_decode.c /Fe:work\dino_pic.exe /Fo:work\ user32.lib gdi32.lib"
cmd /c "`"$vcvars`" >nul 2>&1 && $cl" | Out-Null

$pic = if ($args.Count -ge 1) { $args[0] } else { "original\AUCTION.PIC" }
& .\work\dino_pic.exe $pic
python tools\render_pic.py
