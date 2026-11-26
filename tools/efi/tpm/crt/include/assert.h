/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* <assert.h> of the stage C runtime (crt.h). A failed assertion writes one log line and leaves through the guard of
 * the engine call, as abort() does. */
#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
_Noreturn void crt_assert_fail(const char *expression, const char *file, int line);
#define assert(e) ((e) ? (void)0 : crt_assert_fail(#e, __FILE__, __LINE__))
#endif
#ifndef static_assert
#define static_assert _Static_assert
#endif
