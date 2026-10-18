cmake_minimum_required(VERSION 3.28)
project(omthx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(OMTHX_BUILD_TESTS "Build the omthx tests and tools" ${PROJECT_IS_TOP_LEVEL})
include(cmake/omt.cmake)  # libomt and libvmx, built from the omt/ submodules

find_package(FFMPEG REQUIRED)

function(omthx_warnings target)
  target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endfunction()

# ---- library ----
add_library(omthx STATIC
  src/pure.cpp
  src/encoder.cpp
  src/decoder.cpp
  src/link.cpp
)
target_include_directories(omthx PUBLIC include PRIVATE src ${FFMPEG_INCLUDE_DIRS})
target_link_directories(omthx PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(omthx PUBLIC ${FFMPEG_LIBRARIES} libomt)
omthx_warnings(omthx)

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
    target_link_libraries(omthx-${tool} PRIVATE omthx)
    omthx_warnings(omthx-${tool})
    omthx_copy_runtime(omthx-${tool})
  endforeach()
  target_link_libraries(omthx-send PRIVATE winmm)
endif()
