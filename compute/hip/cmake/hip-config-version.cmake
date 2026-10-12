# hip-config-version.cmake - the version half of the BC-250 HIP CMake package.
#
# The number is read from compute\hip\include\hip\hip_version.h, so that the package, the header
# and the runtime can never state three different versions. It is the HIP interface level this
# runtime implements, not the version of an AMD product; why 6.2 and not more is written in the
# header.

get_filename_component(_bc250_hip_cmake_dir "${CMAKE_CURRENT_LIST_DIR}" REALPATH)
get_filename_component(_bc250_hip_root "${_bc250_hip_cmake_dir}/../../.." REALPATH)
set(_bc250_hip_version_header "${_bc250_hip_root}/include/hip/hip_version.h")

if (NOT EXISTS "${_bc250_hip_version_header}")
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
    set(PACKAGE_VERSION_UNSUITABLE TRUE)
    return()
endif()

file(READ "${_bc250_hip_version_header}" _bc250_hip_version_h)
string(REGEX MATCH "HIP_VERSION_MAJOR[ \t]+([0-9]+)" _m "${_bc250_hip_version_h}")
set(_bc250_major "${CMAKE_MATCH_1}")
string(REGEX MATCH "HIP_VERSION_MINOR[ \t]+([0-9]+)" _m "${_bc250_hip_version_h}")
set(_bc250_minor "${CMAKE_MATCH_1}")
string(REGEX MATCH "HIP_VERSION_PATCH[ \t]+([0-9]+)" _m "${_bc250_hip_version_h}")
set(_bc250_patch "${CMAKE_MATCH_1}")

set(PACKAGE_VERSION "${_bc250_major}.${_bc250_minor}.${_bc250_patch}")

if (PACKAGE_FIND_VERSION STREQUAL "")
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
    set(PACKAGE_VERSION_EXACT TRUE)
elseif (PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
    if (PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)
        set(PACKAGE_VERSION_EXACT TRUE)
    endif()
endif()
