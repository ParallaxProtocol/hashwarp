add_executable(hashwarp-unittests
  unittests/test_console.cpp
  unittests/test_difficulty.cpp
  unittests/test_epoch.cpp
  unittests/test_nonce_segments.cpp
  unittests/test_options.cpp
  unittests/test_log.cpp
  unittests/test_uri.cpp
  unittests/test_wizard.cpp
  unittests/globals.cpp
)
if(UNIX AND NOT APPLE)
  target_sources(hashwarp-unittests PRIVATE unittests/test_amdsysfs.cpp)
endif()
target_include_directories(hashwarp-unittests PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(hashwarp-unittests PRIVATE
  hashwarp-cli poolprotocols parallaxcore hwmon devcore xhash::xhash Boost::filesystem
  GTest::gtest_main)

add_test(NAME unittests COMMAND hashwarp-unittests)
