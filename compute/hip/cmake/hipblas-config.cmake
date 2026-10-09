# hipblas-config.cmake - the CMake package of the BC-250 hipBLAS interface (M16, route B, step 3).
#
# Why it exists: llama.cpp's ggml-hip backend calls find_package(hipblas REQUIRED) and
# find_package(rocblas REQUIRED) (ggml/src/ggml-hip/CMakeLists.txt:47-48) and includes
# <hipblas/hipblas.h> unconditionally. There is no option that turns either off, so a build
# without hipBLAS needs either a patch to that file or a package that answers for it. We answer
# for it, because the alternative is a patch to four source files of an upstream project we do
# not own; the reasoning and the measurement are in compute\hipblas\README.md.
#
# What it promises: the include tree that holds hipblas\hipblas.h, and the import library of
# bc250hipblas.dll when one has been built. The GEMM entry points ggml-cuda still reaches are
# listed in the header; this package does not claim a BLAS of ROCm's quality or coverage.

if (TARGET roc::hipblas)
    return()
endif()

get_filename_component(_bc250_hipblas_cmake_dir "${CMAKE_CURRENT_LIST_DIR}" REALPATH)
get_filename_component(_bc250_hipblas_root "${_bc250_hipblas_cmake_dir}/../../.." REALPATH)

set(_bc250_hipblas_include "${_bc250_hipblas_root}/include")
if (NOT EXISTS "${_bc250_hipblas_include}/hipblas/hipblas.h")
    message(FATAL_ERROR
        "the BC-250 hipBLAS package at ${_bc250_hipblas_root} has no include/hipblas/hipblas.h; "
        "assemble the root with compute/hip/tools/make-rocm-root.py")
endif()

set(hipblas_VERSION       "2.2.0")
set(hipblas_VERSION_MAJOR "2")
set(hipblas_VERSION_MINOR "2")
set(hipblas_VERSION_PATCH "0")
set(hipblas_INCLUDE_DIR   "${_bc250_hipblas_include}")
set(hipblas_INCLUDE_DIRS  "${_bc250_hipblas_include}")

find_library(hipblas_LIBRARY
    NAMES bc250hipblas hipblas
    HINTS "${_bc250_hipblas_root}/lib" "${_bc250_hipblas_root}/lib64"
    NO_DEFAULT_PATH)

add_library(roc::hipblas INTERFACE IMPORTED)
set_target_properties(roc::hipblas PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_bc250_hipblas_include}")
if (hipblas_LIBRARY)
    set_property(TARGET roc::hipblas APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "${hipblas_LIBRARY}")
else()
    message(STATUS
        "BC-250 hipBLAS: headers only, no bc250hipblas import library in ${_bc250_hipblas_root}/lib. "
        "A target that calls a GEMM entry point will not link.")
endif()

# hipBLAS also publishes the unprefixed alias, and programs use both spellings.
if (NOT TARGET hip::hipblas)
    add_library(hip::hipblas INTERFACE IMPORTED)
    set_target_properties(hip::hipblas PROPERTIES INTERFACE_LINK_LIBRARIES "roc::hipblas")
endif()

set(hipblas_LIBRARIES roc::hipblas)
set(hipblas_FOUND TRUE)
set(HIPBLAS_FOUND TRUE)

message(STATUS "BC-250 hipBLAS interface ${hipblas_VERSION} at ${_bc250_hipblas_root}")
