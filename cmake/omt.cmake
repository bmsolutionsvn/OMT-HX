# Open Media Transport from source: the omt/ submodules, forks of openmediatransport with OMT-HX's changes.
# Defines the target libomt (link it: C API in libomt.h) and OMTHX_RUNTIME, the DLLs to put beside an executable.
# No FFmpeg here, so an app with its own FFmpeg (BreadStudio, inside OBS) can include just this file.
# Needs the .NET 8 SDK: MSVC on PATH on Windows, clang and zlib1g-dev on Linux.
include_guard(GLOBAL)

set(OMT_DIR "${CMAKE_CURRENT_LIST_DIR}/../omt")
cmake_path(NORMAL_PATH OMT_DIR)
if(NOT EXISTS "${OMT_DIR}/libomt/libomt.h")
  message(FATAL_ERROR "omt/ is empty. Run: git submodule update --init")
endif()

# libomt's .NET runtime identifier, and the native library the linker and the runtime look for.
if(WIN32)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|ARM64")
    set(OMTHX_RID win-arm64)
  else()
    set(OMTHX_RID win-x64)
  endif()
  set(OMTHX_OMT_LIB libomt.dll)
  set(OMTHX_OMT_IMPORT libomt.lib)
else()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(OMTHX_RID linux-arm64)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(OMTHX_RID linux-x64)
  else()
    message(FATAL_ERROR "unsupported CPU: ${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  set(OMTHX_OMT_LIB libomt.so)
  set(OMTHX_OMT_IMPORT libomt.so)
endif()

# ---- libvmx: the VMX codec of plain OMT, C++. Its Visual Studio project wants the Intel compiler, so the
# sources and flags follow its clang build (build/buildwinx64.cmd) instead.
set(LIBVMX_SOURCES "${OMT_DIR}/libvmx/src/vmxcodec.cpp")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  list(APPEND LIBVMX_SOURCES "${OMT_DIR}/libvmx/src/vmxcodec_arm.cpp")  # NEON is baseline on ARMv8
else()
  list(APPEND LIBVMX_SOURCES
    "${OMT_DIR}/libvmx/src/vmxcodec_x86.cpp"
    "${OMT_DIR}/libvmx/src/vmxcodec_avx2.cpp")
endif()
if(WIN32)
  list(APPEND LIBVMX_SOURCES "${OMT_DIR}/libvmx/exports.def")
endif()

add_library(libvmx SHARED ${LIBVMX_SOURCES})
set_target_properties(libvmx PROPERTIES PREFIX "" OUTPUT_NAME libvmx CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
  COMPILE_WARNING_AS_ERROR OFF)
if(MSVC)
  target_compile_definitions(libvmx PRIVATE VMXCODEC_EXPORTS _WINDOWS _USRDLL NDEBUG)
  target_compile_options(libvmx PRIVATE /O2 /W0 /WX-)  # upstream code: its warnings are not ours to fail on
  set_source_files_properties("${OMT_DIR}/libvmx/src/vmxcodec_avx2.cpp" PROPERTIES COMPILE_OPTIONS /arch:AVX2)
else()
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "libvmx needs Clang on Linux: its sources use __declspec(align(16)). Install clang and configure with -DCMAKE_CXX_COMPILER=clang++.")
  endif()
  target_compile_options(libvmx PRIVATE -O2 -w -fPIC -fdeclspec)
  # libvmx chooses the AVX2 path at run time, so only its AVX2 file may be compiled for AVX2. libvmx uses
  # LZCNT unconditionally, as upstream's own Linux build does (-mlzcnt).
  set_source_files_properties("${OMT_DIR}/libvmx/src/vmxcodec_x86.cpp" PROPERTIES COMPILE_OPTIONS "-march=x86-64-v2;-mlzcnt")
  set_source_files_properties("${OMT_DIR}/libvmx/src/vmxcodec_avx2.cpp" PROPERTIES COMPILE_OPTIONS "-march=x86-64-v3;-mlzcnt")
endif()
# omthx calls libvmx itself, for VMX alpha: vmxcodec.h, with the functions imported from libvmx.dll.
target_include_directories(libvmx INTERFACE "${OMT_DIR}/libvmx/src")
if(WIN32)
  target_compile_definitions(libvmx INTERFACE "VMX_API=__declspec(dllimport)")
endif()

# ---- libomt: libomtnet (C#) compiled by .NET Native AOT into a native library with a C API.
# dotnet writes into each submodule's bin/ and obj/, which the submodules ignore.
find_program(OMTHX_DOTNET dotnet REQUIRED)
file(GLOB_RECURSE _omt_sources CONFIGURE_DEPENDS "${OMT_DIR}/libomtnet/src/*.cs" "${OMT_DIR}/libomt/src/*.cs")
list(APPEND _omt_sources "${OMT_DIR}/libomtnet/libomtnet.csproj" "${OMT_DIR}/libomt/libomt.csproj")
set(_omt_native "${OMT_DIR}/libomt/bin/Release/net8.0/${OMTHX_RID}/native")
set(_omt_outputs "${_omt_native}/${OMTHX_OMT_LIB}")
if(WIN32)
  list(APPEND _omt_outputs "${_omt_native}/${OMTHX_OMT_IMPORT}")
  # IlcUseEnvironmentalTools: link with the MSVC on PATH; .NET 8 cannot find a newer Visual Studio by itself
  set(_ilc -p:IlcUseEnvironmentalTools=true)
endif()
add_custom_command(
  OUTPUT ${_omt_outputs}
  # libomt.csproj finds libomtnet.dll at ../libomtnet/bin/Release/netstandard2.0. Platform=AnyCPU keeps
  # that path: a developer shell or Visual Studio sets Platform=x64, which would add an x64 folder to it.
  COMMAND "${OMTHX_DOTNET}" build "${OMT_DIR}/libomtnet/libomtnet.csproj" -c Release -f netstandard2.0
          -p:Platform=AnyCPU -nologo -v q
  COMMAND "${OMTHX_DOTNET}" publish "${OMT_DIR}/libomt/libomt.csproj" -r ${OMTHX_RID} -c Release
          -p:Platform=AnyCPU ${_ilc} -nologo -v q
  # an unchanged build may leave the files as they were; mark them current so the step does not rerun
  COMMAND "${CMAKE_COMMAND}" -E touch ${_omt_outputs}
  DEPENDS ${_omt_sources}
  COMMENT "Building ${OMTHX_OMT_LIB} from omt/libomtnet and omt/libomt"
  VERBATIM
)
add_custom_target(libomt_build DEPENDS ${_omt_outputs})

add_library(libomt INTERFACE)
target_include_directories(libomt INTERFACE "${OMT_DIR}/libomt")
target_link_libraries(libomt INTERFACE "${_omt_native}/${OMTHX_OMT_IMPORT}")
add_dependencies(libomt libomt_build libvmx)  # libomt loads libvmx at run time, so build both

# Global, so it is seen from the project that add_subdirectory'd OMT-HX too.
set_property(GLOBAL PROPERTY OMTHX_RUNTIME "${_omt_native}/${OMTHX_OMT_LIB}" "$<TARGET_FILE:libvmx>")

# Copies libomt and libvmx into `dir` after `target` builds.
function(omthx_copy_runtime_to target dir)
  get_property(files GLOBAL PROPERTY OMTHX_RUNTIME)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${files} "${dir}"
    VERBATIM)
endfunction()

# Copies libomt and libvmx next to an executable that uses omthx.
function(omthx_copy_runtime target)
  omthx_copy_runtime_to(${target} "$<TARGET_FILE_DIR:${target}>")
endfunction()