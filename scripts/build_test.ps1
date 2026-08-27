# Unit-harness builds: drive one lifted function directly instead of booting.
#   scripts\build_test.ps1 sprintf  [args...]
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

$which = if ($args.Count -ge 1) { $args[0] } else { "sprintf" }
$rest  = if ($args.Count -gt 1) { $args[1..($args.Count-1)] } else { @() }

if (-not (Test-Path "src\recomp\gen\recomp_000.c")) { python tools\lift_full.py | Out-Null }

# MSVC. $env:VCVARS overrides it; otherwise take the first install that is
# there, so this is not pinned to one edition on one machine.
$vcvars = $env:VCVARS
if (-not $vcvars) {
    $vcvars = @("Enterprise","Professional","Community","BuildTools") |
        ForEach-Object { "${env:ProgramFiles}\Microsoft Visual Studio\2022\$_\VC\Auxiliary\Build\vcvars64.bat" } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $vcvars) { Write-Output "no vcvars64.bat found; set $env:VCVARS"; exit 1 }
$srcs = @("src\test_$which.c","src\recomp\cpu.c","src\recomp\runtime16.c","src\recomp\dino_impl.c","src\recomp\video.c","src\recomp\music.c","src\recomp\ems.c","src\recomp\digi.c")
$srcs += (Get-ChildItem "src\recomp\gen\recomp_*.c" | ForEach-Object { $_.FullName })
New-Item -ItemType Directory -Force work\obj2 | Out-Null
Remove-Item "work\test_$which.exe" -ErrorAction SilentlyContinue
$cl = "cl /nologo /O1 /W0 /DRECOMP_MEM_HOOK /wd4244 /wd4267 /wd4101 /wd4102 /I src /I src\recomp " +
      ($srcs -join " ") + " /Fe:work\test_$which.exe /Fo:work\obj2\ user32.lib gdi32.lib winmm.lib"
cmd /c "`"$vcvars`" >nul 2>&1 && $cl" *> work\cl_test.log
if (-not (Test-Path "work\test_$which.exe")) {
    Write-Output "BUILD FAILED"
    (Get-Content work\cl_test.log) -replace "`0","" | Select-String "error" | Select-Object -First 8
    exit 1
}
& ".\work\test_$which.exe" @rest
