// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
#include "engine-modules.h"
namespace bc250::umd {
// Internal creation transaction. The adapter must retain failedCleanup until it
// can retire it, even when the runtime frees hDrvDevice after failed creation.
// Adapter/capability negotiation and DXGI table publication are separate steps.
HRESULT create_render_device(const D3D10DDIARG_CREATEDEVICE &,UINT64 luid,
    PFN_vkGetInstanceProcAddr,const BC250_DXVK_ENGINE_FUNCS &,D3D_FEATURE_LEVEL,
    const BC250_DXVK_SHELL_SERVICES &,DdiDeviceHandle &failedCleanup) noexcept;
inline HRESULT create_render_device(const D3D10DDIARG_CREATEDEVICE &args,UINT64 luid,
    const EngineModules &modules,D3D_FEATURE_LEVEL level,const BC250_DXVK_SHELL_SERVICES &services,
    DdiDeviceHandle &failedCleanup) noexcept {
    if (!modules.loaded()) return E_UNEXPECTED;
    return create_render_device(args,luid,modules.get_instance_proc_addr(),modules.functions(),level,services,failedCleanup);
}
}
