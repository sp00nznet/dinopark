# Build the boot exe and run it. Deletes the exe first: a failed compile leaves
# the previous one sitting there, and every "is it built?" check reads it as
# success, so the run measures a binary from some earlier idea.
#   scripts\run.ps1 [-Audit] [-Seconds 25] [-Tag 95]
param([switch]$Audit, [switch]$NoLift, [int]$Seconds = 25, [string]$Tag = "run")
$ErrorActionPreference = "Continue"
Set-Location (Split-Path $PSScriptRoot -Parent)

Get-Process dino_boot -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item work\dino_boot.exe -ErrorAction SilentlyContinue

# Lift here too. -Audit needs the lifter to emit the call-stack bookkeeping the
# build then references, so leaving the lift to the caller means a stale one
# fails the link -- or worse, links against the wrong idea of the generated code.
$defs = "/DRECOMP_MEM_HOOK"
if ($Audit) { $env:DINO_SPCHECK = "1"; $defs += " /DDINO_SPCHECK" }
if (-not $NoLift) {
    # The miss list carries over. It is only safe to feed back because the
    # lifter judges those addresses strictly: a dispatch miss is any address the
    # guest jumped to, and forcing all of them lifted function *tails* -- 1C69F
    # is `pop bp; retf`, reached by an indirect jump -- whose frames then went
    # unbalanced and tripped the game's own stack check. Nothing from here is
    # promoted now without a Borland prologue to prove it is an entry.
    python tools\lift_full.py *> work\lift.log
}
if ($Audit) { Remove-Item Env:\DINO_SPCHECK }
$srcs = @("src\boot.c","src\recomp\cpu.c","src\recomp\runtime16.c","src\recomp\dino_impl.c","src\recomp\video.c","src\recomp\music.c","src\recomp\ems.c")
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
$cl = "cl /nologo /O1 /W0 $defs /wd4244 /wd4267 /wd4101 /wd4102 /I src /I src\recomp " +
      ($srcs -join " ") + " /Fe:work\dino_boot.exe /Fo:work\obj\ user32.lib gdi32.lib winmm.lib"
cmd /c "`"$vcvars`" >nul 2>&1 && $cl" *> work\cl.log
if (-not (Test-Path work\dino_boot.exe)) {
    Write-Output "BUILD FAILED"
    (Get-Content work\cl.log) -replace "`0","" | Select-String ": error|: fatal" | Select-Object -First 8
    exit 1
}

# Nobody is at the keyboard under the harness, so ask for the scripted keys
# that walk the intro along. Without DINO_KEYS the game takes real ones only.
if (-not $env:DINO_KEYS) { $env:DINO_KEYS = "39" }
# And silence, unless asked otherwise. A test run is something you leave going
# in another window; it should not play music at whoever is sitting there. The
# game still registers its sequences either way, so nothing about the run
# changes -- set DINO_MUSIC=1 to hear it.
if (-not $env:DINO_MUSIC) { $env:DINO_MUSIC = "0"; $muted = $true }
$env:DINO_WATCHDOG = "$Seconds"
$err = "work\bt$Tag.txt"; $out = "work\bo$Tag.txt"
$p = Start-Process -FilePath ".\work\dino_boot.exe" -RedirectStandardError $err `
                   -RedirectStandardOutput $out -PassThru -NoNewWindow
$p.WaitForExit(($Seconds + 45) * 1000) | Out-Null
if (-not $p.HasExited) { $p.Kill() }
Remove-Item Env:\DINO_WATCHDOG
if ($muted) { Remove-Item Env:\DINO_MUSIC }
Write-Output "-> $err"
