// SDK 26100 compatibility prelude for clang-cl. User mode only.
#pragma once
#if !defined(__clang__) || !defined(_WIN32)
#error This prelude is for clang-cl on Windows only.
#endif
// MSVC's SDK has prefast/intrinsic pragmas and balanced packing include files.
// Keep those diagnostics enabled for project sources outside this include scope.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-pragmas"
#pragma clang diagnostic ignored "-Wpragma-pack"
#pragma clang diagnostic ignored "-Wignored-pragma-intrinsic"
#include <windows.h>
#include <winternl.h>
#pragma clang diagnostic pop
// winnt.h on i686 assigns T0=1 and T2=3. LLVM's _mm_prefetch intrinsic uses the
// instruction encoding T0=3 and T2=1. Use LLVM's definitions after the SDK, not
// whichever definitions happen to come last. No platform declaration is changed.
#if defined(_M_IX86)
#undef _MM_HINT_T0
#undef _MM_HINT_T1
#undef _MM_HINT_T2
#undef _MM_HINT_NTA
#endif
#include <intrin.h>
// With a message, so that C11 (the state client of the router) and C++11 take it as well.
static_assert(_MM_HINT_T0==3 && _MM_HINT_T1==2 && _MM_HINT_T2==1 && _MM_HINT_NTA==0, "LLVM prefetch hints");
