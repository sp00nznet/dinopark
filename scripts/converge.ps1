# Iterative boot convergence: lift (forcing accumulated misses) -> build -> boot
# (watchdog-bounded) -> collect misses. Repeat, watching the cumulative miss count
# and whether the game reaches file opens / video init.
param([int]$rounds = 6)
$ErrorActionPreference = "Continue"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

for ($r = 1; $r -le $rounds; $r++) {
    Get-Process dino_boot -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
    Remove-Item -Recurse -Force work\obj -ErrorAction SilentlyContinue

    python tools\lift_full.py *> work\lift.log
    $srcs = @("src\boot.c","src\recomp\cpu.c","src\recomp\runtime16.c")
    $srcs += (Get-ChildItem "src\recomp\gen\recomp_*.c" | ForEach-Object { $_.FullName })
    $cl = "cl /nologo /O1 /W0 /wd4244 /wd4267 /wd4101 /wd4102 /I src /I src\recomp " + ($srcs -join " ") + " /Fe:work\dino_boot.exe /Fo:work\obj\ "
    New-Item -ItemType Directory -Force work\obj | Out-Null
    cmd /c "`"$vcvars`" >nul 2>&1 && $cl" *> work\cl.log
    if (-not (Test-Path work\dino_boot.exe)) { Write-Output "round $r : BUILD FAILED"; Get-Content work\cl.log -Tail 3; break }

    $env:DINO_TRACE = "1"; $env:DINO_DBG = "1"; $env:DINO_WATCHDOG = "6"
    $p = Start-Process -FilePath ".\work\dino_boot.exe" -RedirectStandardError "work\bt.txt" -RedirectStandardOutput "work\bo.txt" -PassThru -NoNewWindow
    $p.WaitForExit(20000) | Out-Null
    if (-not $p.HasExited) { $p.Kill() }
    Remove-Item Env:\DINO_TRACE, Env:\DINO_DBG, Env:\DINO_WATCHDOG

    $nmiss = if (Test-Path work\dino_misses.txt) { (Get-Content work\dino_misses.txt | Measure-Object -Line).Lines } else { 0 }
    $ndisp = (Get-Content work\bt.txt | Select-String "\[disp\]|^\[[0-9]" ).Count
    $opens = (Get-Content work\bt.txt | Select-String "AH=3D").Count
    $vmode = (Get-Content work\bt.txt | Select-String "set mode").Count
    Write-Output ("round {0} : misses={1} dispatch_lines={2} file_opens={3} video_mode={4}" -f $r,$nmiss,$ndisp,$opens,$vmode)
}
Write-Output "=== final: any file opens? ==="
Get-Content work\bt.txt | Select-String "AH=3D|open '|set mode" | Select-Object -First 10
