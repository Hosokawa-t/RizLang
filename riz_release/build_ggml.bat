@echo off
chcp 65001 >nul
set PATH=C:\msys64\mingw64\bin;%PATH%

echo === Compiling ggml.c ===
gcc -c -O2 -DNDEBUG -DGGML_VERSION=\"0.0.0\" -DGGML_COMMIT=\"local\" -DGGML_BUILD_NUMBER=0 -Ideps/ggml/include -Ideps/ggml/src -o deps/ggml/build_ggml.o deps/ggml/src/ggml.c
if errorlevel 1 (echo ggml.c: FAILED & exit /b 1) else (echo ggml.c: OK)

echo === Compiling ggml-alloc.c ===
gcc -c -O2 -DNDEBUG -Ideps/ggml/include -Ideps/ggml/src -o deps/ggml/build_ggml_alloc.o deps/ggml/src/ggml-alloc.c
if errorlevel 1 (echo ggml-alloc.c: FAILED & exit /b 1) else (echo ggml-alloc.c: OK)

echo === Compiling ggml-quants.c ===
gcc -c -O2 -Ideps/ggml/include -Ideps/ggml/src -o deps/ggml/build_quants.o deps/ggml/src/ggml-quants.c
if errorlevel 1 (echo ggml-quants.c: FAILED & exit /b 1)

echo === Compiling C++ components ===
g++ -c -O2 -O2 -std=c++11 -Ideps/ggml/include -Ideps/ggml/src -o deps/ggml/build_threading.o deps/ggml/src/ggml-threading.cpp
g++ -c -O2 -O2 -std=c++11 -Ideps/ggml/include -Ideps/ggml/src -o deps/ggml/build_backend.o deps/ggml/src/ggml-backend.cpp
if errorlevel 1 (echo C++ components: FAILED & exit /b 1)

echo === Archiving libggml.a ===
ar rcs deps/ggml/libggml.a deps/ggml/build_ggml.o deps/ggml/build_ggml_alloc.o deps/ggml/build_quants.o deps/ggml/build_threading.o deps/ggml/build_backend.o
if errorlevel 1 (echo archive: FAILED & exit /b 1) else (echo archive: OK)

echo === Building plugin_ggml.dll ===
g++ -shared -O2 -Isrc -Ideps/ggml/include -o plugin_ggml.dll examples/llm/plugin_ggml.c deps/ggml/libggml.a -lm
if errorlevel 1 (echo plugin: FAILED & exit /b 1) else (echo plugin: OK)

echo === All done ===
