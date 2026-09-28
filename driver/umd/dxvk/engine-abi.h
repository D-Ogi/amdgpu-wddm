// SPDX-License-Identifier: MIT
#pragma once
#include "ddi/bc250_dxvk_engine.h"
namespace bc250::umd {
inline bool compatible_engine_abi(UINT32 engine,UINT32 shell=BC250_DXVK_ENGINE_ABI_VERSION) {
    return (engine>>16)==(shell>>16) && (engine & 0xffffu)>=(shell & 0xffffu);
}
}
