# build.ps1 — FAT-P ECS build script (PowerShell)
# Usage: .\build.ps1 [-Clean] [-NoVisual] [-Debug]

param(
    [switch]$Clean,
    [switch]$NoVisual,
    [switch]$Debug
)

$ErrorActionPreference = "Stop"

$BuildDir = "build"
$Config = if ($Debug) { "Debug" } else { "Release" }
$FatPDir = "../FatP/include"
$ToolchainFile = "C:/Program Files/Microsoft Visual Studio/18/Professional/VC/vcpkg/scripts/buildsystems/vcpkg.cmake"

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}

$CMakeArgs = @(
    "-B", $BuildDir
    "-DFATP_INCLUDE_DIR=$FatPDir"
    "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile"
    "-DVCPKG_TARGET_TRIPLET=x64-windows"
)

if (-not $NoVisual) {
    $CMakeArgs += "-DFATP_ECS_BUILD_VISUAL_DEMO=ON"
}

Write-Host "Configuring ($Config)..." -ForegroundColor Cyan
cmake @CMakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Building ($Config)..." -ForegroundColor Cyan
cmake --build $BuildDir --config $Config
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Running tests..." -ForegroundColor Cyan
ctest --test-dir $BuildDir -C $Config --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "Build complete. Binaries in $BuildDir/$Config/" -ForegroundColor Green
Write-Host "  demo.exe          — terminal demo"
if (-not $NoVisual) {
    Write-Host "  visual_demo.exe   — SDL2 visual demo"
}
