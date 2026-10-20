# Open Media Transport from source: the omt/ submodules, forks of openmediatransport with OMT-HX's changes.
# Defines the target libomt (link it: C API in libomt.h) and OMTHX_RUNTIME, the DLLs to put beside an executable.
# No FFmpeg here, so an app with its own FFmpeg (BreadStudio, inside OBS) can include just this file.
# Needs the .NET 8 SDK, and MSVC on PATH (a Visual Studio developer shell, or a Visual Studio generator).
include_guard(GLOBAL)

set(OMT_DIR "${CMAKE_CURRENT_LIST_DIR}/../omt")
cmake_path(NORMAL_PATH OMT_DIR)
if(NOT EXISTS "${OMT_DIR}/libomt/libomt.h")
  message(FATAL_ERROR "omt/ is empty. Run: git submodule update --init")
endif()

# ---- libvmx: the VMX codec of plain OMT, C++. Its Visual Studio project wants the Intel compiler, so the
# sources and flags follow its clang build (build/buildwinx64.cmd) instead.
add_library(libvmx SHARED
  "${OMT_DIR}/libvmx/src/vmxcodec.cpp"
  "${OMT_DIR}/libvmx/src/vmxcodec_x86.cpp"
  "${OMT_DIR}/libvmx/src/vmxcodec_avx2.cpp"
  "${OMT_DIR}/libvmx/exports.def"
)
set_target_properties(libvmx PROPERTIES PREFIX "" OUTPUT_NAME libvmx CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
  COMPILE_WARNING_AS_ERROR OFF)
target_compile_definitions(libvmx PRIVATE VMXCODEC_EXPORTS _WINDOWS _USRDLL NDEBUG)
target_compile_options(libvmx PRIVATE /O2 /W0 /WX-)  # upstream code: its warnings are not ours to fail on
set_source_files_properties("${OMT_DIR}/libvmx/src/vmxcodec_avx2.cpp" PROPERTIES COMPILE_OPTIONS /arch:AVX2)
# omthx calls libvmx itself, for VMX alpha: vmxcodec.h, with the functions imported from libvmx.dll.
target_include_directories(libvmx INTERFACE "${OMT_DIR}/libvmx/src")
target_compile_definitions(libvmx INTERFACE "VMX_API=__declspec(dllimport)")

# ---- libomt: libomtnet (C#) compiled by .NET Native AOT into a native DLL with a C API.
# dotnet writes into each submodule's bin/ and obj/, which the submodules ignore.
find_program(OMTHX_DOTNET dotnet REQUIRED)
file(GLOB_RECURSE _omt_sources CONFIGURE_DEPENDS "${OMT_DIR}/libomtnet/src/*.cs" "${OMT_DIR}/libomt/src/*.cs")
list(APPEND _omt_sources "${OMT_DIR}/libomtnet/libomtnet.csproj" "${OMT_DIR}/libomt/libomt.csproj")
set(_omt_native "${OMT_DIR}/libomt/bin/Release/net8.0/win-x64/native")
add_custom_command(
  OUTPUT "${_omt_native}/libomt.dll" "${_omt_native}/libomt.lib"
  # libomt.csproj finds libomtnet.dll at ../libomtnet/bin/Release/netstandard2.0. Platform=AnyCPU keeps
  # that path: a developer shell or Visual Studio sets Platform=x64, which would add an x64 folder to it.
  COMMAND "${OMTHX_DOTNET}" build "${OMT_DIR}/libomtnet/libomtnet.csproj" -c Release -f netstandard2.0
          -p:Platform=AnyCPU -nologo -v q
  # IlcUseEnvironmentalTools: link with the MSVC on PATH; .NET 8 cannot find a newer Visual Studio by itself
  COMMAND "${OMTHX_DOTNET}" publish "${OMT_DIR}/libomt/libomt.csproj" -r win-x64 -c Release
          -p:Platform=AnyCPU -p:IlcUseEnvironmentalTools=true -nologo -v q
  # an unchanged build may leave the files as they were; mark them current so the step does not rerun
  COMMAND "${CMAKE_COMMAND}" -E touch "${_omt_native}/libomt.dll" "${_omt_native}/libomt.lib"
  DEPENDS ${_omt_sources}
  COMMENT "Building libomt.dll from omt/libomtnet and omt/libomt"
  VERBATIM
)
add_custom_target(libomt_build DEPENDS "${_omt_native}/libomt.dll")

add_library(libomt INTERFACE)
target_include_directories(libomt INTERFACE "${OMT_DIR}/libomt")
target_link_libraries(libomt INTERFACE "${_omt_native}/libomt.lib")
add_dependencies(libomt libomt_build libvmx)  # libomt.dll loads libvmx.dll at run time, so build both

# Global, so it is seen from the project that add_subdirectory'd OMT-HX too.
set_property(GLOBAL PROPERTY OMTHX_RUNTIME "${_omt_native}/libomt.dll" "$<TARGET_FILE:libvmx>")

# Copies libomt.dll and libvmx.dll into `dir` after `target` builds.
function(omthx_copy_runtime_to target dir)
  get_property(files GLOBAL PROPERTY OMTHX_RUNTIME)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${files} "${dir}"
    VERBATIM)
endfunction()

# Copies libomt.dll and libvmx.dll next to an executable that uses omthx.
function(omthx_copy_runtime target)
  omthx_copy_runtime_to(${target} "$<TARGET_FILE_DIR:${target}>")
endfunction()
