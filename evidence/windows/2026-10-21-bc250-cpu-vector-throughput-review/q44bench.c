/* q44bench.c - follow-up measurements for the review of tools/win/cpubench (review questions 13-35; the Q
 * numbers in comments and the q14-q34 group names are those questions). The clang build holds explicit-assembly
 * kernels (kernels_gen.inc, gen.py) and explicit-assembly memory kernels; the MSVC build holds only the C parts
 * (row copies, the Mesa-shaped GDI loop, the C chain sweep), so that the production compiler's code is measured
 * too. One "key value" line per result; nothing is written to disk. Usage: q44bench.exe [quick]
 */
#include <immintrin.h>
#include <intrin.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static double qpf;
static double now_s(void)
{
   LARGE_INTEGER c;
   QueryPerformanceCounter(&c);
   return (double)c.QuadPart / qpf;
}

static int cmp_d(const void *a, const void *b)
{
   double x = *(const double *)a, y = *(const double *)b;
   return x < y ? -1 : x > y;
}
/* min, median, max of v[0..n) */
static void stats(double *v, int n, double *mn, double *md, double *mx)
{
   double t[64];
   memcpy(t, v, n * sizeof(double));
   qsort(t, n, sizeof(double), cmp_d);
   *mn = t[0];
   *md = (n & 1) ? t[n / 2] : 0.5 * (t[n / 2 - 1] + t[n / 2]);
   *mx = t[n - 1];
}

static void topology(void)
{
   DWORD len = 0;
   GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
   char *buf = (char *)malloc(len);
   if (!GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
      printf("topology error %lu\n", GetLastError());
      return;
   }
   int core = 0;
   for (DWORD off = 0; off < len; core++) {
      PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
      printf("topology core %d group %u mask 0x%llx smt %d efficiency %u\n", core, p->Processor.GroupMask[0].Group,
             (unsigned long long)p->Processor.GroupMask[0].Mask, (p->Processor.Flags & LTP_PC_SMT) ? 1 : 0,
             p->Processor.EfficiencyClass);
      off += p->Size;
   }
   free(buf);
}

/* ---------------------------------------------------------------- C row copies, as in cpubench.c */
static void row_bytes(uint8_t *d, const uint8_t *s, unsigned w)
{
   for (unsigned x = 0; x < w; x++) {
      d[x * 4 + 0] = s[x * 4 + 2];
      d[x * 4 + 1] = s[x * 4 + 1];
      d[x * 4 + 2] = s[x * 4 + 0];
      d[x * 4 + 3] = s[x * 4 + 3];
   }
}
static void row_words(uint8_t *d, const uint8_t *s, unsigned w)
{
   const uint32_t *s32 = (const uint32_t *)s;
   uint32_t *d32 = (uint32_t *)d;
   for (unsigned x = 0; x < w; x++) {
      uint32_t v = s32[x];
      d32[x] = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
   }
}
static void row_ssse3(uint8_t *d, const uint8_t *s, unsigned w)
{
   const __m128i m = _mm_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
   unsigned x = 0;
   for (; x + 4 <= w; x += 4)
      _mm_storeu_si128((__m128i *)(d + x * 4), _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(s + x * 4)), m));
   row_words(d + x * 4, s + x * 4, w - x);
}
static void row_avx2(uint8_t *d, const uint8_t *s, unsigned w)
{
   const __m256i m = _mm256_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15,
                                      2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
   unsigned x = 0;
   for (; x + 8 <= w; x += 8)
      _mm256_storeu_si256((__m256i *)(d + x * 4),
                          _mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i *)(s + x * 4)), m));
   row_words(d + x * 4, s + x * 4, w - x);
}
static void row_memcpy(uint8_t *d, const uint8_t *s, unsigned w) { memcpy(d, s, (size_t)w * 4); }

/* The GDI present loop of wsi_common_win32.cpp@042227dc:2299-2313, with the width and format read through the
 * chain pointer as there (fake_chain puts extent.width at the offset the shipped code reads, 0x39c). */
struct fake_chain {
   char pad[0x39c];
   unsigned width, height;
   char pad2[0x3b0 - 0x3a4];
   int format;
};
void mesa_gdi_copy(struct fake_chain *chain, uint8_t *dptr, const uint8_t *ptr, int dpitch, unsigned spitch);
void mesa_gdi_copy_hoisted(struct fake_chain *chain, uint8_t *dptr, const uint8_t *ptr, int dpitch, unsigned spitch);
typedef void (*mesa_fn)(struct fake_chain *, uint8_t *, const uint8_t *, int, unsigned);

/* ---------------------------------------------------------------- C+KEEP chain sweep (as kernels.inc) */
#ifdef __clang__
#define KEEP(v) __asm__ volatile("" : "+x"(v))
#else
#define KEEP(v) (void)0
#endif
#define CK(name, K, ...)                                                                                     \
   static double name(long n, double *chk)                                                                   \
   {                                                                                                         \
      const __m256 gb = _mm256_set1_ps(1.0000001f), gc = _mm256_set1_ps(1e-7f);                              \
      __m256 a[12];                                                                                          \
      for (int i = 0; i < 12; i++) a[i] = _mm256_set1_ps((float)i);                                          \
      __m256 a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4], a5 = a[5], a6 = a[6], a7 = a[7];          \
      __m256 a8 = a[8], a9 = a[9], a10 = a[10], a11 = a[11];                                                 \
      double t0 = now_s();                                                                                   \
      for (long k = 0; k < n; k++) { __VA_ARGS__ }                                                           \
      double t = now_s() - t0;                                                                               \
      *chk = _mm256_cvtss_f32(a0) + _mm256_cvtss_f32(a1) + _mm256_cvtss_f32(a2) + _mm256_cvtss_f32(a3) +      \
             _mm256_cvtss_f32(a4) + _mm256_cvtss_f32(a5) + _mm256_cvtss_f32(a6) + _mm256_cvtss_f32(a7) +      \
             _mm256_cvtss_f32(a8) + _mm256_cvtss_f32(a9) + _mm256_cvtss_f32(a10) + _mm256_cvtss_f32(a11);    \
      return t * 1e9 / ((double)n * K);                                                                      \
   }
#define F(x) x = _mm256_fmadd_ps(x, gb, gc); KEEP(x);
CK(c_fma256_k1, 4, F(a0) F(a0) F(a0) F(a0))
CK(c_fma256_k4, 4, F(a0) F(a1) F(a2) F(a3))
CK(c_fma256_k6, 6, F(a0) F(a1) F(a2) F(a3) F(a4) F(a5))
CK(c_fma256_k8, 8, F(a0) F(a1) F(a2) F(a3) F(a4) F(a5) F(a6) F(a7))
CK(c_fma256_k10, 10, F(a0) F(a1) F(a2) F(a3) F(a4) F(a5) F(a6) F(a7) F(a8) F(a9))
CK(c_fma256_k12, 12, F(a0) F(a1) F(a2) F(a3) F(a4) F(a5) F(a6) F(a7) F(a8) F(a9) F(a10) F(a11))
/* Q24 in C: FMA with a constant 1.0 multiplier. The disassembly says whether it stays an FMA. */
#define G(x) x = _mm256_fmadd_ps(x, _mm256_set1_ps(1.0f), gc); KEEP(x);
CK(c_fmaone256_k12, 12, G(a0) G(a1) G(a2) G(a3) G(a4) G(a5) G(a6) G(a7) G(a8) G(a9) G(a10) G(a11))

/* ---------------------------------------------------------------- explicit-assembly parts (clang only) */
#ifdef __clang__
#include "kernels_gen.inc"

/* Memory kernels over a whole frame: s = source, d = destination, n = bytes (a multiple of 256). */
#define MEMK(name, step, body)                                                                               \
   static __attribute__((noinline)) void name(uint8_t *d, const uint8_t *s, size_t n)                        \
   {                                                                                                         \
      const uint8_t *e = s + n;                                                                              \
      __asm__ volatile("vbroadcasti128 (%[m]), %%ymm12\n\t"                                                  \
                       "vpxor %%xmm0, %%xmm0, %%xmm0\n\tvpxor %%xmm1, %%xmm1, %%xmm1\n\t"                    \
                       "vpxor %%xmm2, %%xmm2, %%xmm2\n\tvpxor %%xmm3, %%xmm3, %%xmm3\n\t"                    \
                       "1:\n\t" body "\n\tadd $" #step ", %[s]\n\tadd $" #step ", %[d]\n\t"                    \
                       "cmp %[e], %[s]\n\tjb 1b\n\tsfence\n\tvmovdqu %%ymm0, (%[sink])\n\tvzeroupper"          \
                       : [s] "+r"(s), [d] "+r"(d)                                                            \
                       : [e] "r"(e), [m] "r"(shufmask), [sink] "r"(sinkbuf)                                  \
                       : "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm12");\
   }
static const uint8_t shufmask[16] __attribute__((aligned(16))) = {2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15};
static uint8_t sinkbuf[64] __attribute__((aligned(64)));
MEMK(m_read_x4, 64, "vpaddd (%[s]), %%xmm0, %%xmm0\n\tvpaddd 16(%[s]), %%xmm1, %%xmm1\n\t"
                    "vpaddd 32(%[s]), %%xmm2, %%xmm2\n\tvpaddd 48(%[s]), %%xmm3, %%xmm3")
MEMK(m_read_y4, 128, "vpaddd (%[s]), %%ymm0, %%ymm0\n\tvpaddd 32(%[s]), %%ymm1, %%ymm1\n\t"
                     "vpaddd 64(%[s]), %%ymm2, %%ymm2\n\tvpaddd 96(%[s]), %%ymm3, %%ymm3")
MEMK(m_write_x4, 64, "vmovdqa %%xmm0, (%[d])\n\tvmovdqa %%xmm1, 16(%[d])\n\tvmovdqa %%xmm2, 32(%[d])\n\t"
                     "vmovdqa %%xmm3, 48(%[d])")
MEMK(m_write_y4, 128, "vmovdqa %%ymm0, (%[d])\n\tvmovdqa %%ymm1, 32(%[d])\n\tvmovdqa %%ymm2, 64(%[d])\n\t"
                      "vmovdqa %%ymm3, 96(%[d])")
MEMK(m_write_nt_y4, 128, "vmovntdq %%ymm0, (%[d])\n\tvmovntdq %%ymm1, 32(%[d])\n\tvmovntdq %%ymm2, 64(%[d])\n\t"
                         "vmovntdq %%ymm3, 96(%[d])")
MEMK(m_copy_x1, 16, "vmovdqu (%[s]), %%xmm0\n\tvmovdqu %%xmm0, (%[d])")
MEMK(m_copy_x4, 64, "vmovdqu (%[s]), %%xmm0\n\tvmovdqu 16(%[s]), %%xmm1\n\tvmovdqu 32(%[s]), %%xmm2\n\t"
                    "vmovdqu 48(%[s]), %%xmm3\n\tvmovdqu %%xmm0, (%[d])\n\tvmovdqu %%xmm1, 16(%[d])\n\t"
                    "vmovdqu %%xmm2, 32(%[d])\n\tvmovdqu %%xmm3, 48(%[d])")
MEMK(m_copy_y1, 32, "vmovdqu (%[s]), %%ymm0\n\tvmovdqu %%ymm0, (%[d])")
MEMK(m_copy_y4, 128, "vmovdqu (%[s]), %%ymm0\n\tvmovdqu 32(%[s]), %%ymm1\n\tvmovdqu 64(%[s]), %%ymm2\n\t"
                     "vmovdqu 96(%[s]), %%ymm3\n\tvmovdqu %%ymm0, (%[d])\n\tvmovdqu %%ymm1, 32(%[d])\n\t"
                     "vmovdqu %%ymm2, 64(%[d])\n\tvmovdqu %%ymm3, 96(%[d])")
MEMK(m_shuf_x1, 16, "vmovdqu (%[s]), %%xmm0\n\tvpshufb %%xmm12, %%xmm0, %%xmm0\n\tvmovdqu %%xmm0, (%[d])")
MEMK(m_shuf_y1, 32, "vmovdqu (%[s]), %%ymm0\n\tvpshufb %%ymm12, %%ymm0, %%ymm0\n\tvmovdqu %%ymm0, (%[d])")
MEMK(m_shuf_y4, 128, "vmovdqu (%[s]), %%ymm0\n\tvmovdqu 32(%[s]), %%ymm1\n\tvmovdqu 64(%[s]), %%ymm2\n\t"
                     "vmovdqu 96(%[s]), %%ymm3\n\tvpshufb %%ymm12, %%ymm0, %%ymm0\n\tvpshufb %%ymm12, %%ymm1, %%ymm1\n\t"
                     "vpshufb %%ymm12, %%ymm2, %%ymm2\n\tvpshufb %%ymm12, %%ymm3, %%ymm3\n\t"
                     "vmovdqu %%ymm0, (%[d])\n\tvmovdqu %%ymm1, 32(%[d])\n\t"
                     "vmovdqu %%ymm2, 64(%[d])\n\tvmovdqu %%ymm3, 96(%[d])")
/* ymm copy with seven independent vector ALU ops between load and store, the spacing of MSVC's AVX2 word swap. */
MEMK(m_copy_y1_pad7, 32, "vmovdqu (%[s]), %%ymm0\n\tvpand %%ymm12, %%ymm4, %%ymm4\n\tvpand %%ymm12, %%ymm5, %%ymm5\n\t"
                         "vpand %%ymm12, %%ymm6, %%ymm6\n\tvpand %%ymm12, %%ymm7, %%ymm7\n\tvpor %%ymm12, %%ymm4, %%ymm4\n\t"
                         "vpor %%ymm12, %%ymm5, %%ymm5\n\tvpor %%ymm12, %%ymm6, %%ymm6\n\tvmovdqu %%ymm0, (%[d])")
MEMK(m_copy_nt_y4, 128, "vmovdqu (%[s]), %%ymm0\n\tvmovdqu 32(%[s]), %%ymm1\n\tvmovdqu 64(%[s]), %%ymm2\n\t"
                        "vmovdqu 96(%[s]), %%ymm3\n\tvmovntdq %%ymm0, (%[d])\n\tvmovntdq %%ymm1, 32(%[d])\n\t"
                        "vmovntdq %%ymm2, 64(%[d])\n\tvmovntdq %%ymm3, 96(%[d])")
static __attribute__((noinline)) void m_rep_movsb(uint8_t *d, const uint8_t *s, size_t n)
{
   __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}
static __attribute__((noinline)) void m_memcpy_frame(uint8_t *d, const uint8_t *s, size_t n) { memcpy(d, s, n); }
#endif

/* ---------------------------------------------------------------- frame tests */
static const unsigned W = 1920, H = 1200;
static uint8_t *src, *dst, *ref, *dst2;
static size_t BYTES;

typedef void (*row_fn)(uint8_t *, const uint8_t *, unsigned);
typedef void (*frame_fn)(uint8_t *, const uint8_t *, size_t);

static void report_frame(const char *name, double *ms, int reps, int ok, double traffic_factor, uint8_t *d)
{
   double mn, md, mx;
   stats(ms, reps, &mn, &md, &mx);
   printf("frame %-16s ms min %.3f med %.3f max %.3f | image GB/s %.2f | est traffic x%.0f GB/s %.2f | dst&4095 %4u | %s\n",
          name, mn, md, mx, BYTES / (mn * 1e6), traffic_factor, traffic_factor * BYTES / (mn * 1e6),
          (unsigned)((uintptr_t)d & 4095), ok < 0 ? "n/a" : ok ? "match" : "MISMATCH");
}

static void frame_rows(const char *name, row_fn fn, uint8_t *d, int reps, int is_copy)
{
   double ms[64];
   fn(d, src, W); /* warm the code */
   for (unsigned y = 0; y < H; y++) fn(d + (size_t)y * W * 4, src + (size_t)y * W * 4, W);
   for (int r = 0; r < reps; r++) {
      double t0 = now_s();
      for (unsigned y = 0; y < H; y++) fn(d + (size_t)y * W * 4, src + (size_t)y * W * 4, W);
      ms[r] = (now_s() - t0) * 1000.0;
   }
   report_frame(name, ms, reps, memcmp(d, is_copy ? src : ref, BYTES) == 0, 3, d);
}

static void frame_whole(const char *name, frame_fn fn, uint8_t *d, int reps, int check, double tf)
{
   double ms[64];
   fn(d, src, BYTES);
   for (int r = 0; r < reps; r++) {
      double t0 = now_s();
      fn(d, src, BYTES);
      ms[r] = (now_s() - t0) * 1000.0;
   }
   report_frame(name, ms, reps, check == 0 ? -1 : memcmp(d, check == 1 ? src : ref, BYTES) == 0, tf, d);
}

static void frame_mesa(const char *name, mesa_fn fn, uint8_t *d, int reps)
{
   static struct fake_chain ch;
   ch.width = W; ch.height = H; ch.format = 37; /* VK_FORMAT_R8G8B8A8_UNORM */
   double ms[64];
   memset(d, 0, BYTES);
   fn(&ch, d, src, W * 4, W * 4);
   for (int r = 0; r < reps; r++) {
      double t0 = now_s();
      fn(&ch, d, src, W * 4, W * 4);
      ms[r] = (now_s() - t0) * 1000.0;
   }
   report_frame(name, ms, reps, memcmp(d, ref, BYTES) == 0, 3, d);
}

/* ---------------------------------------------------------------- register kernels */
#ifdef __clang__
static float kin[16][8] __attribute__((aligned(64)));
static float kout[1024] __attribute__((aligned(64)));
static double ghz_now = 0;

static void setup_in(const char *name, int rot)
{
   for (int i = 0; i < 12; i++)
      for (int l = 0; l < 8; l++) kin[i][l] = (float)i;
   if (strstr(name, "pshufb") || strstr(name, "vpadd") || strstr(name, "vpmul") || strstr(name, "vperm"))
      for (int i = 0; i < 12; i++)
         for (int l = 0; l < 8; l++) ((uint32_t *)kin[i])[l] = (uint32_t)(i * 0x01020304u + l * 0x11111111u);
   uint8_t *m = (uint8_t *)kin[12];
   for (int b = 0; b < 32; b++)
      m[b] = rot ? (uint8_t)(((b & 15) + 1) & 15) : (uint8_t)((b & 12) | ((b & 3) == 0 ? 2 : (b & 3) == 2 ? 0 : (b & 3)));
   if (strstr(name, "vpermd"))
      for (int l = 0; l < 8; l++) ((uint32_t *)kin[12])[l] = (uint32_t)((l + 1) & 7);
   for (int l = 0; l < 8; l++) { kin[13][l] = 1.0f; kin[14][l] = 1.0000001f; kin[15][l] = 1e-7f; }
   for (int i = 0; i < 256; i++) kout[256 + i] = (float)(i & 15); /* memory slots at out + 1024 bytes */
}

static int is_fp(const char *n)
{
   if (strstr(n, "pshufb") || strstr(n, "vpaddd")) return 0; /* mixes with integer chains hold non-FP bits */
   return !strncmp(n, "fma", 3) || !strncmp(n, "fadd", 4) || !strncmp(n, "fmul", 4) || !strncmp(n, "mix_f", 5) ||
          !strncmp(n, "mem_", 4);
}

/* Runs one kernel: calibrate to ~target_s, then `reps` timed passes. Returns the median ns per instruction. */
static double run_k(const struct kspec *k, int reps, double target_s, int rot, const char *tag)
{
   setup_in(k->name, rot);
   uint64_t it = 2000;
   double t0 = now_s();
   k->fn(it, kin, kout);
   double t = now_s() - t0;
   if (t < 1e-5) t = 1e-5;
   it = (uint64_t)(it * target_s / t);
   if (it < 1000) it = 1000;
   double ns[64];
   unsigned long long cyc_tsc = 0;
   for (int r = 0; r < reps; r++) {
      setup_in(k->name, rot);
      unsigned long long c0 = __rdtsc();
      t0 = now_s();
      k->fn(it, kin, kout);
      t = now_s() - t0;
      cyc_tsc = __rdtsc() - c0;
      ns[r] = t * 1e9 / ((double)it * k->insns);
   }
   double mn, md, mx;
   stats(ns, reps, &mn, &md, &mx);
   printf("reg %-22s %s w%-3d insns/iter %3d iters %10llu | ns/insn min %.4f med %.4f max %.4f | cyc/insn %.3f | insn/cyc %.3f",
          k->name, tag, k->width, k->insns, (unsigned long long)it, mn, md, mx, md * ghz_now, 1.0 / (md * ghz_now));
   if (k->width >= 128 && is_fp(k->name)) {
      const float *v = strncmp(k->name, "mem_", 4) ? kout : kout + 256;
      int nv = strncmp(k->name, "mem_", 4) ? 12 * 8 : 26 * 8, fin = 1;
      float lo = INFINITY, hi = -INFINITY;
      for (int i = 0; i < nv; i++) {
         if (!strncmp(k->name, "mem_", 4) && i >= 10 * 8 && i < 16 * 8) continue;
         if (!isfinite(v[i])) fin = 0;
         if (v[i] < lo) lo = v[i];
         if (v[i] > hi) hi = v[i];
      }
      printf(" | values finite %d min %.6g max %.6g", fin, lo, hi);
   }
   printf(" | tsc/insn %.3f\n", (double)cyc_tsc / ((double)it * k->insns));
   (void)cyc_tsc;
   return md;
}

static const struct kspec *find_k(const char *name)
{
   for (unsigned i = 0; i < sizeof(kspecs) / sizeof(kspecs[0]); i++)
      if (!strcmp(kspecs[i].name, name)) return &kspecs[i];
   printf("missing kernel %s\n", name);
   exit(2);
}

static double clock_ghz(const char *when)
{
   const struct kspec *k = find_k("clk_add_chain");
   double ns[16];
   setup_in(k->name, 0);
   uint64_t it = 200000; /* 20 M dependent adds */
   for (int r = 0; r < 7; r++) {
      double t0 = now_s();
      k->fn(it, kin, kout);
      ns[r] = (now_s() - t0) * 1e9 / ((double)it * k->insns);
   }
   double mn, md, mx;
   stats(ns, 7, &mn, &md, &mx);
   ghz_now = 1.0 / md;
   printf("clock %-10s dependent add chain ns/add min %.5f med %.5f max %.5f -> %.3f GHz (spread %.3f-%.3f)\n", when, mn,
          md, mx, 1.0 / md, 1.0 / mx, 1.0 / mn);
   return ghz_now;
}

/* ---------------------------------------------------------------- SMT (Q30) */
struct smt_arg { const struct kspec *k; uint64_t it; DWORD_PTR mask; volatile LONG *go; double ns; };
static DWORD WINAPI smt_thread(LPVOID p)
{
   struct smt_arg *a = (struct smt_arg *)p;
   SetThreadAffinityMask(GetCurrentThread(), a->mask);
   SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
   static __declspec(thread) float tin[16][8], tout[1024];
   memcpy(tin, kin, sizeof(kin));
   InterlockedIncrement(a->go);
   while (*a->go < 100) _mm_pause();
   double t0 = now_s();
   a->k->fn(a->it, tin, tout);
   a->ns = (now_s() - t0) * 1e9 / ((double)a->it * a->k->insns);
   return 0;
}
static void smt_case(const char *kname, const char *label, int nthr, DWORD_PTR m0, DWORD_PTR m1)
{
   const struct kspec *k = find_k(kname);
   setup_in(k->name, 0);
   uint64_t it = 2000;
   double t0 = now_s();
   k->fn(it, kin, kout);
   double t = now_s() - t0;
   it = (uint64_t)(it * 0.08 / (t > 1e-5 ? t : 1e-5));
   struct smt_arg a[2];
   volatile LONG go = 0;
   HANDLE h[2];
   for (int i = 0; i < nthr; i++) {
      a[i].k = k; a[i].it = it; a[i].mask = i ? m1 : m0; a[i].go = &go; a[i].ns = 0;
      h[i] = CreateThread(NULL, 0, smt_thread, &a[i], 0, NULL);
   }
   while (go < nthr) _mm_pause();
   InterlockedExchange(&go, 100);
   WaitForMultipleObjects(nthr, h, TRUE, INFINITE);
   double agg = 0;
   for (int i = 0; i < nthr; i++) { agg += 1.0 / a[i].ns; CloseHandle(h[i]); }
   printf("smt %-16s %-28s threads %d masks 0x%llx 0x%llx | ns/insn per thread %.4f %.4f | total insn/ns %.3f\n", kname,
          label, nthr, (unsigned long long)m0, (unsigned long long)(nthr > 1 ? m1 : 0), a[0].ns, nthr > 1 ? a[1].ns : 0.0,
          agg);
}
#endif

int main(int argc, char **argv)
{
   LARGE_INTEGER f;
   QueryPerformanceFrequency(&f);
   qpf = (double)f.QuadPart;
   int quick = argc > 1 && !strcmp(argv[1], "quick");
   int reps = quick ? 3 : 15, kreps = quick ? 3 : 5;
   double ktarget = quick ? 0.01 : 0.03;
   SetThreadAffinityMask(GetCurrentThread(), 1ull << 2);
   SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
   printf("q44bench compiler %s qpf %.0f\n",
#ifdef __clang__
          "clang " __clang_version__,
#else
          "msvc",
#endif
          qpf);
   topology();
   {  /* TSC rate against QPC over 50 ms of spinning */
      unsigned long long c0 = __rdtsc();
      double t0 = now_s();
      while (now_s() - t0 < 0.05) _mm_pause();
      printf("tsc rate %.4f GHz\n", (__rdtsc() - c0) / ((now_s() - t0) * 1e9));
   }

   BYTES = (size_t)W * H * 4;
   src = (uint8_t *)_aligned_malloc(BYTES, 64);
   dst = (uint8_t *)_aligned_malloc(BYTES, 64);
   ref = (uint8_t *)_aligned_malloc(BYTES, 64);
   uint8_t *big = (uint8_t *)_aligned_malloc(BYTES + 8192, 4096);
   for (size_t i = 0; i < BYTES; i++) src[i] = (uint8_t)(i * 2654435761u >> 13);
   memset(dst, 0, BYTES);
   memset(big, 0, BYTES + 8192);
   row_bytes(ref, src, (unsigned)(BYTES / 4));
   printf("buffers src&4095 %u dst&4095 %u ref&4095 %u bytes %zu\n", (unsigned)((uintptr_t)src & 4095),
          (unsigned)((uintptr_t)dst & 4095), (unsigned)((uintptr_t)ref & 4095), BYTES);

#ifdef __clang__
   clock_ghz("start");
#endif
   /* Q31/Q32/Q38: frame copies, C row functions (this compiler), then the explicit-assembly variants. */
   frame_rows("row_bytes", row_bytes, dst, reps, 0);
   frame_rows("row_words", row_words, dst, reps, 0);
   frame_rows("row_ssse3", row_ssse3, dst, reps, 0);
   frame_rows("row_avx2", row_avx2, dst, reps, 0);
   frame_rows("row_memcpy", row_memcpy, dst, reps, 1);
   frame_mesa("mesa_gdi_loop", mesa_gdi_copy, dst, reps);
   frame_mesa("mesa_gdi_hoisted", mesa_gdi_copy_hoisted, dst, reps);
#ifdef __clang__
   for (int pass = 0; pass < 2; pass++) {
      /* pass 0: dst as allocated; pass 1: dst at a page offset 2048 apart from src's, to test 4K aliasing */
      uint8_t *d = pass ? big + (((uintptr_t)src + 2048) & 4095) : dst;
      if (pass) printf("frame pass dst page offset src+2048\n");
      frame_whole("asm_copy_x1", m_copy_x1, d, reps, 1, 3);
      frame_whole("asm_copy_x4", m_copy_x4, d, reps, 1, 3);
      frame_whole("asm_copy_y1", m_copy_y1, d, reps, 1, 3);
      frame_whole("asm_copy_y4", m_copy_y4, d, reps, 1, 3);
      frame_whole("asm_shuf_x1", m_shuf_x1, d, reps, 2, 3);
      frame_whole("asm_shuf_y1", m_shuf_y1, d, reps, 2, 3);
      frame_whole("asm_shuf_y4", m_shuf_y4, d, reps, 2, 3);
      frame_whole("asm_copy_y1_pad7", m_copy_y1_pad7, d, reps, 1, 3);
      frame_whole("asm_copy_nt_y4", m_copy_nt_y4, d, reps, 1, 2);
      frame_whole("rep_movsb", m_rep_movsb, d, reps, 1, 3);
      frame_whole("memcpy_frame", m_memcpy_frame, d, reps, 1, 2);
      if (!pass) {
         frame_whole("asm_read_x4", m_read_x4, d, reps, 0, 1);
         frame_whole("asm_read_y4", m_read_y4, d, reps, 0, 1);
         frame_whole("asm_write_x4", m_write_x4, d, reps, 0, 2);
         frame_whole("asm_write_y4", m_write_y4, d, reps, 0, 2);
         frame_whole("asm_write_nt_y4", m_write_nt_y4, d, reps, 0, 1);
      }
   }
#endif
   /* C chain sweep with this compiler (Q15/Q19): MSVC keeps or spills depending on the chain count. */
   {
      double chk, v;
      long n = quick ? 2000000 : 10000000;
      struct { const char *n; double (*f)(long, double *); } c[] = {
         {"c_fma256_k1", c_fma256_k1}, {"c_fma256_k4", c_fma256_k4}, {"c_fma256_k6", c_fma256_k6},
         {"c_fma256_k8", c_fma256_k8}, {"c_fma256_k10", c_fma256_k10}, {"c_fma256_k12", c_fma256_k12},
         {"c_fmaone256_k12", c_fmaone256_k12}};
      for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
         double ns[8];
         for (int r = 0; r < 3; r++) ns[r] = c[i].f(n, &chk);
         double mn, md, mx;
         stats(ns, 3, &mn, &md, &mx);
         v = chk;
         printf("csweep %-16s ns/insn min %.4f med %.4f max %.4f | lane0 sum %.6g finite %d\n", c[i].n, mn, md, mx, v,
                isfinite(v) ? 1 : 0);
      }
   }
#ifdef __clang__
   /* Register kernels (Q15, Q22-Q27, Q34) */
   for (unsigned i = 0; i < sizeof(kspecs) / sizeof(kspecs[0]); i++) {
      if (!strcmp(kspecs[i].group, "clock")) continue;
      if (i == sizeof(kspecs) / sizeof(kspecs[0]) / 2) clock_ghz("middle");
      run_k(&kspecs[i], kreps, ktarget, 0, "    ");
      if (!strncmp(kspecs[i].name, "pshufb", 6)) run_k(&kspecs[i], kreps, ktarget, 1, "rot ");
   }
   run_k(find_k("clk_imul_chain"), kreps, ktarget, 0, "    ");
   run_k(find_k("clk_add48_fma4_256"), kreps, ktarget, 0, "    ");
   /* Q24 numeric check: FADD and FMA(a, 1.0, c) on identical inputs and iteration counts must give the same bits. */
   {
      static float o1[1024] __attribute__((aligned(64))), o2[1024] __attribute__((aligned(64)));
      setup_in("fadd256_c12", 0);
      find_k("fadd256_c12")->fn(100001, kin, o1);
      setup_in("fmaone256_c12", 0);
      find_k("fmaone256_c12")->fn(100001, kin, o2);
      printf("q24 fadd256 vs fmaone256 after 100001 iterations x 2 ops per chain: %s (lane0 chain1 %.9g vs %.9g)\n",
             memcmp(o1, o2, 12 * 32) == 0 ? "bit-identical" : "DIFFERENT", o1[8], o2[8]);
   }
   /* Q27: the result of 2*iters swaps depends only on the parity of the count; a 16-cycle rotation exposes count mod 16. */
   {
      static uint8_t o[4096] __attribute__((aligned(64)));
      const struct kspec *k = find_k("pshufb128_c12");
      for (int rot = 0; rot < 2; rot++)
         for (uint64_t it = 1000; it <= 1003; it++) {
            setup_in(k->name, rot);
            uint8_t init[16];
            memcpy(init, kin[1], 16);
            k->fn(it, kin, o);
            /* per chain per iteration: 24 / 12 = 2 shuffles */
            uint64_t m = 2 * it;
            uint8_t exp[16], tmp[16];
            memcpy(exp, init, 16);
            const uint8_t *mask = (const uint8_t *)kin[12];
            for (uint64_t s = 0; s < (rot ? (m % 16) : (m % 2)); s++) {
               for (int b = 0; b < 16; b++) tmp[b] = exp[mask[b]];
               memcpy(exp, tmp, 16);
            }
            printf("q27 mask %s iters %llu shuffles/chain %llu: result %s expected; result %s initial\n",
                   rot ? "rot16" : "swap ", (unsigned long long)it, (unsigned long long)m,
                   memcmp(o + 32, exp, 16) == 0 ? "==" : "!=", memcmp(o + 32, init, 16) == 0 ? "==" : "!=");
         }
   }
   clock_ghz("pre-smt");
   /* Q30: SMT siblings against separate cores. LP 2 and 3 are one core if topology says so (printed above). */
   {
      DWORD_PTR a = 1ull << 2, sib = 1ull << 3, other = 1ull << 4;
      const char *ks[] = {"fma256_c1", "fma256_c12", "fadd256_c12", "scalar_add_c8"};
      for (int i = 0; i < 4; i++) {
         smt_case(ks[i], "one thread LP2", 1, a, 0);
         smt_case(ks[i], "two threads LP2+LP3 (SMT?)", 2, a, sib);
         smt_case(ks[i], "two threads LP2+LP4 (cores)", 2, a, other);
      }
   }
   clock_ghz("end");
#endif
   _aligned_free(src); _aligned_free(dst); _aligned_free(ref); _aligned_free(big);
   printf("done\n");
   return 0;
}
