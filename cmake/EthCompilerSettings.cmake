# Drop-in replacement: toolchain flags setup (no generator-exprs; CUDA-safe)

include(EthCheckCXXFlags)

# --- Defensive scrub: remove MSVC flags that may have leaked into CUDA ---
if(CMAKE_CUDA_COMPILER)
  foreach(_v
    CMAKE_CUDA_FLAGS
    CMAKE_CUDA_FLAGS_RELEASE
    CMAKE_CUDA_FLAGS_RELWITHDEBINFO
    CMAKE_CUDA_FLAGS_MINSIZEREL
    CMAKE_CUDA_FLAGS_DEBUG
  )
    if(DEFINED ${_v})
      string(REPLACE "/MP" "" ${_v} "${${_v}}")
      string(REPLACE "/GL" "" ${_v} "${${_v}}")
      string(REPLACE "/wd4068" "" ${_v} "${${_v}}")
      string(REPLACE "/wd4267" "" ${_v} "${${_v}}")
      string(REPLACE "/wd4290" "" ${_v} "${${_v}}")
      set(${_v} "${${_v}}" CACHE STRING "" FORCE)
    endif()
  endforeach()
endif()

# ------------------------------- GNU -----------------------------------------
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wno-unknown-pragmas -Wextra -Wno-error=parentheses -pedantic")
  eth_add_cxx_compiler_flag_if_supported(-ffunction-sections)
  eth_add_cxx_compiler_flag_if_supported(-fdata-sections)
  eth_add_cxx_linker_flag_if_supported(-Wl,--gc-sections)

  # ------------------------------ Clang ----------------------------------------
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wno-unknown-pragmas -Wextra")
  eth_add_cxx_compiler_flag_if_supported(-ffunction-sections)
  eth_add_cxx_compiler_flag_if_supported(-fdata-sections)
  eth_add_cxx_linker_flag_if_supported(-Wl,--gc-sections)
  if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libstdc++ -fcolor-diagnostics -Qunused-arguments")
  endif()

  # ------------------------------ MSVC -----------------------------------------
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  # Windows Vista+ requirement; avoid min/max macro conflicts; silence CRT warnings;
  # windows.h without winsock.h, which Boost.Asio can't follow
  add_definitions(/D_WIN32_WINNT=0x0600 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN)

  # IMPORTANT: Use language-specific vars so CUDA (CMAKE_CUDA_FLAGS) stays untouched.
  # C flags (no C++-only warnings here)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MP /GL")

  # C++ flags
  # /MP: parallel build
  # /EHsc: C++ EH model
  # /GL: LTCG (do not let this leak into CUDA)
  # /wdXXXX: silence specific warnings
  # /utf-8: the sources and the log's glyphs are UTF-8
  # /bigobj: Boost.Beast's templates exceed the default number of sections
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP /EHsc /GL /utf-8 /bigobj /wd4068 /wd4267 /wd4290")

  # Link-time codegen and linker opts
  set(CMAKE_STATIC_LINKER_FLAGS "${CMAKE_STATIC_LINKER_FLAGS} /LTCG")
  set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} /LTCG /OPT:REF /OPT:ICF /RELEASE")

else ()
  message(WARNING "Your compiler is not tested; if you hit issues, PRs are welcome.")
endif ()

# ----------------------------- Sanitizers (GCC/Clang) ------------------------
set(SANITIZE NO CACHE STRING "Instrument build with provided sanitizer (e.g., address, undefined)")
if(SANITIZE AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(CMAKE_CXX_FLAGS        "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer -fsanitize=${SANITIZE}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=${SANITIZE}")
endif()
