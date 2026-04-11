@echo off
chcp 65001 >nul
set CMAKE_PREFIX_PATH=d:\Riz\deps\libtorch\share\cmake\Torch

echo === Generating build files with CMake (MSVC) ===
cmake -G "Visual Studio 16 2019" -A x64 -DTorch_DIR="%CMAKE_PREFIX_PATH%" -B build_torch -S .
if errorlevel 1 (echo CMake configuration FAILED & exit /b 1) else (echo CMake OK)

echo === Compiling plugin_torch.dll ===
cmake --build build_torch --config Release
if errorlevel 1 (echo Build FAILED & exit /b 1) else (echo Build OK)

echo === Copying DLL to root ===
copy /Y build_torch\Release\plugin_torch.dll .
if errorlevel 1 (echo Copy FAILED & exit /b 1) else (echo Copy OK)

echo === All done ===
echo Run with: set PATH=d:\Riz\deps\libtorch\lib;%%PATH%% ^&^& riz examples\train_sine.riz
