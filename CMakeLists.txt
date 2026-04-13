cmake_minimum_required(VERSION 3.18)
project(plugin_torch)

set(CMAKE_CXX_STANDARD 17)
add_compile_definitions(NOMINMAX)

set(PYTHON_TORCH_PATH "C:/Users/Owner/AppData/Local/Programs/Python/Python310/lib/site-packages/torch")
set(LIB_DIR "${PYTHON_TORCH_PATH}/lib")

include_directories(
    "${PYTHON_TORCH_PATH}/include"
    "${PYTHON_TORCH_PATH}/include/torch/csrc/api/include"
    "src"
)

add_library(plugin_torch SHARED examples/tensor/plugin_torch.cpp)

# Link directly to the known-good Python torch libs
target_link_libraries(plugin_torch 
    "${LIB_DIR}/c10.lib"
    "${LIB_DIR}/c10_cuda.lib"
    "${LIB_DIR}/torch.lib"
    "${LIB_DIR}/torch_cpu.lib"
    "${LIB_DIR}/torch_cuda.lib"
)

if (MSVC)
    target_compile_options(plugin_torch PRIVATE /Zc:__cplusplus)
    # Remove problematic magic flag and try standard linking
endif()
