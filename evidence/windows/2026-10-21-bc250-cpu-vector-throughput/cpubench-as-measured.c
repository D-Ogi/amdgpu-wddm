/* cpubench.c - what the lab CPU reports through CPUID, and what the GDI route's RGBA8 -> BGRA8 row copy costs
 * with four implementations (the per-byte loop of wsi_common_win32.cpp, a scalar 32-bit word swap, SSSE3 pshufb,
 * AVX2 vpshufb), plus memcpy, and the throughput of 128-bit against 256-bit FMA and shuffles in registers.
 * BD-197 follow-up, 2026-10-21. Build: build.cmd (MSVC, /O2, no /arch so the SSE/AVX paths are only the intrinsics).
 * Run: cpubench.exe [repeats]. Prints one "key value" line per result; nothing is written to disk.
 */
#include <immintrin.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static double now_s(void)
{
   static LARGE_INTEGER f;
   LARGE_INTEGER c;
   if (!f.QuadPart)
      QueryPerformanceFrequency(&f);
   QueryPerformanceCounter(&c);
   return (double)c.QuadPart / (double)f.QuadPart;
}

static void cpuid_dump(int *has_ssse3, int *has_avx2, int *has_fma)
{
   int r[4], max, ext;
   char brand[49] = {0};
   unsigned long long xcr0 = 0;
   __cpuid(r, 0);
   max = r[0];
   __cpuid(r, 0x80000000);
   ext = r[0];
   if (ext >= 0x80000004) {
      __cpuid((int *)(brand + 0), 0x80000002);
      __cpuid((int *)(brand + 16), 0x80000003);
      __cpuid((int *)(brand + 32), 0x80000004);
   }
   printf("brand %s\n", brand);
   __cpuid(r, 1);
   int ecx1 = r[2], edx1 = r[3];
   printf("family-model-stepping eax=0x%08X\n", r[0]);
   int osxsave = (ecx1 >> 27) & 1;
   if (osxsave)
      xcr0 = _xgetbv(0);
   int ymm_os = osxsave && ((xcr0 & 6) == 6);
   int ebx7 = 0, ecx7 = 0, edx7 = 0;
   if (max >= 7) {
      __cpuidex(r, 7, 0);
      ebx7 = r[1];
      ecx7 = r[2];
      edx7 = r[3];
   }
   __cpuid(r, 0x80000001);
   int ecx81 = r[2];
   struct { const char *n; int v; } f[] = {
      {"sse2", (edx1 >> 26) & 1}, {"sse3", ecx1 & 1}, {"ssse3", (ecx1 >> 9) & 1},
      {"sse4.1", (ecx1 >> 19) & 1}, {"sse4.2", (ecx1 >> 20) & 1}, {"popcnt", (ecx1 >> 23) & 1},
      {"aes", (ecx1 >> 25) & 1}, {"pclmulqdq", (ecx1 >> 1) & 1}, {"avx", ((ecx1 >> 28) & 1) && ymm_os},
      {"fma3", ((ecx1 >> 12) & 1) && ymm_os}, {"f16c", (ecx1 >> 29) & 1}, {"rdrand", (ecx1 >> 30) & 1},
      {"avx2", ((ebx7 >> 5) & 1) && ymm_os}, {"bmi1", (ebx7 >> 3) & 1}, {"bmi2", (ebx7 >> 8) & 1},
      {"adx", (ebx7 >> 19) & 1}, {"sha", (ebx7 >> 29) & 1}, {"rdseed", (ebx7 >> 18) & 1},
      {"clflushopt", (ebx7 >> 23) & 1}, {"clwb", (ebx7 >> 24) & 1}, {"avx512f", (ebx7 >> 16) & 1},
      {"vaes", (ecx7 >> 9) & 1}, {"vpclmulqdq", (ecx7 >> 10) & 1}, {"rdpid", (ecx7 >> 22) & 1},
      {"fsrm", (edx7 >> 4) & 1}, {"sse4a", (ecx81 >> 6) & 1}, {"lzcnt", (ecx81 >> 5) & 1},
      {"fma4", (ecx81 >> 16) & 1}, {"xop", (ecx81 >> 11) & 1},
   };
   printf("cpuid");
   for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); i++)
      printf(" %s=%d", f[i].n, f[i].v);
   printf("\nxcr0 0x%llx\n", xcr0);
   *has_ssse3 = (ecx1 >> 9) & 1;
   *has_avx2 = ((ebx7 >> 5) & 1) && ymm_os;
   *has_fma = ((ecx1 >> 12) & 1) && ymm_os;
}

/* The loop of wsi_common_win32.cpp's GDI present for VK_FORMAT_R8G8B8A8_UNORM, per row. */
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

static void row_memcpy(uint8_t *d, const uint8_t *s, unsigned w)
{
   memcpy(d, s, (size_t)w * 4);
}

typedef void (*row_fn)(uint8_t *, const uint8_t *, unsigned);

/* Best of `reps` passes over a w x h frame; returns ms per frame. */
static double frame_ms(row_fn fn, uint8_t *dst, const uint8_t *src, unsigned w, unsigned h, int reps)
{
   double best = 1e9;
   for (int r = 0; r < reps; r++) {
      double t0 = now_s();
      for (unsigned y = 0; y < h; y++)
         fn(dst + (size_t)y * w * 4, src + (size_t)y * w * 4, w);
      double t = (now_s() - t0) * 1000.0;
      if (t < best)
         best = t;
   }
   return best;
}

#include "kernels.inc"

int main(int argc, char **argv)
{
   int reps = argc > 1 ? atoi(argv[1]) : 15;
   int ssse3, avx2, fma;
   SetThreadAffinityMask(GetCurrentThread(), 1ull << 2);
   SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
   cpuid_dump(&ssse3, &avx2, &fma);

   const unsigned w = 1920, h = 1200;
   size_t bytes = (size_t)w * h * 4;
   uint8_t *src = (uint8_t *)_aligned_malloc(bytes, 64), *dst = (uint8_t *)_aligned_malloc(bytes, 64);
   uint8_t *ref = (uint8_t *)_aligned_malloc(bytes, 64);
   for (size_t i = 0; i < bytes; i++) src[i] = (uint8_t)(i * 2654435761u >> 13);
   memset(dst, 0, bytes);
   row_bytes(ref, src, (unsigned)(bytes / 4)); /* reference output */

   struct { const char *n; row_fn f; int ok; } impl[] = {
      {"bytes", row_bytes, 1}, {"words", row_words, 1}, {"ssse3", row_ssse3, ssse3},
      {"avx2", row_avx2, avx2}, {"memcpy", row_memcpy, 1},
   };
   for (unsigned i = 0; i < sizeof(impl) / sizeof(impl[0]); i++) {
      if (!impl[i].ok) { printf("frame %s unsupported\n", impl[i].n); continue; }
      double ms = frame_ms(impl[i].f, dst, src, w, h, reps);
      int same = impl[i].f == row_memcpy ? memcmp(dst, src, bytes) == 0 : memcmp(dst, ref, bytes) == 0;
      printf("frame %s 1920x1200 %.3f ms %.2f GB/s %s\n", impl[i].n, ms, bytes / (ms * 1e6),
             same ? "match" : "MISMATCH");
   }
   /* The same on a 16 KiB row block that stays in L1, to separate compute from memory. */
   for (unsigned i = 0; i < sizeof(impl) / sizeof(impl[0]); i++) {
      if (!impl[i].ok) continue;
      double best = 1e9;
      for (int r = 0; r < reps; r++) {
         double t0 = now_s();
         for (int k = 0; k < 2000; k++) impl[i].f(dst, src, 4096);
         double t = (now_s() - t0) * 1e9 / (2000.0 * 4096 * 4);
         if (t < best) best = t;
      }
      printf("l1 %s %.4f ns/byte %.2f GB/s\n", impl[i].n, best, 1.0 / best);
   }
   long n = 50 * 1000 * 1000;
   printf("reg shuf128 %.4f ns/insn\n", ssse3 ? shuf128_ns(n) : -1.0);
   printf("reg shuf256 %.4f ns/insn\n", avx2 ? shuf256_ns(n) : -1.0);
   printf("reg fma128 %.4f ns/insn\n", fma ? fma128_ns(n) : -1.0);
   printf("reg fma256 %.4f ns/insn\n", fma ? fma256_ns(n) : -1.0);
   printf("reg add128 %.4f ns/insn\n", avx2 ? add128_ns(n) : -1.0);
   printf("reg add256 %.4f ns/insn\n", avx2 ? add256_ns(n) : -1.0);
   printf("reg mul128 %.4f ns/insn\n", avx2 ? mul128_ns(n) : -1.0);
   printf("reg mul256 %.4f ns/insn\n", avx2 ? mul256_ns(n) : -1.0);
   printf("lat fma256 %.4f ns/insn\n", fma ? fma256_lat_ns(n) : -1.0);
   _aligned_free(src); _aligned_free(dst); _aligned_free(ref);
   return 0;
}
