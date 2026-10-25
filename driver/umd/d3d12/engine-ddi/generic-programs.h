// SPDX-License-Identifier: MIT
#pragma once
#include "internal.h"
namespace engine_ddi {
struct GenericStateObject;
#if defined(D3D12DDI_BUILD_VERSION_0119)
bool generic_state_description(const D3D12DDIARG_CREATE_STATE_OBJECT_0054&) noexcept;
HRESULT create_generic_state(DeviceContext*, const D3D12DDIARG_CREATE_STATE_OBJECT_0054&,
                             const GenericStateObject*, GenericStateObject**) noexcept;
void destroy_generic_state(GenericStateObject*) noexcept;
#endif
}
