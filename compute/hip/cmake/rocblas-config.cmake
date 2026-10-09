# rocblas-config.cmake - the CMake package that answers find_package(rocblas) (M16, route B).
#
# Why it exists: llama.cpp's ggml-hip backend requires the package
# (ggml/src/ggml-hip/CMakeLists.txt:48) and then never includes a rocBLAS header and never calls
# a rocBLAS function. MEASURED on llama.cpp b86d2f07: the only occurrence of the name "rocblas"
# in ggml/src is a comment in ggml-cuda/mmq.cu:390. The requirement is the historical shape of
# the ROCm build, where hipBLAS sits on rocBLAS.
#
# This package therefore states the truth: rocBLAS is found, it carries no library and no header,
# and nothing links it. A target that ever does call a rocBLAS function will fail to link, which
# is the honest outcome, instead of a wrong result from a shim nobody wrote.

if (TARGET roc::rocblas)
    return()
endif()

set(rocblas_VERSION       "4.2.0")
set(rocblas_VERSION_MAJOR "4")
set(rocblas_VERSION_MINOR "2")
set(rocblas_VERSION_PATCH "0")

add_library(roc::rocblas INTERFACE IMPORTED)

set(rocblas_LIBRARIES roc::rocblas)
set(rocblas_FOUND TRUE)
set(ROCBLAS_FOUND TRUE)

message(STATUS "BC-250 rocBLAS: an empty package; ggml-hip requires it and calls nothing in it")
