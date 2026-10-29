// Compile on Windows x86 and x64. The shared header enforces wire offsets.
#include "dcn_latch_escape.h"
typedef char BC250_LATCH_ABI_REVISION_CHECK[(BC250_LATCH_PROBE_ABI==3)?1:-1];
