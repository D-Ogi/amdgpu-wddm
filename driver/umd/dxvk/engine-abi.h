// SPDX-License-Identifier: MIT
#pragma once
#include "ddi/bc250_dxvk_engine.h"
namespace bc250::umd {
// Mandatory deferred-error reporting begins with ABI1.4. A later header
// alone must not silently raise the deployed engine requirement.
inline constexpr UINT32 kRequiredEngineAbi=0x00010004u;
static_assert(BC250_DXVK_ENGINE_ABI_MAJOR==1 && BC250_DXVK_ENGINE_ABI_MINOR>=4);
inline bool compatible_engine_abi(UINT32 engine,UINT32 shell=kRequiredEngineAbi) {
    return (engine>>16)==(shell>>16) && (engine & 0xffffu)>=(shell & 0xffffu);
}
}
