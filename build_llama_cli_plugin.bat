@echo off
chcp 65001 >nul
REM Build Riz plugin that shells out to llama.cpp llama-cli (GGUF inference).
REM Requires gcc on PATH (e.g. MSYS2 MinGW64).

echo === Building plugin_llama_cli.dll ===
gcc -shared -O2 -Isrc -o plugin_llama_cli.dll examples\plugin_llama_cli.c
if errorlevel 1 (echo FAILED & exit /b 1) else (echo OK: plugin_llama_cli.dll)
echo.
echo Place llama-cli on PATH or set LLAMA_CLI to its full path.
echo Run: riz examples\infer_llama.riz
echo See: examples\LLAMA_INFER.md
