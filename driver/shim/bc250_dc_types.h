// Fixed-width types for portable DC planners in host and WDK builds.
#pragma once
#include "bc250_atom_types.h"
#ifdef BC250_SHIM_KERNEL
typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef __int64 int64_t;
typedef unsigned char bool;
#define true 1
#define false 0
#define UINT64_MAX 0xffffffffffffffffull
#else
#include <stdbool.h>
#endif
