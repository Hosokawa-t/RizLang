@echo off
REM ╔════════════════════════════════════════════════════╗
REM ║  Riz Programming Language — Windows Build Script   ║
REM ╚════════════════════════════════════════════════════╝

echo.
echo   Building Riz...
echo.

gcc -Wall -Wextra -std=c11 -O2 -Isrc -o riz.exe ^
    src/main.c ^
    src/lexer.c ^
    src/parser.c ^
    src/ast.c ^
    src/interpreter.c ^
    src/value.c ^
    src/environment.c ^
    -lm

if %ERRORLEVEL% EQU 0 (
    echo.
    echo   ✓ Build successful: riz.exe
    echo.
    echo   Usage:
    echo     riz              Start REPL
    echo     riz file.riz     Execute a file
    echo     riz --help       Show help
    echo.
) else (
    echo.
    echo   ✗ Build failed!
    echo.
    echo   Make sure you have GCC installed:
    echo     winget install -e --id GnuWin32.Make
    echo     or install MinGW-w64 from https://www.mingw-w64.org/
    echo.
)
