# Build & run the lifted DPCM sprite-decoder harness (Phase 4).
# Regenerates the lifted C, compiles against recomp16, runs on a real sprite.
#   scripts\build_decode.ps1 [ACT path] [sprite index]
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

python tools\lift_dinopark.py

# MSVC. $env:VCVARS overrides it; otherwise take the first install that is
# there, so this is not pinned to one edition on one machine.
$vcvars = $env:VCVARS
if (-not $vcvars) {
    $vcvars = @("Enterprise","Professional","Community","BuildTools") |
        ForEach-Object { "${env:ProgramFiles}\Microsoft Visual Studio\2022\$_\VC\Auxiliary\Build\vcvars64.bat" } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $vcvars) { Write-Output "no vcvars64.bat found; set $env:VCVARS"; exit 1 }
$cl = "cl /nologo /W2 /I src /I src\recomp src\decode_harness.c src\recomp\cpu.c src\recomp\gen\dino_decode.c /Fe:work\dino_decode.exe /Fo:work\"
cmd /c "`"$vcvars`" >nul 2>&1 && $cl"

$act = if ($args.Count -ge 1) { $args[0] } else { "original\ALBERT.ACT" }
$idx = if ($args.Count -ge 2) { $args[1] } else { "0" }
& .\work\dino_decode.exe $act $idx
