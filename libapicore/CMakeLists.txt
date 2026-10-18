set(SOURCES
    ApiServer.h ApiServer.cpp
    HttpApi.h HttpApi.cpp
)

add_library(apicore ${SOURCES})
target_link_libraries(apicore PRIVATE parallaxcore devcore hashwarp-buildinfo Boost::filesystem
  PUBLIC jsoncpp_static)
target_include_directories(apicore PRIVATE ..)
