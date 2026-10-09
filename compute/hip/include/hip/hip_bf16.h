// hip_bf16.h - the bfloat16 type of the BC-250 HIP runtime (M16, route B, step 3).
//
// Why it exists: ggml-hip includes this header unconditionally (ggml/src/ggml-cuda/vendors/hip.h)
// and names `__hip_bfloat16` and `__hip_bfloat162` as the HIP spelling of CUDA's `nv_bfloat16`.
//
// gfx1013 has no bfloat16 instruction. Every operation below is therefore a conversion to float,
// the arithmetic in float, and a conversion back, which is what a software bfloat16 is. The
// storage is the 16 high bits of the float, which is the definition of the format. Rounding on
// the way down is round-to-nearest-even, the rounding the format's users expect; a plain
// truncation would bias every value towards zero.

#ifndef BC250_HIP_BF16_H
#define BC250_HIP_BF16_H

#include <hip/hip_runtime.h>
#include <hip/hip_vector_types.h>

#include <stdint.h>

#if !defined(__cplusplus)
#error "hip_bf16.h is a C++ header"
#endif

#define BC250_BF16_HD __host__ __device__ __forceinline__

struct __hip_bfloat16_raw {
  unsigned short x;
};

BC250_BF16_HD float __bc250_bf16_to_float(const unsigned short v) {
  union {
    unsigned int u;
    float f;
  } c;
  c.u = ((unsigned int)v) << 16;
  return c.f;
}

BC250_BF16_HD unsigned short __bc250_float_to_bf16(const float f) {
  union {
    float f;
    unsigned int u;
  } c;
  c.f = f;
  // A NaN keeps its payload's top bits and stays a NaN.
  if ((c.u & 0x7fffffffu) > 0x7f800000u) {
    return (unsigned short)((c.u >> 16) | 0x0040u);
  }
  const unsigned int lsb = (c.u >> 16) & 1u;
  const unsigned int round = 0x7fffu + lsb;
  return (unsigned short)((c.u + round) >> 16);
}

struct __hip_bfloat16 {
  unsigned short data;

  __hip_bfloat16() = default;
  __hip_bfloat16(const __hip_bfloat16 &) = default;
  __hip_bfloat16 &operator=(const __hip_bfloat16 &) = default;

  BC250_BF16_HD __hip_bfloat16(float v) : data(__bc250_float_to_bf16(v)) {}
  BC250_BF16_HD __hip_bfloat16(double v) : data(__bc250_float_to_bf16((float)v)) {}
  BC250_BF16_HD __hip_bfloat16(const __hip_bfloat16_raw &r) : data(r.x) {}

  BC250_BF16_HD operator float() const { return __bc250_bf16_to_float(data); }
  BC250_BF16_HD operator __hip_bfloat16_raw() const {
    __hip_bfloat16_raw r;
    r.x = data;
    return r;
  }
};

struct __hip_bfloat162_raw {
  unsigned short x, y;
};

struct __hip_bfloat162 {
  __hip_bfloat16 x, y;

  __hip_bfloat162() = default;
  __hip_bfloat162(const __hip_bfloat162 &) = default;
  __hip_bfloat162 &operator=(const __hip_bfloat162 &) = default;

  BC250_BF16_HD __hip_bfloat162(const __hip_bfloat16 lo, const __hip_bfloat16 hi)
      : x(lo), y(hi) {}
  BC250_BF16_HD __hip_bfloat162(const __hip_bfloat162_raw &r) {
    x.data = r.x;
    y.data = r.y;
  }

  BC250_BF16_HD operator __hip_bfloat162_raw() const {
    __hip_bfloat162_raw r;
    r.x = x.data;
    r.y = y.data;
    return r;
  }
};

typedef __hip_bfloat16 hip_bfloat16;

// ---------------------------------------------------------------------------
// Conversion
// ---------------------------------------------------------------------------

BC250_BF16_HD __hip_bfloat16 __float2bfloat16(const float v) { return __hip_bfloat16(v); }
BC250_BF16_HD __hip_bfloat16 __double2bfloat16(const double v) { return __hip_bfloat16(v); }
BC250_BF16_HD float __bfloat162float(const __hip_bfloat16 a) {
  return __bc250_bf16_to_float(a.data);
}
BC250_BF16_HD __hip_bfloat16 __low2bfloat16(const __hip_bfloat162 a) { return a.x; }
BC250_BF16_HD __hip_bfloat16 __high2bfloat16(const __hip_bfloat162 a) { return a.y; }
BC250_BF16_HD float __low2float(const __hip_bfloat162 a) { return __bfloat162float(a.x); }
BC250_BF16_HD float __high2float(const __hip_bfloat162 a) { return __bfloat162float(a.y); }

BC250_BF16_HD __hip_bfloat162 __bfloat162bfloat162(const __hip_bfloat16 a) {
  return __hip_bfloat162(a, a);
}
BC250_BF16_HD __hip_bfloat162 __halves2bfloat162(const __hip_bfloat16 lo,
                                                 const __hip_bfloat16 hi) {
  return __hip_bfloat162(lo, hi);
}
BC250_BF16_HD float2 __bfloat1622float2(const __hip_bfloat162 a) {
  return make_float2(__bfloat162float(a.x), __bfloat162float(a.y));
}
BC250_BF16_HD __hip_bfloat162 __float22bfloat162_rn(const float2 a) {
  return __hip_bfloat162(__hip_bfloat16(a.x), __hip_bfloat16(a.y));
}
BC250_BF16_HD __hip_bfloat162 __floats2bfloat162_rn(const float a, const float b) {
  return __hip_bfloat162(__hip_bfloat16(a), __hip_bfloat16(b));
}

// ---------------------------------------------------------------------------
// Arithmetic, in float, because this part has no bfloat16 instruction
// ---------------------------------------------------------------------------

BC250_BF16_HD __hip_bfloat16 __hadd(const __hip_bfloat16 a, const __hip_bfloat16 b) {
  return __hip_bfloat16((float)a + (float)b);
}
BC250_BF16_HD __hip_bfloat16 __hsub(const __hip_bfloat16 a, const __hip_bfloat16 b) {
  return __hip_bfloat16((float)a - (float)b);
}
BC250_BF16_HD __hip_bfloat16 __hmul(const __hip_bfloat16 a, const __hip_bfloat16 b) {
  return __hip_bfloat16((float)a * (float)b);
}
BC250_BF16_HD __hip_bfloat16 __hdiv(const __hip_bfloat16 a, const __hip_bfloat16 b) {
  return __hip_bfloat16((float)a / (float)b);
}
BC250_BF16_HD __hip_bfloat16 __hfma(const __hip_bfloat16 a, const __hip_bfloat16 b,
                                    const __hip_bfloat16 c) {
  return __hip_bfloat16(__builtin_fmaf((float)a, (float)b, (float)c));
}
BC250_BF16_HD __hip_bfloat16 __hneg(const __hip_bfloat16 a) { return __hip_bfloat16(-(float)a); }

BC250_BF16_HD __hip_bfloat162 __hadd2(const __hip_bfloat162 a, const __hip_bfloat162 b) {
  return __hip_bfloat162(__hadd(a.x, b.x), __hadd(a.y, b.y));
}
BC250_BF16_HD __hip_bfloat162 __hsub2(const __hip_bfloat162 a, const __hip_bfloat162 b) {
  return __hip_bfloat162(__hsub(a.x, b.x), __hsub(a.y, b.y));
}
BC250_BF16_HD __hip_bfloat162 __hmul2(const __hip_bfloat162 a, const __hip_bfloat162 b) {
  return __hip_bfloat162(__hmul(a.x, b.x), __hmul(a.y, b.y));
}
BC250_BF16_HD __hip_bfloat162 __hfma2(const __hip_bfloat162 a, const __hip_bfloat162 b,
                                      const __hip_bfloat162 c) {
  return __hip_bfloat162(__hfma(a.x, b.x, c.x), __hfma(a.y, b.y, c.y));
}

BC250_BF16_HD __hip_bfloat16 operator+(const __hip_bfloat16 a, const __hip_bfloat16 b) {
  return __hadd(a, b);
}
BC250_BF16_HD __hip_bfloat16 operator-(const __hip_bfloat16 a, const __hip_bfloat16 b) {
  return __hsub(a, b);
}
BC250_BF16_HD __hip_bfloat16 operator*(const __hip_bfloat16 a, const __hip_bfloat16 b) {
  return __hmul(a, b);
}
BC250_BF16_HD __hip_bfloat16 operator/(const __hip_bfloat16 a, const __hip_bfloat16 b) {
  return __hdiv(a, b);
}
BC250_BF16_HD __hip_bfloat16 operator-(const __hip_bfloat16 a) { return __hneg(a); }
BC250_BF16_HD __hip_bfloat16 &operator+=(__hip_bfloat16 &a, const __hip_bfloat16 b) {
  a = __hadd(a, b);
  return a;
}

BC250_BF16_HD bool operator==(const __hip_bfloat16 a, const __hip_bfloat16 b) {
  return (float)a == (float)b;
}
BC250_BF16_HD bool operator!=(const __hip_bfloat16 a, const __hip_bfloat16 b) {
  return (float)a != (float)b;
}
BC250_BF16_HD bool operator<(const __hip_bfloat16 a, const __hip_bfloat16 b) {
  return (float)a < (float)b;
}
BC250_BF16_HD bool operator>(const __hip_bfloat16 a, const __hip_bfloat16 b) {
  return (float)a > (float)b;
}

#endif  // BC250_HIP_BF16_H
