# setup_libtorch.ps1
# Automates downloading and setting up LibTorch based on GPU availability

$INSTALL_DIR = "d:\Riz\deps"
$TARGET_PATH = "$INSTALL_DIR\libtorch"
$ZIP_PATH = "$INSTALL_DIR\libtorch.zip"

if (!(Test-Path $INSTALL_DIR)) {
    New-Item -ItemType Directory -Path $INSTALL_DIR | Out-Null
}

# 1. GPU Detection
Write-Host "--- Detecting GPU ---" -ForegroundColor Cyan
$hasGpu = $false
try {
    $smi = nvidia-smi --query-gpu=name --format=csv,noheader
    if ($smi) {
        Write-Host "Found GPU: $smi" -ForegroundColor Green
        $hasGpu = $true
    }
} catch {
    Write-Host "No NVIDIA GPU detected or drivers not installed. Using CPU version." -ForegroundColor Yellow
}

# 2. Determine Download URL (LibTorch 2.4.0)
$url = ""
if ($hasGpu) {
    # Using CUDA 12.1 variant as it's common for modern drivers (like 560.x)
    $url = "https://download.pytorch.org/libtorch/cu121/libtorch-win-shared-with-deps-2.4.0%2Bcu121.zip"
    Write-Host "Target: LibTorch with CUDA 12.1 Support"
} else {
    $url = "https://download.pytorch.org/libtorch/cpu/libtorch-win-shared-with-deps-2.4.0%2Bcpu.zip"
    Write-Host "Target: LibTorch CPU-only"
}

# 3. Download
Write-Host "--- Downloading (approx. 2.5GB)... ---" -ForegroundColor Cyan
Write-Host "URL: $url"
if (Test-Path $ZIP_PATH) { Remove-Item $ZIP_PATH }

# Use curl.exe for better compatibility and progress display
curl.exe -L -o $ZIP_PATH $url
if ($LASTEXITCODE -ne 0) {
    Write-Host "Download FAILED!" -ForegroundColor Red
    exit 1
}

# 4. Extract
Write-Host "--- Extracting... ---" -ForegroundColor Cyan
if (Test-Path $TARGET_PATH) {
    Write-Host "Cleaning up old version..."
    Remove-Item -Recurse -Force $TARGET_PATH
}

Expand-Archive -Path $ZIP_PATH -DestinationPath $INSTALL_DIR
# The zip contains a 'libtorch' folder, so Expand-Archive to $INSTALL_DIR is correct.

Remove-Item $ZIP_PATH
Write-Host "--- Setup Completed! ---" -ForegroundColor Green
Write-Host "You can now run 'build_torch.bat' to compile the plugin with GPU support."
