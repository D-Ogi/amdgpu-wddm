// amd_abi_probe_ours.cpp - the probe of amd_abi_probe.inc under our hip/hip_runtime.h.
#include <cstddef>
#include <cstdint>

#include "hip/hip_runtime.h"

#define BC250_ABI_DUMP bc250_abi_dump_ours
#include "amd_abi_probe.inc"

void bc250_abi_dump_ours_entry(std::FILE* out) { bc250_abi_dump_ours(out); }
