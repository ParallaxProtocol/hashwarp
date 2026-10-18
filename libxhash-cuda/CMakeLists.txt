# libxhash-cuda/CMakeLists.txt
cmake_minimum_required(VERSION 3.21)

# CUDA architectures to build, in order of precedence:
#   -DXHASH_CUDA_ARCHS="..."  or the XHASH_CUDA_ARCHS environment variable
#   -DCMAKE_CUDA_ARCHITECTURES="..."
#   a default derived from the CUDA toolkit version (see below)
# e.g.
#   cmake -S . -B build \
#     -DCMAKE_CUDA_COMPILER=/usr/local/cuda-12.9/bin/nvcc \
#     -DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++-12 \
#     -DXHASH_CUDA_ARCHS="61-real;75-real;86-real;89-real;120-real;120-virtual"
#   cmake --build build --config Release --target package -j8
#
# project() initializes CMAKE_CUDA_ARCHITECTURES to the compiler default,
# so remember whether the user set it before calling it.
if(DEFINED CMAKE_CUDA_ARCHITECTURES AND NOT CMAKE_CUDA_ARCHITECTURES STREQUAL "")
  set(_xhash_user_archs TRUE)
endif()

project(xhash_cuda LANGUAGES CXX CUDA)

find_package(CUDAToolkit REQUIRED)

set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

if(DEFINED XHASH_CUDA_ARCHS AND NOT XHASH_CUDA_ARCHS STREQUAL "")
  set(_xhash_archs "${XHASH_CUDA_ARCHS}")
elseif(DEFINED ENV{XHASH_CUDA_ARCHS} AND NOT "$ENV{XHASH_CUDA_ARCHS}" STREQUAL "")
  set(_xhash_archs "$ENV{XHASH_CUDA_ARCHS}")
elseif(_xhash_user_archs)
  set(_xhash_archs "${CMAKE_CUDA_ARCHITECTURES}")
else()
  # Native code (-real) for every supported generation plus PTX (-virtual)
  # of the newest one so future GPUs can still JIT the kernel.
  #   60/61 Pascal  70 Volta (V100, CMP 100-210)  75 Turing  80/86 Ampere
  #   89 Ada  90 Hopper  100 Blackwell DC  120 Blackwell (RTX 50)
  if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0)
    # CUDA 13 dropped Maxwell, Pascal and Volta
    set(_xhash_archs 75-real 80-real 86-real 89-real 90-real 100-real 120-real 120-virtual)
  elseif(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.8)
    set(_xhash_archs 60-real 61-real 70-real 75-real 80-real 86-real 89-real 90-real 100-real
      120-real 120-virtual)
  else()
    set(_xhash_archs 60-real 61-real 70-real 75-real 80-real 86-real 89-real 90-real 90-virtual)
  endif()
endif()

file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.cu")
file(GLOB headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.h"   "${CMAKE_CURRENT_SOURCE_DIR}/*.cuh")

add_library(xhash-cuda STATIC ${sources} ${headers})

# --- Always prefer native CUDA_ARCHITECTURES path (CMake >= 3.18) ------------
if(CMAKE_VERSION VERSION_LESS 3.18)
  message(FATAL_ERROR "CMake >= 3.18 required for reliable CUDA arch handling.")
endif()
set_property(TARGET xhash-cuda PROPERTY CUDA_ARCHITECTURES "${_xhash_archs}")
message(STATUS "xhash-cuda: CUDA ${CMAKE_CUDA_COMPILER_VERSION}, CUDA_ARCHITECTURES=${_xhash_archs}")

# Base CUDA compile options
set(_cuda_base_opts "--ptxas-options=-v" "-use_fast_math")
if(NOT MSVC)
  list(APPEND _cuda_base_opts "--disable-warnings")
endif()

target_compile_options(xhash-cuda PRIVATE
  $<$<COMPILE_LANGUAGE:CUDA>:${_cuda_base_opts}>
  $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Release>>:-O3>
  $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Debug>>:-G>
)

# Host flags for CUDA translation units (wrapped correctly)
if(MSVC)
  target_compile_options(xhash-cuda PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/EHsc>
    $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Release>>:-Xcompiler=/O2>
    $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Debug>>:-Xcompiler=/Zi>
    $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/MD>
  )
endif()

target_include_directories(xhash-cuda
  PRIVATE
    ..
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CUDAToolkit_INCLUDE_DIRS}
)

target_link_libraries(xhash-cuda
  PRIVATE
    parallaxcore
    xhash::xhash
    Boost::thread
    CUDA::cudart
)

set_target_properties(xhash-cuda PROPERTIES POSITION_INDEPENDENT_CODE ON)
