cmake_policy(SET CMP0015 NEW)

include_directories(BEFORE ..)

set(EXECUTABLE hashwarp)

hunter_add_package(CLI11)
find_package(CLI11 CONFIG REQUIRED)

# Command line and configuration options (shared with the unit tests)
add_library(hashwarp-options STATIC Options.cpp Options.h Wizard.cpp Wizard.h)
target_include_directories(hashwarp-options PUBLIC ..)
target_link_libraries(hashwarp-options PUBLIC CLI11::CLI11 PRIVATE poolprotocols devcore
  jsoncpp_static Boost::filesystem)

add_executable(${EXECUTABLE} main.cpp DBusInt.h)
if(MSVC)
  target_sources(${EXECUTABLE} PRIVATE hashwarp.rc)
endif()

target_link_libraries(hashwarp PRIVATE hashwarp-options parallaxcore poolprotocols devcore hashwarp-buildinfo Boost::system Boost::thread)

if(ETHDBUS)
  find_package(PkgConfig)
  set( ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/usr/lib/x86_64-linux-gnu/pkgconfig" )
  pkg_check_modules(DBUS dbus-1)
  include_directories(${DBUS_INCLUDE_DIRS})
  link_directories(${DBUS_LIBRARY_DIRS})
  target_link_libraries(hashwarp PRIVATE ${DBUS_LIBRARIES})
endif()

if(APICORE)
  target_link_libraries(hashwarp PRIVATE apicore)
endif()

include(GNUInstallDirs)
install(TARGETS hashwarp DESTINATION ${CMAKE_INSTALL_BINDIR})
if(MSVC)
  install(FILES $<TARGET_PDB_FILE:hashwarp> DESTINATION ${CMAKE_INSTALL_BINDIR} OPTIONAL)
endif()
