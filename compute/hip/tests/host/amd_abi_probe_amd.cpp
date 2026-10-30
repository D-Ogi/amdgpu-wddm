// amd_abi_probe_amd.cpp - the probe of amd_abi_probe.inc under AMD's hip_runtime_api.h. The
// build passes AMD's include directory, and only this file sees it.
#include <cstddef>
#include <cstdint>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <hip/hip_runtime_api.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#define BC250_ABI_DUMP bc250_abi_dump_amd
#include "amd_abi_probe.inc"

void bc250_abi_dump_amd_entry(std::FILE* out) { bc250_abi_dump_amd(out); }
