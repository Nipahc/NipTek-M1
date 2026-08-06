# Windows build entry point for the M1 firmware.
#
# The repo's CMakePresets.json sets full compiler paths, but cmake/gcc-arm-none-eabi.cmake
# then overrides them with the bare `arm-none-eabi-` prefix -- so the toolchain must be on
# PATH regardless of what the preset says (the toolchain file's own comment notes this).
# This script puts the required tools on PATH for the duration of the build only; it does
# not modify your system environment.
#
# Usage:  .\build_windows.ps1            # release
#         .\build_windows.ps1 -Debug     # debug
#         .\build_windows.ps1 -Clean     # wipe the build dir first

param(
    [switch]$Debug,
    [switch]$Clean
)

# Deliberately NOT 'Stop': in Windows PowerShell 5.1 a native exe writing to stderr
# raises NativeCommandError under 'Stop', and both cmake and arm-none-eabi-gcc write
# ordinary progress and warnings there. Success is judged by $LASTEXITCODE instead.
$ErrorActionPreference = 'Continue'

$armBin   = 'C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin'
$ninjaDir = 'C:\Users\crims\AppData\Local\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe'
$cmakeDir = 'C:\Program Files\CMake\bin'
$srecDir  = 'C:\Users\crims\AppData\Local\Programs\srec_cat_shim'

foreach ($d in @($armBin, $ninjaDir, $cmakeDir, $srecDir)) {
    if (-not (Test-Path $d)) { throw "Required tool directory not found: $d" }
}

$env:Path = "$armBin;$ninjaDir;$cmakeDir;$srecDir;" + $env:Path

$preset = if ($Debug) { 'gcc-14_2_build-debug' } else { 'gcc-14_2_build-release' }
$buildDir = "out/build/$preset"

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

# srec_cat here is a verified stand-in covering only the CRC post-build step, not real
# SRecord. Install SRecord from https://srecord.sourceforge.net/ and put srec_cat.exe
# earlier on PATH if you need the genuine tool.
if (-not (Get-Command srec_cat.exe -ErrorAction SilentlyContinue)) {
    Write-Host "note: using the srec_cat stand-in (CRC step only), not real SRecord." -ForegroundColor DarkYellow
}

Write-Host "Configuring ($preset)..." -ForegroundColor Cyan
cmake --preset $preset
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

Write-Host "Building..." -ForegroundColor Cyan
cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

Write-Host "`nArtifacts in $buildDir :" -ForegroundColor Green
Get-ChildItem "$buildDir\NipTek_M1_v0800.*" | Select-Object Name, Length | Format-Table -AutoSize
Write-Host "Flash NipTek_M1_v0800_wCRC.bin (the CRC-bearing image)." -ForegroundColor Green
