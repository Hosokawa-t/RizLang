@echo off
chcp 65001 >nul
REM +------------------------------------------------------+
REM |  Riz Programming Language -- Windows Build Script    |
REM +------------------------------------------------------+

echo.
echo   Building Riz...
echo.

gcc -Wall -Wextra -std=c11 -O2 -Isrc -o riz.exe ^
    src/diagnostic.c ^
    src/main.c ^
    src/lexer.c ^
    src/parser.c ^
    src/ast.c ^
    src/static_analysis.c ^
    src/interpreter.c ^
    src/value.c ^
    src/environment.c ^
    src/chunk.c ^
    src/compiler.c ^
    src/vm.c ^
    src/codegen.c ^
    src/pkg.c ^
    src/riz_import.c ^
    src/riz_env.c ^
    -lm

echo   Building Standard Library Plugins...
gcc -shared -O2 -Isrc -o plugin_math.dll src/plugin_math.c -lm
gcc -shared -O2 -Isrc -o plugin_os.dll src/plugin_os.c

if %ERRORLEVEL% EQU 0 (
    echo.
    echo   [OK] Build successful: riz.exe
    echo.
) else (
    echo.
    echo   [ERROR] Build failed!
    echo.
)
