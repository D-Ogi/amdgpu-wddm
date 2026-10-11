// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include "bc250_host_bootstrap.h"
#include <cstring>

namespace native12 {
inline HRESULT check_host_contract(PFN_vkGetInstanceProcAddr get, const char** reason) noexcept {
    using Query = VkResult(VKAPI_PTR*)(uint32_t, bc250_host_contract*);
    *reason = "missing-query";
    if (!get) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    const auto query = reinterpret_cast<Query>(get(VK_NULL_HANDLE, BC250_HOST_CONTRACT_QUERY_NAME));
    if (!query) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    bc250_host_contract actual{};
    *reason = "query-refused";
    if (query(sizeof(actual), &actual) != VK_SUCCESS) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    const auto expected = bc250_host_contract_description();
    *reason = "layout-or-feature-mismatch";
    if (std::memcmp(&actual, &expected, sizeof(actual))) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    *reason = "match";
    return S_OK;
}
}
