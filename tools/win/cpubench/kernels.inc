/* In-register throughput: 10 independent dependency chains in named locals (an array may live on the stack and
 * then measures store forwarding). Zen 2: FMA latency 5 on 2 pipes needs 10 chains in flight. ns per instruction. */
/* KEEP: an empty asm that takes and returns the whole register, so clang cannot narrow a chain to its lane 0 (vaddss
 * for vaddps) or compose two constant shuffles into one. MSVC has no such barrier; its reg lines are not valid anyway. */
#ifdef __clang__
#define KEEP(v) __asm__ volatile("" : "+x"(v))
#else
#define KEEP(v) (void)0
#endif
#define KERNEL(name, T, set1, op, sinkexpr, ...)                                                     \
static double name(long n)                                                                           \
{                                                                                                    \
   __VA_ARGS__;                                                                                      \
   T a0 = set1(0), a1 = set1(1), a2 = set1(2), a3 = set1(3), a4 = set1(4);                            \
   T a5 = set1(5), a6 = set1(6), a7 = set1(7), a8 = set1(8), a9 = set1(9);                            \
   double t0 = now_s();                                                                              \
   for (long k = 0; k < n; k++) {                                                                    \
      a0 = op(a0); a1 = op(a1); a2 = op(a2); a3 = op(a3); a4 = op(a4);                               \
      a5 = op(a5); a6 = op(a6); a7 = op(a7); a8 = op(a8); a9 = op(a9);                               \
      KEEP(a0); KEEP(a1); KEEP(a2); KEEP(a3); KEEP(a4);                                              \
      KEEP(a5); KEEP(a6); KEEP(a7); KEEP(a8); KEEP(a9);                                              \
   }                                                                                                 \
   double t = now_s() - t0;                                                                          \
   double sink = sinkexpr(a0) + sinkexpr(a1) + sinkexpr(a2) + sinkexpr(a3) + sinkexpr(a4) +          \
                 sinkexpr(a5) + sinkexpr(a6) + sinkexpr(a7) + sinkexpr(a8) + sinkexpr(a9);           \
   if (sink == 12345.0) printf("x");                                                                 \
   return t * 1e9 / ((double)n * 10);                                                                \
}
#define S128(i) _mm_set1_ps((float)(i))
#define S256(i) _mm256_set1_ps((float)(i))
#define I128(i) _mm_set1_epi32((i) * 0x01020304)
#define I256(i) _mm256_set1_epi32((i) * 0x01020304)
#define F128(x) _mm_fmadd_ps(x, fb, fc)
#define F256(x) _mm256_fmadd_ps(x, gb, gc)
#define P128(x) _mm_shuffle_epi8(x, m1)
#define P256(x) _mm256_shuffle_epi8(x, m2)
#define K128(x) (double)_mm_cvtss_f32(x)
#define K256(x) (double)_mm256_cvtss_f32(x)
#define J128(x) (double)_mm_cvtsi128_si32(x)
#define J256(x) (double)_mm256_cvtsi256_si32(x)
#define A128(x) _mm_add_ps(x, fc)
#define A256(x) _mm256_add_ps(x, gc)
#define M128(x) _mm_mul_ps(x, fb)
#define M256(x) _mm256_mul_ps(x, gb)
KERNEL(fma128_ns, __m128, S128, F128, K128, const __m128 fb = _mm_set1_ps(1.0000001f); const __m128 fc = _mm_set1_ps(1e-7f))
KERNEL(fma256_ns, __m256, S256, F256, K256, const __m256 gb = _mm256_set1_ps(1.0000001f); const __m256 gc = _mm256_set1_ps(1e-7f))
KERNEL(add128_ns, __m128, S128, A128, K128, const __m128 fc = _mm_set1_ps(1e-7f))
KERNEL(add256_ns, __m256, S256, A256, K256, const __m256 gc = _mm256_set1_ps(1e-7f))
KERNEL(mul128_ns, __m128, S128, M128, K128, const __m128 fb = _mm_set1_ps(1.0000001f))
KERNEL(mul256_ns, __m256, S256, M256, K256, const __m256 gb = _mm256_set1_ps(1.0000001f))

/* Latency: one dependency chain, ns per instruction. */
static double fma256_lat_ns(long n)
{
   const __m256 gb = _mm256_set1_ps(1.0000001f), gc = _mm256_set1_ps(1e-7f);
   __m256 a = _mm256_set1_ps(1.0f);
   double t0 = now_s();
   for (long k = 0; k < n; k++) {
      a = _mm256_fmadd_ps(a, gb, gc); a = _mm256_fmadd_ps(a, gb, gc);
      a = _mm256_fmadd_ps(a, gb, gc); a = _mm256_fmadd_ps(a, gb, gc);
      KEEP(a);
   }
   double t = now_s() - t0;
   if (_mm256_cvtss_f32(a) == 12345.0f) printf("x");
   return t * 1e9 / ((double)n * 4);
}
KERNEL(shuf128_ns, __m128i, I128, P128, J128,
       const __m128i m1 = _mm_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15))
KERNEL(shuf256_ns, __m256i, I256, P256, J256,
       const __m256i m2 = _mm256_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15,
                                           2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15))
