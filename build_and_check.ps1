# build_and_check.ps1
$LIBTORCH_PATH = "d:\Riz\deps\libtorch"
$BUILD_DIR = "d:\Riz\build_check"
if (Test-Path $BUILD_DIR) { Remove-Item -Recurse -Force $BUILD_DIR }
New-Item -ItemType Directory -Path $BUILD_DIR
cd $BUILD_DIR
cmake -G "Visual Studio 16 2019" -A x64 -S d:\Riz -B .
cmake --build . --config Release --target check_gpu_direct
if ($LASTEXITCODE -eq 0) {
    $env:PATH = "$LIBTORCH_PATH\lib;" + $env:PATH
    .\Release\check_gpu_direct.exe
} else {
    Write-Host "Build failed." -ForegroundColor Red
}
