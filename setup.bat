@echo off
title Riz Language - Dependency Setup
echo ========================================
echo   Riz Dependency Setup
echo ========================================
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup_libtorch.ps1"
if %errorlevel% neq 0 (
    echo.
    echo Setup FAILED. Please check your internet connection and try again.
    pause
    exit /b 1
)
echo.
echo Build the torch plugin now?
set /p choice="[y/n]: "
if /i "%choice%"=="y" (
    call build_torch.bat
)
echo.
echo Setup finished!
pause
