# Building from source

## Table of Contents

* [Requirements](#requirements)
* [Instructions](#instructions)
* [Docker](#docker)
* [CMake configuration options](#cmake-configuration-options)
* [Tests](#tests)
* [Disable Hunter](#disable-hunter)

## Requirements

Hashwarp is written in C++17 and built with [CMake], which downloads and builds the other
dependencies (Boost, OpenSSL, jsoncpp, CLI11, the OpenCL loader) with [Hunter].

* [CMake] 3.21 or newer (CMake 4 works)
* [Git](https://git-scm.com/downloads)
* [Perl](https://www.perl.org/get.html), to build OpenSSL
* A C++17 compiler: GCC 9 or newer, Clang 10 or newer, or Visual Studio 2017 or newer
* For NVIDIA (CUDA) support, the [CUDA Toolkit](https://developer.nvidia.com/cuda-downloads):
  * 12.8 or newer to build native code for Blackwell (RTX 50) GPUs
  * CUDA 13 no longer supports Pascal and Volta GPUs: use 12.x to build for them
* On Linux, the OpenGL headers for the OpenCL loader, e.g. on Ubuntu:

    ```shell
    sudo apt install build-essential cmake git perl mesa-common-dev
    ```

  and the D-Bus development files if building with `-DETHDBUS=ON` (`libdbus-1-dev`)

## Instructions

1. Get the sources with their submodules:

    ```shell
    git clone --recursive https://github.com/microstack-tech/hashwarp.git
    cd hashwarp
    ```

    or, in an existing clone, `git submodule update --init --recursive`.

2. Configure (see the [options](#cmake-configuration-options)) and build:

    ```shell
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release -j
    ```

    The executable is `build/hashwarp/hashwarp` (`build\hashwarp\Release\hashwarp.exe` on Windows).
    The first configuration downloads and builds the dependencies, which takes a while.

    On Windows run the commands from a "x64 Native Tools Command Prompt" of Visual Studio, with
    Perl and CMake in the `PATH`.

3. Optionally:

    ```shell
    cmake --build build --target package       # release archive with docs and start scripts
    cmake --build build --target rig-packages  # HiveOS and mmpOS packages (Linux)
    sudo cmake --install build                 # install the executable (Linux)
    ```

To build for a different CUDA toolkit or host compiler than the defaults:

```shell
cmake -S . -B build -DCMAKE_CUDA_COMPILER=/usr/local/cuda-12.9/bin/nvcc \
    -DCMAKE_CUDA_HOST_COMPILER=g++-12
```

## Docker

```shell
docker build -f packaging/docker/Dockerfile -t hashwarp .
docker run --rm --gpus all hashwarp --pool pool.example.com:3333 --wallet 0xYourParallaxAddress
```

The image builds hashwarp with CUDA 12.9 (`--build-arg CUDA_VERSION=...` for another version) and
runs the unit tests. Mining on NVIDIA GPUs in a container needs the
[NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/).

## CMake configuration options

Pass these options to CMake configuration command, e.g.

```shell
cmake .. -DXHASHCUDA=ON -DXHASHCL=OFF
```

* `-DXHASHCL=ON` - enable OpenCL mining, `ON` by default.
* `-DXHASHCUDA=ON` - enable CUDA mining, `ON` by default.
* `-DAPICORE=ON` - enable API Server, `ON` by default.
* `-DETHDBUS=ON` - enable D-Bus support, `OFF` by default.
* `-DUSE_SYS_OPENCL=ON` - Use system OpenCL, `OFF` by default, unless on macOS. Specify to use local **ROCm-OpenCL** package.
* `-DHASHWARP_TESTS=ON` - build the unit tests, `OFF` by default.
* `-DXHASH_CUDA_ARCHS="..."` - CUDA architectures to build (also read from the `XHASH_CUDA_ARCHS`
  environment variable), e.g. `"86-real;89-real;120-real;120-virtual"`. By default every
  architecture supported by the CUDA toolkit from Pascal (Turing with CUDA 13) to Blackwell is built.

## Tests

Unit tests (no GPU needed):

```shell
cmake -S . -B build -DHASHWARP_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

GPU self-test: mines against a local mock node (`getwork`) or pool (`stratum`, EthereumStratum/1.0.0)
and checks every GPU result on the CPU, the epoch and, for stratum, that the share rate matches the
pool difficulty:

```shell
python3 test/gpu/selftest.py build/hashwarp/hashwarp cuda
python3 test/gpu/selftest.py build/hashwarp/hashwarp opencl --protocol stratum
```

## Disable Hunter

If you want to install dependencies yourself or use system package manager you can disable Hunter by adding
[`-DHUNTER_ENABLED=OFF`](https://docs.hunter.sh/en/latest/reference/user-variables.html#hunter-enabled)
to the configuration options.

[CMake]: https://cmake.org/
[CMake Build Tool Mode]: https://cmake.org/cmake/help/latest/manual/cmake.1.html#build-tool-mode
[Hunter]: https://docs.hunter.sh/
