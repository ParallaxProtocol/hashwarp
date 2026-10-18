add_executable(hashwarp-unittests
  unittests/test_difficulty.cpp
  unittests/test_epoch.cpp
  unittests/test_nonce_segments.cpp
  unittests/test_options.cpp
  unittests/test_log.cpp
  unittests/test_uri.cpp
  unittests/globals.cpp
)
target_include_directories(hashwarp-unittests PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(hashwarp-unittests PRIVATE
  hashwarp-options poolprotocols parallaxcore devcore xhash::xhash Boost::filesystem
  GTest::gtest_main)

add_test(NAME unittests COMMAND hashwarp-unittests)
