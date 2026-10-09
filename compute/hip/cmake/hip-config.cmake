# hip-config.cmake - the CMake package of the BC-250 HIP runtime (M16, route B, step 3).
#
# Why it exists: a program that uses HIP finds it with find_package(hip), and llama.cpp's
# ggml-hip backend does exactly that and then requires a version of at least 6.1
# (ggml/src/ggml-hip/CMakeLists.txt:46 and :55). A ROCm installation ships this file. We have no
# ROCm installation, so we ship our own, and a build finds it through CMAKE_PREFIX_PATH or
# through ROCM_PATH, both of which ggml-hip already honours.
#
# What it promises, and nothing more: the two targets a HIP program links, hip::host and
# hip::device, over the include tree in compute\hip\include and the import library of
# amdhip64.dll. The version it states is the HIP interface level this runtime implements
# (compute\hip\include\hip\hip_version.h), not the version of any AMD product.
#
# Assemble the package root with tools\make-rocm-root.py, which puts this file where CMake looks
# and puts the headers and the device library bitcode where clang looks.

if (TARGET hip::host)
    return()
endif()

include(CMakeFindDependencyMacro)

get_filename_component(_bc250_hip_cmake_dir "${CMAKE_CURRENT_LIST_DIR}" REALPATH)
# <root>/lib/cmake/hip -> <root>
get_filename_component(_bc250_hip_root "${_bc250_hip_cmake_dir}/../../.." REALPATH)

set(_bc250_hip_include "${_bc250_hip_root}/include")
if (NOT EXISTS "${_bc250_hip_include}/hip/hip_runtime.h")
    message(FATAL_ERROR
        "the BC-250 HIP package at ${_bc250_hip_root} has no include/hip/hip_runtime.h; "
        "assemble the root with compute/hip/tools/make-rocm-root.py")
endif()

# The version the headers state. Read it from the header, so that the two can never disagree.
file(READ "${_bc250_hip_include}/hip/hip_version.h" _bc250_hip_version_h)
string(REGEX MATCH "HIP_VERSION_MAJOR[ \t]+([0-9]+)" _m "${_bc250_hip_version_h}")
set(_bc250_hip_major "${CMAKE_MATCH_1}")
string(REGEX MATCH "HIP_VERSION_MINOR[ \t]+([0-9]+)" _m "${_bc250_hip_version_h}")
set(_bc250_hip_minor "${CMAKE_MATCH_1}")
string(REGEX MATCH "HIP_VERSION_PATCH[ \t]+([0-9]+)" _m "${_bc250_hip_version_h}")
set(_bc250_hip_patch "${CMAKE_MATCH_1}")
set(hip_VERSION "${_bc250_hip_major}.${_bc250_hip_minor}.${_bc250_hip_patch}")
set(hip_VERSION_MAJOR "${_bc250_hip_major}")
set(hip_VERSION_MINOR "${_bc250_hip_minor}")
set(hip_VERSION_PATCH "${_bc250_hip_patch}")

set(hip_INCLUDE_DIR  "${_bc250_hip_include}")
set(hip_INCLUDE_DIRS "${_bc250_hip_include}")
set(hip_ROOT         "${_bc250_hip_root}")
set(HIP_ROOT_DIR     "${_bc250_hip_root}")

find_library(hip_RUNTIME_LIBRARY
    NAMES amdhip64 amdhip64.lib
    HINTS "${_bc250_hip_root}/lib" "${_bc250_hip_root}/lib64"
    NO_DEFAULT_PATH)
if (NOT hip_RUNTIME_LIBRARY)
    message(FATAL_ERROR
        "the BC-250 HIP package at ${_bc250_hip_root} has no lib/amdhip64.lib; "
        "build compute/hip with build-runtime.ps1 first")
endif()

# The target that gives the host side of a program the HIP interface and the runtime to link.
add_library(hip::host INTERFACE IMPORTED)
set_target_properties(hip::host PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_bc250_hip_include}"
    INTERFACE_COMPILE_DEFINITIONS "__HIP_PLATFORM_AMD__=1"
    INTERFACE_LINK_LIBRARIES "${hip_RUNTIME_LIBRARY}")

# The target that also compiles device code. CMake on Windows has no HIP language, so ggml-hip
# compiles its .cu files as C++ and links hip::device; the device pass therefore has to come from
# this target's compile options, as it does with hipcc.
#
# BC250_HIP_OFFLOAD_ARCH is the one knob: it is gfx1013 for this machine, and a caller that
# builds for another part sets it. -fno-gpu-rdc keeps every translation unit's device code
# self-contained, which is what our loader expects.
if (NOT DEFINED BC250_HIP_OFFLOAD_ARCH)
    set(BC250_HIP_OFFLOAD_ARCH "gfx1013" CACHE STRING "the AMDGPU target the device pass builds for")
endif()

add_library(hip::device INTERFACE IMPORTED)
set_target_properties(hip::device PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_bc250_hip_include}"
    INTERFACE_COMPILE_DEFINITIONS "__HIP_PLATFORM_AMD__=1"
    #
    # -include __clang_hip_runtime_wrapper.h is not redundant. MEASURED: clang adds that header
    # by itself only when it recognises the --rocm-path root as a HIP installation, and our root
    # is not one (it has no bin\hipconfig and no .hipVersion). Without the wrapper the device
    # pass has no __clang_hip_math.h, so every call of fabsf, expf, powf or ldexpf in a kernel
    # fails with "call to __host__ function from __device__ function" - 1431 such errors over all
    # 144 translation units of ggml-hip, which is what this one option removes.
    INTERFACE_COMPILE_OPTIONS
        "-x;hip;--offload-arch=${BC250_HIP_OFFLOAD_ARCH};--rocm-path=${_bc250_hip_root};-fno-gpu-rdc;-include;__clang_hip_runtime_wrapper.h"
    INTERFACE_LINK_LIBRARIES "hip::host")

# The names a ROCm hip-config.cmake also sets, because programs read them.
set(hip_LIBRARIES hip::host)
set(hip_HIPCC_EXECUTABLE "${CMAKE_CXX_COMPILER}")
set(hip_FOUND TRUE)
set(HIP_FOUND TRUE)

message(STATUS "BC-250 HIP ${hip_VERSION} at ${_bc250_hip_root} (device target ${BC250_HIP_OFFLOAD_ARCH})")
