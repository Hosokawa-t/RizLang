@echo off
setlocal

set PYTHON_DIR=C:\Users\Owner\AppData\Local\Programs\Python\Python310
set INCLUDE_DIR=%PYTHON_DIR%\include
set LIB_DIR=%PYTHON_DIR%\libs

echo === Compiling plugin_python.dll ===
gcc -shared -Wall -Wextra -O2 -fPIC -Isrc -I"%INCLUDE_DIR%" examples\plugin_python.c -o plugin_python.dll -L"%LIB_DIR%" -lpython310

if %errorlevel% neq 0 (
    echo [ERROR] Build failed!
    exit /b %errorlevel%
)

echo Build OK
echo Run with: .\riz.exe examples\python_test.riz
