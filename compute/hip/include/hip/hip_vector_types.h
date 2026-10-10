// hip_vector_types.h - the short vector types of the BC-250 HIP runtime (M16, route B, step 3).
//
// Why it exists: clang supplies HIP mathematics, but no HIP type. A HIP program expects the
// CUDA-shaped short vector types (float4, int2, uchar4 ...) and the make_* constructors. This
// header defines them for both compilation passes, and for MSVC as well, because amdhip64.dll
// itself is built with MSVC and includes hip_runtime.h.
//
// Written here, not imported: this workspace holds no ROCm source tree, and the contract of
// these types is small enough to state exactly. Each type is a plain structure of N components
// with the alignment CUDA gives it, so that a structure laid out by the host compiler and read
// by the device pass agrees field by field. The alignment rule CUDA states, and which this
// header follows: a 1- or 3-component vector has the alignment of its component; a 2-component
// vector has twice it; a 4-component vector has four times it, capped at 16 bytes. The numbers
// are written out per type instead of being computed from sizeof, because MSVC wants a plain
// integer in __declspec(align).
//
// Arithmetic: CUDA gives these types no operators, and ggml-cuda uses none on them (MEASURED on
// llama.cpp b86d2f07: no operator over a short vector type in ggml/src/ggml-cuda). This header
// therefore stays a plain aggregate, which keeps it usable from C as well.

#ifndef BC250_HIP_VECTOR_TYPES_H
#define BC250_HIP_VECTOR_TYPES_H

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__clang__)
#define BC250_VEC_ALIGN(n) __declspec(align(n))
#else
#define BC250_VEC_ALIGN(n) __attribute__((aligned(n)))
#endif

#if defined(__HIP__)
#define BC250_VEC_HD __attribute__((host)) __attribute__((device))
#else
#define BC250_VEC_HD
#endif

#if defined(__cplusplus)
#define BC250_VEC_FN inline BC250_VEC_HD
#else
#define BC250_VEC_FN static BC250_VEC_HD
#endif

// base: the CUDA name stem. T: the component type. A2, A4: the alignment of the 2- and
// 4-component forms.
#define BC250_VEC_DEFINE(base, T, A2, A4)                                       \
  typedef struct base##1 {                                                      \
    T x;                                                                        \
  } base##1;                                                                    \
  typedef struct BC250_VEC_ALIGN(A2) base##2 {                                   \
    T x, y;                                                                     \
  } base##2;                                                                    \
  typedef struct base##3 {                                                      \
    T x, y, z;                                                                  \
  } base##3;                                                                    \
  typedef struct BC250_VEC_ALIGN(A4) base##4 {                                   \
    T x, y, z, w;                                                               \
  } base##4;                                                                    \
  BC250_VEC_FN base##1 make_##base##1(T x) {                                     \
    base##1 r;                                                                  \
    r.x = x;                                                                    \
    return r;                                                                   \
  }                                                                              \
  BC250_VEC_FN base##2 make_##base##2(T x, T y) {                                \
    base##2 r;                                                                  \
    r.x = x;                                                                    \
    r.y = y;                                                                    \
    return r;                                                                   \
  }                                                                              \
  BC250_VEC_FN base##3 make_##base##3(T x, T y, T z) {                           \
    base##3 r;                                                                  \
    r.x = x;                                                                    \
    r.y = y;                                                                    \
    r.z = z;                                                                    \
    return r;                                                                   \
  }                                                                              \
  BC250_VEC_FN base##4 make_##base##4(T x, T y, T z, T w) {                      \
    base##4 r;                                                                  \
    r.x = x;                                                                    \
    r.y = y;                                                                    \
    r.z = z;                                                                    \
    r.w = w;                                                                    \
    return r;                                                                   \
  }

BC250_VEC_DEFINE(char, signed char, 2, 4)
BC250_VEC_DEFINE(uchar, unsigned char, 2, 4)
BC250_VEC_DEFINE(short, short, 4, 8)
BC250_VEC_DEFINE(ushort, unsigned short, 4, 8)
BC250_VEC_DEFINE(int, int, 8, 16)
BC250_VEC_DEFINE(uint, unsigned int, 8, 16)
// `long1` to `long4` and the unsigned set are deliberately absent. `long` is 4 bytes to MSVC,
// which builds amdhip64.dll and the host side of every HIP program on Windows, and 8 bytes to
// the device pass, which targets amdgcn-amd-amdhsa. A `long2` could therefore never have one
// layout that both passes agree on, and a type that silently changes size between the two sides
// of the same program is worse than a type that does not exist. ggml-cuda uses none of them
// (MEASURED, llama.cpp b86d2f07). A program that wants 64 bits uses `longlong2`.
BC250_VEC_DEFINE(longlong, long long, 16, 16)
BC250_VEC_DEFINE(ulonglong, unsigned long long, 16, 16)
BC250_VEC_DEFINE(float, float, 8, 16)
BC250_VEC_DEFINE(double, double, 16, 16)

#undef BC250_VEC_DEFINE

#endif  // BC250_HIP_VECTOR_TYPES_H
