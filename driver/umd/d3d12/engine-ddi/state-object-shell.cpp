// SPDX-License-Identifier: MIT
// engine-ddi: the owner of a state object, for the slots whose only handle is the state object (D110-D113:
// GetShaderIdentifier, GetShaderStackSize, GetPipelineStackSize, SetPipelineStackSize). The shell enters its device
// scope from the first handle of a slot; without this lookup it has no device for these four and refuses them.
#include "internal.h"

namespace engine_ddi {

void* state_object_shell(D3D12DDI_HSTATEOBJECT_0054 state_object) noexcept {
    const auto* r = record_of<StateObjectRecord>(state_object.pDrvPrivate, Tag::StateObject);
    return (r && r->h.device) ? r->h.device->hooks.shell : nullptr;
}

} // namespace engine_ddi
