add_executable(hashwarp-unittests
  unittests/test_console.cpp
  unittests/test_difficulty.cpp
  unittests/test_epoch.cpp
  unittests/test_nonce_segments.cpp
  unittests/test_oc.cpp
  unittests/test_options.cpp
  unittests/test_httpapi.cpp
  unittests/test_log.cpp
  unittests/test_uri.cpp
  unittests/test_wizard.cpp
  unittests/globals.cpp
)
if(UNIX AND NOT APPLE)
  target_sources(hashwarp-unittests PRIVATE unittests/test_amdsysfs.cpp
    unittests/test_gpucontrol.cpp)
  # NVML stand-in for the overclocking tests, loaded at run time like the driver's
  add_library(fake-nvml MODULE unittests/fake_nvml.cpp)
  add_dependencies(hashwarp-unittests fake-nvml)
  target_compile_definitions(hashwarp-unittests PRIVATE
    FAKE_NVML_LIB="$<TARGET_FILE:fake-nvml>")
endif()
target_include_directories(hashwarp-unittests PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(hashwarp-unittests PRIVATE
  hashwarp-cli apicore poolprotocols parallaxcore hwmon devcore xhash::xhash Boost::filesystem
  GTest::gtest_main)

add_test(NAME unittests COMMAND hashwarp-unittests)
