#include <torch/torch.h>
#include <iostream>

int main() {
    std::cout << "LibTorch GPU Diagnostic Tool" << std::endl;
    bool avail = torch::cuda::is_available();
    std::cout << "CUDA Available: " << (avail ? "YES" : "NO") << std::endl;
    if (avail) {
        std::cout << "CUDA Device Count: " << torch::cuda::device_count() << std::endl;
    }
    return 0;
}
