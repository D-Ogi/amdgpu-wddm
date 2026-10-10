/* hip_version.h - the HIP interface level of the BC-250 HIP runtime (M16, route B, step 3).
 *
 * Why it exists: clang's HIP runtime wrapper header (__clang_hip_runtime_wrapper.h) includes
 * "hip/hip_version.h" when it finds one, and its device malloc declaration is gated on the
 * version it reads. A ROCm installation supplies that file; we supply it ourselves, so that
 * clang's own HIP mathematics headers work against our include tree with no ROCm present.
 *
 * The number is the HIP interface level this runtime implements, not the version of any AMD
 * product, and it is stated honestly:
 *
 *   6.2.0 - above the 6.1 floor that llama.cpp's ggml-hip CMake file enforces, and the level at
 *   which this include tree is complete. We do NOT claim 6.3, because 6.3 brings hip/hip_fp8.h
 *   and the __hip_fp8_e4m3 set (ggml/src/ggml-cuda/vendors/hip.h:251), which this part has no
 *   instruction for and this tree does not carry. Stating 6.4 would make ggml-hip include a
 *   header we do not have.
 */
#ifndef BC250_HIP_VERSION_H
#define BC250_HIP_VERSION_H

#define HIP_VERSION_MAJOR 6
#define HIP_VERSION_MINOR 2
#define HIP_VERSION_PATCH 0
#define HIP_VERSION_GITHASH "bc250"
#define HIP_VERSION_BUILD_ID 0
#define HIP_VERSION_BUILD_NAME "bc250"

#define HIP_VERSION \
    (HIP_VERSION_MAJOR * 10000000 + HIP_VERSION_MINOR * 100000 + HIP_VERSION_PATCH)

#endif /* BC250_HIP_VERSION_H */
