# Build the full DinoPark recompilation boot harness (playable bring-up).
#   scripts\build_boot.ps1   then  work\dino_boot.exe
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

python tools\lift_full.py | Out-Null

$srcs = @("src\boot.c", "src\recomp\cpu.c", "src\recomp\runtime16.c","src\recomp\dino_impl.c")
$srcs += (Get-ChildItem "src\recomp\gen\recomp_*.c" | ForEach-Object { $_.FullName })

# MSVC. $env:VCVARS overrides it; otherwise take the first install that is
# there, so this is not pinned to one edition on one machine.
$vcvars = $env:VCVARS
if (-not $vcvars) {
    $vcvars = @("Enterprise","Professional","Community","BuildTools") |
        ForEach-Object { "${env:ProgramFiles}\Microsoft Visual Studio\2022\$_\VC\Auxiliary\Build\vcvars64.bat" } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $vcvars) { Write-Output "no vcvars64.bat found; set $env:VCVARS"; exit 1 }
# /O1 keeps the 90k-line build fast-ish; /wd disables noisy generated-code warnings
$opts = "/nologo /DRECOMP_MEM_HOOK /O1 /W0 /wd4244 /wd4267 /wd4101 /wd4102 /I src /I src\recomp"
$cl = "cl $opts " + ($srcs -join " ") + " /Fe:work\dino_boot.exe /Fo:work\obj\ "
New-Item -ItemType Directory -Force work\obj | Out-Null
cmd /c "`"$vcvars`" >nul 2>&1 && $cl"
