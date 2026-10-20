cmake_minimum_required(VERSION 3.28)
# libvmx's sources use __declspec(align(16)): clang accepts it with -fdeclspec, GCC does not. Prefer clang++
# on Linux unless the caller chose a compiler; the linux-* presets land here.
if(NOT WIN32 AND NOT DEFINED CMAKE_CXX_COMPILER)
  find_program(OMTHX_CLANGXX clang++)
  if(OMTHX_CLANGXX)
    set(CMAKE_CXX_COMPILER "${OMTHX_CLANGXX}")
  endif()
endif()
project(omthx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# No C++20 modules; keeps Ninja from requiring clang-scan-deps on Linux.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(OMTHX_BUILD_TESTS "Build the omthx tests and tools" ${PROJECT_IS_TOP_LEVEL})
option(OMTHX_SHARED "Also build omthx.dll/omthx.so, the binary release; the tools then use it" OFF)
include(cmake/omt.cmake)  # libomt and libvmx, built from the omt/ submodules

# FFmpeg: vcpkg's package on Windows, the distribution's on Linux, both by pkg-config-compatible variables.
if(WIN32)
  find_package(FFMPEG REQUIRED)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale libswresample)
  # Binaries find libomt.so and libvmx.so beside themselves, from any working directory.
  set(CMAKE_BUILD_RPATH "$ORIGIN")
  set(CMAKE_INSTALL_RPATH "$ORIGIN")
  # libstdc++'s parallel algorithms run serially without TBB; pure.cpp uses them.
  find_package(TBB QUIET)
endif()

function(omthx_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

# ---- library ----
set(OMTHX_SOURCES src/pure.cpp src/alpha.cpp src/encoder.cpp src/decoder.cpp src/link.cpp src/capi.cpp)
function(omthx_library target type)
  add_library(${target} ${type} ${OMTHX_SOURCES})
  target_include_directories(${target} PUBLIC include PRIVATE src ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(${target} PUBLIC ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(${target} PUBLIC ${FFMPEG_LIBRARIES} libomt PRIVATE libvmx)
  if(TBB_FOUND)
    target_link_libraries(${target} PRIVATE TBB::tbb)
  endif()
  omthx_warnings(${target})
endfunction()

# Static, for apps that build omthx in (BreadCamera) and for the tests, which reach past the public API.
omthx_library(omthx STATIC)
target_compile_definitions(omthx PUBLIC OMTHX_STATIC)

# omthx.dll and its import library omthx.lib: what the binary release ships beside omthx.h.
if(OMTHX_SHARED)
  set_target_properties(omthx PROPERTIES OUTPUT_NAME omthx_static)  # frees the name omthx.lib
  omthx_library(omthx_dll SHARED)
  set_target_properties(omthx_dll PROPERTIES OUTPUT_NAME omthx)
  target_compile_definitions(omthx_dll PRIVATE OMTHX_BUILD_DLL)
  set(OMTHX_TOOL_LIB omthx_dll)
else()
  set(OMTHX_TOOL_LIB omthx)
endif()

if(OMTHX_BUILD_TESTS)
  find_package(doctest CONFIG REQUIRED)

  # ---- tests ----
  add_executable(omthx_tests
    tests/main.cpp
    tests/libomt_test.cpp
    tests/pure_test.cpp
    tests/codec_test.cpp
    tests/link_test.cpp
    tests/capi_test.cpp
  )
  target_include_directories(omthx_tests PRIVATE src tools)
  target_link_libraries(omthx_tests PRIVATE omthx doctest::doctest)
  omthx_warnings(omthx_tests)
  omthx_copy_runtime(omthx_tests)

  # ---- tools ----
  foreach(tool send recv)
    add_executable(omthx-${tool} tools/omthx_${tool}.cpp)
    target_link_libraries(omthx-${tool} PRIVATE ${OMTHX_TOOL_LIB})
    omthx_warnings(omthx-${tool})
    omthx_copy_runtime(omthx-${tool})
  endforeach()
  if(WIN32)
    target_link_libraries(omthx-send PRIVATE winmm)
  endif()
endif()