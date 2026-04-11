cmake_minimum_required(VERSION 3.18)
project(plugin_torch)

set(CMAKE_CXX_STANDARD 17)
add_compile_definitions(NOMINMAX)

set(CMAKE_PREFIX_PATH "d:/Riz/deps/libtorch/share/cmake/Torch")
find_package(Torch REQUIRED)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${TORCH_CXX_FLAGS}")

add_library(plugin_torch SHARED examples/plugin_torch.cpp)
target_include_directories(plugin_torch PRIVATE src)

target_link_libraries(plugin_torch "${TORCH_LIBRARIES}")
set_property(TARGET plugin_torch PROPERTY CXX_STANDARD 17)

if (MSVC)
  file(TO_CMAKE_PATH "${TORCH_INSTALL_PREFIX}/lib" TORCH_LIB_PATH)
  link_directories("${TORCH_LIB_PATH}")
endif()
