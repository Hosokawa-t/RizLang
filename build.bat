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
    src/chunk.c ^
    src/compiler.c ^
    src/vm.c ^
    src/codegen.c ^
    -lm

if %ERRORLEVEL% EQU 0 (
    echo.
    echo   ✓ Build successful: riz.exe
    echo.
) else (
    echo.
    echo   ✗ Build failed!
    echo.
)
