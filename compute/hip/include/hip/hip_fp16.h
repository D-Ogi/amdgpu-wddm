// hip_fp16.h - the half-precision type of the BC-250 HIP runtime (M16, route B, step 3).
//
// Why it exists: clang supplies the HIP mathematics library, but no HIP type. A HIP program
// expects `__half`, `__half2` and the CUDA-shaped conversion and arithmetic set over them, and
// llama.cpp's ggml-hip backend uses 25 of those names (MEASURED, llama.cpp b86d2f07, see
// scratch\m16-hip\step3\out\device_scan.md).
//
// Written here, not imported: this workspace holds no ROCm source tree. Every name below is the
// public CUDA/HIP name with the documented meaning; the implementation is `_Float16`, which this
// compiler has for both the host pass (x86-64) and the device pass (amdgcn gfx1013), so the
// arithmetic is the hardware's own and the packed form becomes the v_pk_*_f16 instructions.
//
// Layout contract, which matters because ggml-common.h puts `half` inside the block structures
// that the host writes and the device reads: `__half` is 2 bytes aligned 2, `__half2` is 4 bytes
// aligned 4, and both stay trivially copyable and trivially default-constructible.

#ifndef BC250_HIP_FP16_H
#define BC250_HIP_FP16_H

#include <hip/hip_runtime.h>
#include <hip/hip_vector_types.h>
#include <hip/hip_float_conversion.h>

#include <stdint.h>

#if !defined(__cplusplus)
#error "hip_fp16.h is a C++ header"
#endif

#define BC250_HALF_HD __host__ __device__ __forceinline__

// ---------------------------------------------------------------------------
// float <-> half, and why the host does it by hand
//
// The device pass has v_cvt_f16_f32 and v_cvt_f32_f16, so a cast is one instruction there. The
// host pass has neither: clang lowers a `_Float16` cast on x86-64 to the compiler-rt helpers
// __truncsfhf2 and __extendhfsf2, and MEASURED: the portable AMDGPU toolchain
// (toolchain\llvm-amdgpu-22.1.8) ships no compiler-rt builtins library for x86-64 Windows, so
// linking ggml-hip.dll stopped at "undefined symbol: __truncsfhf2". The host side therefore
// converts with integer arithmetic, which needs no library at all. The storage stays _Float16,
// so the layout and the device code are unaffected.
// ---------------------------------------------------------------------------

union __bc250_half_bits {
  unsigned short u;
  _Float16 h;
};

// The four rounding modes CUDA and HIP name in a conversion's own name. A caller that writes
// __float2half_rd asks for the value toward negative infinity and not for the nearest one, so
// each mode is implemented and none of them is a second name for round to nearest even.
#define BC250_HALF_RN 0  // to nearest, ties to even
#define BC250_HALF_RZ 1  // toward zero
#define BC250_HALF_RD 2  // toward negative infinity
#define BC250_HALF_RU 3  // toward positive infinity

// float -> binary16 in one named rounding mode, in integer arithmetic. The same code serves the
// host pass, which has no float16 conversion helper to call, and the device pass, where
// v_cvt_f16_f32 rounds to nearest even and a directed mode would otherwise need the hardware
// rounding mode changed around every conversion.
//
// Each branch computes the truncated value and whether anything was cut off (`rest`). The
// nearest-even mode compares the remainder with a half step; a directed mode raises the
// magnitude only when it rounds away from zero, which is `away` below. Raising the magnitude of
// the packed form carries into the exponent by itself, up to 0x7c00, which is infinity.
BC250_HALF_HD _Float16 __bc250_float_to_half_mode(const float f, const int mode) {
  union {
    float f;
    unsigned int u;
  } in;
  in.f = f;
  const unsigned int sign = (in.u >> 16) & 0x8000u;
  const unsigned int exp_field = (in.u >> 23) & 0xffu;
  const int exp = (int)exp_field - 127 + 15;
  const unsigned int mant = in.u & 0x007fffffu;
  const int negative = sign != 0u;
  const int away = (mode == BC250_HALF_RU && !negative) || (mode == BC250_HALF_RD && negative);
  __bc250_half_bits out;
  if (exp_field == 0xffu) {
    // Infinity keeps its sign; a NaN keeps a non-zero mantissa so that it stays a NaN. No
    // rounding mode applies to either.
    out.u = (unsigned short)(sign | 0x7c00u | (mant != 0 ? 0x0200u : 0u));
  } else if (exp >= 0x1f) {
    // Above every finite binary16 value. Only a mode that rounds away from zero reaches
    // infinity; toward zero, and toward the other infinity, the answer is the largest finite
    // value of that sign.
    out.u = (unsigned short)(sign | ((away || mode == BC250_HALF_RN) ? 0x7c00u : 0x7bffu));
  } else if (exp <= 0) {
    if (exp < -10) {
      // Below half of the smallest subnormal. Zero stays zero in every mode; a non-zero value
      // becomes the smallest subnormal when the mode rounds away from zero.
      const unsigned int nonzero = (in.u & 0x7fffffffu) != 0u;
      out.u = (unsigned short)(sign | ((nonzero && away) ? 1u : 0u));
    } else {
      // Subnormal: put the implied one back and shift.
      const unsigned int m = mant | 0x00800000u;
      const int shift = 14 - exp;
      const unsigned int value = m >> shift;
      const unsigned int rest = m & ((1u << shift) - 1u);
      const unsigned int half = 1u << (shift - 1);
      unsigned int rounded = value;
      if (mode == BC250_HALF_RN) {
        if (rest > half || (rest == half && (value & 1u) != 0u)) {
          rounded += 1u;
        }
      } else if (away && rest != 0u) {
        rounded += 1u;
      }
      out.u = (unsigned short)(sign | rounded);
    }
  } else {
    const unsigned int value = ((unsigned int)exp << 10) | (mant >> 13);
    const unsigned int rest = mant & 0x1fffu;
    unsigned int rounded = value;
    if (mode == BC250_HALF_RN) {
      if (rest > 0x1000u || (rest == 0x1000u && (value & 1u) != 0u)) {
        rounded += 1u;
      }
    } else if (away && rest != 0u) {
      rounded += 1u;
    }
    out.u = (unsigned short)(sign | rounded);
  }
  return out.h;
}

BC250_HALF_HD _Float16 __bc250_float_to_half(const float f) {
#if defined(__HIP_DEVICE_COMPILE__)
  return (_Float16)f;
#else
  return __bc250_float_to_half_mode(f, BC250_HALF_RN);
#endif
}

BC250_HALF_HD float __bc250_half_to_float(const _Float16 h) {
#if defined(__HIP_DEVICE_COMPILE__)
  return (float)h;
#else
  __bc250_half_bits in;
  in.h = h;
  const unsigned int sign = ((unsigned int)in.u & 0x8000u) << 16;
  unsigned int exp = ((unsigned int)in.u >> 10) & 0x1fu;
  unsigned int mant = (unsigned int)in.u & 0x3ffu;
  union {
    unsigned int u;
    float f;
  } out;
  if (exp == 0) {
    if (mant == 0) {
      out.u = sign;
    } else {
      // Subnormal half: normalise it into a float.
      exp = 127 - 15 + 1;
      while ((mant & 0x400u) == 0u) {
        mant <<= 1;
        exp -= 1;
      }
      mant &= 0x3ffu;
      out.u = sign | (exp << 23) | (mant << 13);
    }
  } else if (exp == 0x1f) {
    out.u = sign | 0x7f800000u | (mant << 13);
  } else {
    out.u = sign | ((exp - 15 + 127) << 23) | (mant << 13);
  }
  return out.f;
#endif
}

// ---------------------------------------------------------------------------
// The raw forms. A program that wants the bit pattern uses these.
// ---------------------------------------------------------------------------

struct __half_raw {
  union {
    unsigned short x;
    _Float16 data;
  };
};

struct __half2_raw {
  union {
    struct {
      unsigned short x, y;
    };
    _Float16 data[2];
  };
};

// ---------------------------------------------------------------------------
// __half
// ---------------------------------------------------------------------------

struct __half {
  _Float16 __x;

  __half() = default;
  __half(const __half &) = default;
  __half &operator=(const __half &) = default;

  BC250_HALF_HD __half(_Float16 v) : __x(v) {}
  BC250_HALF_HD __half(float v) : __x(__bc250_float_to_half(v)) {}
  BC250_HALF_HD __half(double v) {
    __bc250_half_bits bits;
    bits.u = __bc250_double_to_float16_bits(v, 10u, 15);
    __x = bits.h;
  }
  BC250_HALF_HD __half(int v) : __x(__bc250_float_to_half((float)v)) {}
  BC250_HALF_HD __half(unsigned int v) : __x(__bc250_float_to_half((float)v)) {}
  BC250_HALF_HD __half(long long v) : __x(__bc250_float_to_half((float)v)) {}
  BC250_HALF_HD __half(unsigned long long v) : __x(__bc250_float_to_half((float)v)) {}
  BC250_HALF_HD __half(const __half_raw &r) : __x(r.data) {}

  // MEASURED: `operator float()` has to be the only implicit way out. With an
  // `operator _Float16()` next to it, `int32_t(x)` over a `__half` is ambiguous, because both
  // conversions reach int (ggml-cuda/convert.cuh:62). Code inside this header reads `__x`.
  BC250_HALF_HD operator float() const { return __bc250_half_to_float(__x); }
  BC250_HALF_HD operator __half_raw() const {
    __half_raw r;
    r.data = __x;
    return r;
  }
};

typedef __half half;

// ---------------------------------------------------------------------------
// __half2
//
// The storage is a two-element native vector, so that a packed operation becomes one
// instruction. `x` and `y` are the names CUDA gives the two halves, and ggml-cuda reads them
// (common.cuh:682). They are therefore a second view of the same four bytes.
// ---------------------------------------------------------------------------

typedef _Float16 __bc250_half2_vec __attribute__((ext_vector_type(2)));

struct __half2 {
  union {
    __bc250_half2_vec __v;
    struct {
      __half x, y;
    };
  };

  __half2() = default;
  __half2(const __half2 &) = default;
  __half2 &operator=(const __half2 &) = default;

  BC250_HALF_HD __half2(__bc250_half2_vec v) : __v(v) {}
  BC250_HALF_HD __half2(const __half &lo, const __half &hi) {
    __v[0] = lo.__x;
    __v[1] = hi.__x;
  }
  BC250_HALF_HD __half2(const __half2_raw &r) {
    __v[0] = r.data[0];
    __v[1] = r.data[1];
  }

  BC250_HALF_HD operator __half2_raw() const {
    __half2_raw r;
    r.data[0] = __v[0];
    r.data[1] = __v[1];
    return r;
  }
};

typedef __half2 half2;

// ---------------------------------------------------------------------------
// Conversion
// ---------------------------------------------------------------------------

BC250_HALF_HD __half __float2half(const float v) { return __half(v); }
BC250_HALF_HD __half __float2half_rn(const float v) { return __half(v); }
BC250_HALF_HD __half __float2half_rz(const float v) {
  return __half(__bc250_float_to_half_mode(v, BC250_HALF_RZ));
}
BC250_HALF_HD __half __float2half_rd(const float v) {
  return __half(__bc250_float_to_half_mode(v, BC250_HALF_RD));
}
BC250_HALF_HD __half __float2half_ru(const float v) {
  return __half(__bc250_float_to_half_mode(v, BC250_HALF_RU));
}
BC250_HALF_HD __half __double2half(const double v) { return __half(v); }
BC250_HALF_HD float __half2float(const __half h) { return __bc250_half_to_float(h.__x); }
BC250_HALF_HD __half __int2half_rn(const int v) { return __half(v); }
BC250_HALF_HD __half __uint2half_rn(const unsigned int v) { return __half(v); }
BC250_HALF_HD __half __short2half_rn(const short v) { return __half((int)v); }
BC250_HALF_HD __half __ushort2half_rn(const unsigned short v) { return __half((int)v); }
BC250_HALF_HD int __half2int_rn(const __half h) { return (int)__builtin_rintf((float)h.__x); }
BC250_HALF_HD int __half2int_rz(const __half h) { return (int)(float)h.__x; }
BC250_HALF_HD short __half_as_short(const __half h) {
  __half_raw r = (__half_raw)h;
  return (short)r.x;
}
BC250_HALF_HD unsigned short __half_as_ushort(const __half h) {
  __half_raw r = (__half_raw)h;
  return r.x;
}
BC250_HALF_HD __half __short_as_half(const short v) {
  __half_raw r;
  r.x = (unsigned short)v;
  return __half(r);
}
BC250_HALF_HD __half __ushort_as_half(const unsigned short v) {
  __half_raw r;
  r.x = v;
  return __half(r);
}

// ---------------------------------------------------------------------------
// The two halves of a __half2
// ---------------------------------------------------------------------------

BC250_HALF_HD __half __low2half(const __half2 a) { return __half(a.__v[0]); }
BC250_HALF_HD __half __high2half(const __half2 a) { return __half(a.__v[1]); }
BC250_HALF_HD float __low2float(const __half2 a) { return __bc250_half_to_float(a.__v[0]); }
BC250_HALF_HD float __high2float(const __half2 a) { return __bc250_half_to_float(a.__v[1]); }

BC250_HALF_HD __half2 __half2half2(const __half a) { return __half2(a, a); }
BC250_HALF_HD __half2 __halves2half2(const __half lo, const __half hi) { return __half2(lo, hi); }
BC250_HALF_HD __half2 __low2half2(const __half2 a) { return __half2(a.x, a.x); }
BC250_HALF_HD __half2 __high2half2(const __half2 a) { return __half2(a.y, a.y); }
BC250_HALF_HD __half2 __lows2half2(const __half2 a, const __half2 b) {
  return __half2(a.x, b.x);
}
BC250_HALF_HD __half2 __highs2half2(const __half2 a, const __half2 b) {
  return __half2(a.y, b.y);
}
BC250_HALF_HD __half2 __lowhigh2highlow(const __half2 a) { return __half2(a.y, a.x); }

BC250_HALF_HD float2 __half22float2(const __half2 a) {
  return make_float2(__bc250_half_to_float(a.__v[0]), __bc250_half_to_float(a.__v[1]));
}
BC250_HALF_HD __half2 __float22half2_rn(const float2 a) {
  return __half2(__half(a.x), __half(a.y));
}
BC250_HALF_HD __half2 __floats2half2_rn(const float a, const float b) {
  return __half2(__half(a), __half(b));
}
BC250_HALF_HD __half2 make_half2(const __half a, const __half b) { return __half2(a, b); }

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

BC250_HALF_HD __half __hadd(const __half a, const __half b) { return __half(a.__x + b.__x); }
BC250_HALF_HD __half __hsub(const __half a, const __half b) { return __half(a.__x - b.__x); }
BC250_HALF_HD __half __hmul(const __half a, const __half b) { return __half(a.__x * b.__x); }
BC250_HALF_HD __half __hdiv(const __half a, const __half b) { return __half(a.__x / b.__x); }
BC250_HALF_HD __half __hneg(const __half a) { return __half((_Float16)(-a.__x)); }
BC250_HALF_HD __half __habs(const __half a) {
  return __half((_Float16)__builtin_fabsf((float)a.__x));
}
BC250_HALF_HD __half __hfma(const __half a, const __half b, const __half c) {
  return __half((_Float16)__builtin_fmaf16(a.__x, b.__x, c.__x));
}
BC250_HALF_HD __half __hfma_relu(const __half a, const __half b, const __half c) {
  const _Float16 r = __builtin_fmaf16(a.__x, b.__x, c.__x);
  return __half(__builtin_isnan((float)r) ? r : (r > (_Float16)0 ? r : (_Float16)0));
}
BC250_HALF_HD __half __hmax(const __half a, const __half b) {
  return __half((_Float16)__builtin_fmaxf((float)a.__x, (float)b.__x));
}
BC250_HALF_HD __half __hmin(const __half a, const __half b) {
  return __half((_Float16)__builtin_fminf((float)a.__x, (float)b.__x));
}
BC250_HALF_HD __half __hmax_nan(const __half a, const __half b) {
  if (__builtin_isnan((float)a.__x)) return a;
  if (__builtin_isnan((float)b.__x)) return b;
  return __hmax(a, b);
}
BC250_HALF_HD __half __hmin_nan(const __half a, const __half b) {
  if (__builtin_isnan((float)a.__x)) return a;
  if (__builtin_isnan((float)b.__x)) return b;
  return __hmin(a, b);
}

BC250_HALF_HD __half2 __hadd2(const __half2 a, const __half2 b) { return __half2(a.__v + b.__v); }
BC250_HALF_HD __half2 __hsub2(const __half2 a, const __half2 b) { return __half2(a.__v - b.__v); }
BC250_HALF_HD __half2 __hmul2(const __half2 a, const __half2 b) { return __half2(a.__v * b.__v); }
BC250_HALF_HD __half2 __h2div(const __half2 a, const __half2 b) { return __half2(a.__v / b.__v); }
BC250_HALF_HD __half2 __hneg2(const __half2 a) { return __half2(-a.__v); }
BC250_HALF_HD __half2 __habs2(const __half2 a) {
  return __half2(__habs(a.x), __habs(a.y));
}
BC250_HALF_HD __half2 __hfma2(const __half2 a, const __half2 b, const __half2 c) {
  return __half2(__hfma(a.x, b.x, c.x), __hfma(a.y, b.y, c.y));
}
BC250_HALF_HD __half2 __hmax2(const __half2 a, const __half2 b) {
  return __half2(__hmax(a.x, b.x), __hmax(a.y, b.y));
}
BC250_HALF_HD __half2 __hmin2(const __half2 a, const __half2 b) {
  return __half2(__hmin(a.x, b.x), __hmin(a.y, b.y));
}

// ---------------------------------------------------------------------------
// Comparison. `__hgt2_mask` is deliberately absent: ggml-cuda defines it itself for every HIP
// build (common.cuh:704), and a second definition here would be an error.
// ---------------------------------------------------------------------------

BC250_HALF_HD bool __heq(const __half a, const __half b) { return a.__x == b.__x; }
BC250_HALF_HD bool __hne(const __half a, const __half b) { return a.__x != b.__x; }
BC250_HALF_HD bool __hlt(const __half a, const __half b) { return a.__x < b.__x; }
BC250_HALF_HD bool __hle(const __half a, const __half b) { return a.__x <= b.__x; }
BC250_HALF_HD bool __hgt(const __half a, const __half b) { return a.__x > b.__x; }
BC250_HALF_HD bool __hge(const __half a, const __half b) { return a.__x >= b.__x; }
BC250_HALF_HD bool __hisnan(const __half a) { return __builtin_isnan((float)a.__x); }
BC250_HALF_HD bool __hisinf(const __half a) { return __builtin_isinf((float)a.__x); }

BC250_HALF_HD bool __hbeq2(const __half2 a, const __half2 b) {
  return a.__v[0] == b.__v[0] && a.__v[1] == b.__v[1];
}
BC250_HALF_HD bool __hbgt2(const __half2 a, const __half2 b) {
  return a.__v[0] > b.__v[0] && a.__v[1] > b.__v[1];
}
BC250_HALF_HD __half2 __heq2(const __half2 a, const __half2 b) {
  return __half2(__half((_Float16)(a.__v[0] == b.__v[0] ? 1 : 0)),
                 __half((_Float16)(a.__v[1] == b.__v[1] ? 1 : 0)));
}
BC250_HALF_HD __half2 __hgt2(const __half2 a, const __half2 b) {
  return __half2(__half((_Float16)(a.__v[0] > b.__v[0] ? 1 : 0)),
                 __half((_Float16)(a.__v[1] > b.__v[1] ? 1 : 0)));
}
BC250_HALF_HD __half2 __hlt2(const __half2 a, const __half2 b) {
  return __half2(__half((_Float16)(a.__v[0] < b.__v[0] ? 1 : 0)),
                 __half((_Float16)(a.__v[1] < b.__v[1] ? 1 : 0)));
}

// ---------------------------------------------------------------------------
// Operators. CUDA gives `__half` and `__half2` the full set, and code written for CUDA uses it.
// ---------------------------------------------------------------------------

BC250_HALF_HD __half operator+(const __half a, const __half b) { return __hadd(a, b); }
BC250_HALF_HD __half operator-(const __half a, const __half b) { return __hsub(a, b); }
BC250_HALF_HD __half operator*(const __half a, const __half b) { return __hmul(a, b); }
BC250_HALF_HD __half operator/(const __half a, const __half b) { return __hdiv(a, b); }
BC250_HALF_HD __half operator-(const __half a) { return __hneg(a); }
BC250_HALF_HD __half &operator+=(__half &a, const __half b) { a = __hadd(a, b); return a; }
BC250_HALF_HD __half &operator-=(__half &a, const __half b) { a = __hsub(a, b); return a; }
BC250_HALF_HD __half &operator*=(__half &a, const __half b) { a = __hmul(a, b); return a; }
BC250_HALF_HD __half &operator/=(__half &a, const __half b) { a = __hdiv(a, b); return a; }
BC250_HALF_HD bool operator==(const __half a, const __half b) { return __heq(a, b); }
BC250_HALF_HD bool operator!=(const __half a, const __half b) { return __hne(a, b); }
BC250_HALF_HD bool operator<(const __half a, const __half b) { return __hlt(a, b); }
BC250_HALF_HD bool operator<=(const __half a, const __half b) { return __hle(a, b); }
BC250_HALF_HD bool operator>(const __half a, const __half b) { return __hgt(a, b); }
BC250_HALF_HD bool operator>=(const __half a, const __half b) { return __hge(a, b); }

BC250_HALF_HD __half2 operator+(const __half2 a, const __half2 b) { return __hadd2(a, b); }
BC250_HALF_HD __half2 operator-(const __half2 a, const __half2 b) { return __hsub2(a, b); }
BC250_HALF_HD __half2 operator*(const __half2 a, const __half2 b) { return __hmul2(a, b); }
BC250_HALF_HD __half2 operator/(const __half2 a, const __half2 b) { return __h2div(a, b); }
BC250_HALF_HD __half2 operator-(const __half2 a) { return __hneg2(a); }
BC250_HALF_HD __half2 &operator+=(__half2 &a, const __half2 b) { a = __hadd2(a, b); return a; }
BC250_HALF_HD __half2 &operator-=(__half2 &a, const __half2 b) { a = __hsub2(a, b); return a; }
BC250_HALF_HD __half2 &operator*=(__half2 &a, const __half2 b) { a = __hmul2(a, b); return a; }

#endif  // BC250_HIP_FP16_H
