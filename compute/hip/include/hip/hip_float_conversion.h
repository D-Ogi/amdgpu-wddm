// Binary64 to a 16-bit IEEE-style format, round to nearest, ties to even.
// Integer rounding avoids double -> float -> 16-bit double rounding on host and device.
#ifndef BC250_HIP_FLOAT_CONVERSION_H
#define BC250_HIP_FLOAT_CONVERSION_H
#include <hip/hip_runtime.h>
__host__ __device__ __forceinline__ unsigned short
__bc250_double_to_float16_bits(double value, unsigned mantissa_bits, int bias) {
  union { double f; unsigned long long u; } bits;
  bits.f = value;
  const unsigned sign = (unsigned)(bits.u >> 48) & 0x8000u;
  const unsigned exponent = (unsigned)(bits.u >> 52) & 0x7ffu;
  const unsigned long long fraction = bits.u & 0xfffffffffffffULL;
  const unsigned max_exponent = (unsigned)(2 * bias + 1);
  if (exponent == 0x7ffu)
    return (unsigned short)(sign | (max_exponent << mantissa_bits) |
                            (fraction ? 1u << (mantissa_bits - 1) : 0u));
  if (exponent == 0u) return (unsigned short)sign;
  int target_exponent = (int)exponent - 1023 + bias;
  if (target_exponent >= (int)max_exponent)
    return (unsigned short)(sign | (max_exponent << mantissa_bits));
  const unsigned long long significand = fraction | (1ULL << 52);
  const int shift = 52 - (int)mantissa_bits + (target_exponent <= 0 ? 1 - target_exponent : 0);
  if (shift >= 64) return (unsigned short)sign;
  unsigned long long rounded = significand >> shift;
  const unsigned long long remainder = significand & ((1ULL << shift) - 1);
  const unsigned long long halfway = 1ULL << (shift - 1);
  if (remainder > halfway || (remainder == halfway && (rounded & 1))) ++rounded;
  if (target_exponent > 0)
    rounded += (unsigned long long)(target_exponent - 1) << mantissa_bits;
  return (unsigned short)(sign | (unsigned)rounded);
}
#endif
