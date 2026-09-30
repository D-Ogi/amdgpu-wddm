// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-device-create.h"
namespace bc250::umd {
// Resolves the adapter's sparse-binding policy (registry value AmdgpuWddmSparseBinding, as the D3D12
// shell). S_OK with sparse set or cleared; a failure resolves to off. A handle the query could not close
// is returned in unresolved and stays with the adapter.
using SparsePolicyQuery=HRESULT (*)(UINT64 luid,bool &sparse,D3DKMT_HANDLE &unresolved) noexcept;
// The deployment layer supplies artifact-bound capabilities and absolute DLL
// paths. This transaction does not guess caps or load Vulkan at adapter open.
struct AdapterConfiguration {
    const wchar_t *engine_path;
    const wchar_t *icd_path;
    AdapterCaps caps;
    unsigned char engine_sha256[32]{};
    unsigned char icd_sha256[32]{};
    SparsePolicyQuery sparse_policy=nullptr; // nullptr: the adapter key through gdi32
};
HRESULT open_render_adapter(D3D10DDIARG_OPENADAPTER &,const AdapterConfiguration &) noexcept;
// The advertised caps under a resolved policy: FL12 needs sparse binding in the hosted ICD.
inline AdapterCaps adapter_caps_for_policy(const AdapterCaps &caps,UINT32 policy_flags) {
    return (policy_flags & BC250_HOST_POLICY_SPARSE) ? caps : without_fl12(caps);
}
}
