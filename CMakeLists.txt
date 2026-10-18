# Fix for CMake compatibility error with versions < 3.5
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
cmake_minimum_required(VERSION 3.5...4.1)

add_compile_definitions(BOOST_ALL_NO_LIB=1)

include(cmake/cable/bootstrap.cmake)

include(CableBuildInfo)
include(CableBuildType)
include(CableToolchains)
include(HunterGate)

include(defaults/HunterCacheServers)

cable_configure_toolchain(DEFAULT cxx11)

set(HUNTER_CONFIGURATION_TYPES Release CACHE STRING "Build type of Hunter packages")
set(HUNTER_JOBS_NUMBER 6 CACHE STRING "Number of parallel builds used by Hunter")
HunterGate(
    URL "https://github.com/cpp-pm/hunter/archive/v0.26.6.tar.gz"
    SHA1 "e70c29f878f5d5f5cdf1b9ccd628fb872e8624a8"
    LOCAL
)

project(hashwarp)
set(PROJECT_VERSION 1.2.0)

# The cable toolchain sets C++11 for Hunter packages; hashwarp itself is C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

cable_set_build_type(DEFAULT Release CONFIGURATION_TYPES Release RelWithDebInfo)

option(XHASHCL "Build with OpenCL mining" ON)
option(XHASHCUDA "Build with CUDA mining" ON)
option(XHASHCPU "Build with CPU mining (only for development)" OFF)
option(ETHDBUS "Build with D-Bus support" OFF)
option(APICORE "Build with API Server support" ON)
option(DEVBUILD "Log developer metrics" OFF)
option(USE_SYS_OPENCL "Build with system OpenCL" OFF)
option(HASHWARP_TESTS "Build unit tests" OFF)
option(HASHWARP_COMPUTE_BENCH "Development only: kernel compute benchmark, hashes are WRONG" OFF)

# propagates CMake configuration options to the compiler
function(configureProject)
  if (XHASHCL)
    add_definitions(-DETH_XHASHCL)
  endif()
  if (XHASHCUDA)
    add_definitions(-DETH_XHASHCUDA)
  endif()
  if (XHASHCPU)
    add_definitions(-DETH_XHASHCPU)
  endif()
  if (ETHDBUS)
    add_definitions(-DETH_DBUS)
  endif()
  if (APICORE)
    add_definitions(-DAPI_CORE)
  endif()
  if (DEVBUILD)
    add_definitions(-DDEV_BUILD)
  endif()
  if (USE_SYS_OPENCL)
    add_definitions(-DUSE_SYS_OPENCL)
  endif()
  if (HASHWARP_COMPUTE_BENCH)
    message(WARNING "HASHWARP_COMPUTE_BENCH: this build computes wrong hashes, never mine with it")
    add_definitions(-DXHASH_COMPUTE_BENCH)
  endif()
endfunction()

# Before any other package: find_library caches directory listings during a
# configure run, a package unpacked into Hunter's Install/lib after an earlier
# search there is not seen (OpenSSL was then not found, or the system's was used)
hunter_add_package(OpenSSL)

hunter_add_package(Boost COMPONENTS system filesystem thread)
find_package(Boost CONFIG REQUIRED COMPONENTS system filesystem thread)

hunter_add_package(jsoncpp)
find_package(jsoncpp CONFIG REQUIRED)

hunter_add_package(xhash)
find_package(xhash CONFIG REQUIRED)

if (HASHWARP_TESTS)
  hunter_add_package(GTest)
  find_package(GTest CONFIG REQUIRED)
endif()

configureProject()

if(APPLE)
  set(USE_SYS_OPENCL ON)
endif()

message("----------------------------------------------------------------------------")
message("-- CMake ${CMAKE_VERSION}")
message("-- Build ${CMAKE_BUILD_TYPE} / ${CMAKE_SYSTEM_NAME}")
message("----------------------------------------------------------------- components")
message("-- XHASHCL         Build OpenCL components                      ${XHASHCL}")
message("-- XHASHCUDA       Build CUDA components                        ${XHASHCUDA}")
message("-- XHASHCPU        Build CPU components (only for development)  ${XHASHCPU}")
message("-- ETHDBUS          Build D-Bus components                       ${ETHDBUS}")
message("-- APICORE          Build API Server components                  ${APICORE}")
message("-- DEVBUILD         Build with dev logging                       ${DEVBUILD}")
message("-- USE_SYS_OPENCL   Build with system OpenCL                     ${USE_SYS_OPENCL}")
message("-- HASHWARP_TESTS   Build unit tests                             ${HASHWARP_TESTS}")
message("----------------------------------------------------------------------------")
message("")

include(EthCompilerSettings)
if(UNIX AND NOT APPLE)
  # Only glibc stays dynamic: the GPU drivers (libcuda, NVML, OpenCL ICDs) are
  # loaded with dlopen, which needs it
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++ -static-libgcc")
endif()

cable_add_buildinfo_library(PROJECT_NAME ${PROJECT_NAME})

add_subdirectory(libdevcore)
add_subdirectory(libparallaxcore)
add_subdirectory(libhwmon)
add_subdirectory(libpoolprotocols)

if (XHASHCL)
  add_subdirectory(libxhash-cl)
endif ()
if (XHASHCUDA)
  add_subdirectory(libxhash-cuda)
endif ()
if (XHASHCPU)
  add_subdirectory(libxhash-cpu)
endif ()
if (APICORE)
  add_subdirectory(libapicore)
endif()

add_subdirectory(hashwarp)

if (HASHWARP_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()


# CPack configuration: produce platform-specific release archives named
#   - Linux:   hashwarp-v<VERSION>-linux.tar.gz
#   - Windows: hashwarp-v<VERSION>-windows.zip
if(UNIX AND NOT APPLE)
  set(CPACK_GENERATOR TGZ)
  set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-v${PROJECT_VERSION}-linux")
elseif(WIN32)
  set(CPACK_GENERATOR ZIP)
  set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-v${PROJECT_VERSION}-windows")
else()
  # Other platforms fallback
  set(CPACK_GENERATOR TGZ)
  set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-v${PROJECT_VERSION}")
endif()
# The archive holds one directory with the executable, the documentation, a
# sample configuration and start scripts
install(FILES README.md LICENSE packaging/release/hashwarp.json.example
  DESTINATION . COMPONENT release EXCLUDE_FROM_ALL)
if(WIN32)
  install(FILES packaging/release/start.bat packaging/release/start-solo.bat
    scripts/fix-opencl-icd.vbs DESTINATION . COMPONENT release EXCLUDE_FROM_ALL)
  install(FILES scripts/README.txt RENAME fix-opencl-icd.txt
    DESTINATION . COMPONENT release EXCLUDE_FROM_ALL)
  # The Visual C++ runtime DLLs next to the executable, so it also runs where
  # the redistributable is not installed
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT release)
  include(InstallRequiredSystemLibraries)
else()
  install(PROGRAMS packaging/release/start.sh packaging/release/start-solo.sh
    DESTINATION . COMPONENT release EXCLUDE_FROM_ALL)
endif()
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL release)
set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_ARCHIVE_RELEASE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}")
set(CPACK_PACKAGE_CHECKSUM SHA256)
include(CPack)

# HiveOS and mmpOS packages (Linux): cmake --build build --target rig-packages
if(UNIX AND NOT APPLE)
  add_custom_target(rig-packages
    COMMAND ${CMAKE_SOURCE_DIR}/packaging/make-rig-packages.sh $<TARGET_FILE:hashwarp>
      ${PROJECT_VERSION} ${CMAKE_BINARY_DIR}
    DEPENDS hashwarp
    COMMENT "Building the HiveOS and mmpOS packages")
endif()
