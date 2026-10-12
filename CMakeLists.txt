cmake_minimum_required(VERSION 3.28)
project(omthx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(OMTHX_BUILD_TESTS "Build the omthx tests and tools" ${PROJECT_IS_TOP_LEVEL})
set(OMTHX_LIBOMT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libomt" CACHE PATH "Folder holding the patched libomt and libvmx")
if(NOT EXISTS "${OMTHX_LIBOMT_DIR}/libomt.lib")
  message(FATAL_ERROR "No libomt in ${OMTHX_LIBOMT_DIR}. Run scripts/build-libomt.ps1 first.")
endif()

find_package(FFMPEG REQUIRED)

function(omthx_warnings target)
  target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endfunction()

# Copies libomt.dll and libvmx.dll next to an executable that uses omthx.
# A custom target, not a post-build step, so a rebuilt libomt.dll is copied even when the executable is up to date.
function(omthx_copy_runtime target)
  add_custom_target(${target}_omt_runtime ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${OMTHX_LIBOMT_DIR}/libomt.dll" "${OMTHX_LIBOMT_DIR}/libvmx.dll" "$<TARGET_FILE_DIR:${target}>")
  add_dependencies(${target}_omt_runtime ${target})
endfunction()

# ---- library ----
add_library(omthx STATIC
  src/pure.cpp
  src/encoder.cpp
  src/decoder.cpp
  src/link.cpp
)
target_include_directories(omthx PUBLIC include PRIVATE src ${FFMPEG_INCLUDE_DIRS} "${OMTHX_LIBOMT_DIR}")
target_link_directories(omthx PUBLIC ${FFMPEG_LIBRARY_DIRS} "${OMTHX_LIBOMT_DIR}")
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
  target_include_directories(omthx_tests PRIVATE src tools "${OMTHX_LIBOMT_DIR}")
  target_link_libraries(omthx_tests PRIVATE omthx doctest::doctest)
  omthx_warnings(omthx_tests)
  omthx_copy_runtime(omthx_tests)

  # ---- tools ----
  # (added in Task 6)
endif()
