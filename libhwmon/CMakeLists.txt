set(SOURCES
    wraphelper.cpp wraphelper.h
    wrapnvml.h wrapnvml.cpp
    wrapadl.h wrapadl.cpp
    wrapamdsysfs.h wrapamdsysfs.cpp
    GpuControl.h GpuControl.cpp
)

add_library(hwmon ${SOURCES})
set(HWMON_LINK_LIBRARIES devcore)

if (UNIX)
  list(APPEND HWMON_LINK_LIBRARIES dl)
endif ()

target_link_libraries(hwmon ${HWMON_LINK_LIBRARIES} Boost::filesystem)
target_include_directories(hwmon PRIVATE ..)

if (XHASHCUDA)
  find_package(CUDAToolkit REQUIRED)
  target_include_directories(hwmon PUBLIC ${CUDA_INCLUDE_DIRS})
endif()
