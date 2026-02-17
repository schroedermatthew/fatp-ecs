@echo off
REM build.bat — FAT-P ECS build script (Windows batch)
REM Usage: build.bat [clean] [novisual] [debug]

setlocal

set BUILD_DIR=build
set CONFIG=Release
set FATP_DIR=../FatP/include
set TOOLCHAIN=C:/Program Files/Microsoft Visual Studio/18/Professional/VC/vcpkg/scripts/buildsystems/vcpkg.cmake
set VISUAL=ON
set BENCH=OFF

:parse_args
if "%~1"=="" goto :done_args
if /i "%~1"=="clean" (
    if exist %BUILD_DIR% (
        echo Cleaning build directory...
        rd /s /q %BUILD_DIR%
    )
)
if /i "%~1"=="novisual" set VISUAL=OFF
if /i "%~1"=="debug" set CONFIG=Debug
if /i "%~1"=="bench" set BENCH=ON
shift
goto :parse_args
:done_args

echo Configuring (%CONFIG%)...
cmake -B %BUILD_DIR% -DFATP_INCLUDE_DIR=%FATP_DIR% -DFATP_ECS_BUILD_VISUAL_DEMO=%VISUAL% -DFATP_ECS_BUILD_BENCH=%BENCH% "-DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN%" -DVCPKG_TARGET_TRIPLET=x64-windows
if errorlevel 1 exit /b %errorlevel%

echo Building (%CONFIG%)...
cmake --build %BUILD_DIR% --config %CONFIG%
if errorlevel 1 exit /b %errorlevel%

echo Running tests...
ctest --test-dir %BUILD_DIR% -C %CONFIG% --output-on-failure
if errorlevel 1 exit /b %errorlevel%

echo.
echo Build complete. Binaries in %BUILD_DIR%\%CONFIG%\
echo   demo.exe          — terminal demo
if "%VISUAL%"=="ON" echo   visual_demo.exe   — SDL2 visual demo
if "%BENCH%"=="ON" echo   benchmark.exe     — FAT-P ECS vs EnTT benchmark

endlocal
