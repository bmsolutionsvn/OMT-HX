cmake_minimum_required(VERSION 3.28)
project(omthx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(OMTHX_BUILD_TESTS "Build the omthx tests and tools" ${PROJECT_IS_TOP_LEVEL})
option(OMTHX_SHARED "Also build omthx.dll, the binary release; the tools then use it" OFF)
include(cmake/omt.cmake)  # libomt and libvmx, built from the omt/ submodules

find_package(FFMPEG REQUIRED)

function(omthx_warnings target)
  target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endfunction()

# ---- library ----
set(OMTHX_SOURCES src/pure.cpp src/encoder.cpp src/decoder.cpp src/link.cpp)
function(omthx_library target type)
  add_library(${target} ${type} ${OMTHX_SOURCES})
  target_include_directories(${target} PUBLIC include PRIVATE src ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(${target} PUBLIC ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(${target} PUBLIC ${FFMPEG_LIBRARIES} libomt)
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
  target_link_libraries(omthx-send PRIVATE winmm)
endif()
