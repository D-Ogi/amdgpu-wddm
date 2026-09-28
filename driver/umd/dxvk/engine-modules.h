// SPDX-License-Identifier: MIT
#pragma once
#include "ddi/bc250_dxvk_engine.h"
namespace bc250::umd {
// Adapter-owned loader references. DeviceOwner takes independent references
// before using these entry points, including retained failed device teardown.
class EngineModules final {
public:
    EngineModules()=default;
    ~EngineModules();
    EngineModules(const EngineModules &)=delete;
    EngineModules &operator=(const EngineModules &)=delete;
    HRESULT open(const wchar_t *enginePath,const wchar_t *icdPath,
        const unsigned char *engineSha256=nullptr,const unsigned char *icdSha256=nullptr);
    void close();
    const BC250_DXVK_ENGINE_FUNCS &functions() const { return functions_; }
    PFN_vkGetInstanceProcAddr get_instance_proc_addr() const { return get_; }
    bool loaded() const { return engine_ && icd_ && get_; }
    static bool absolute_path(const wchar_t *path);
private:
    HMODULE engine_=nullptr,icd_=nullptr;
    BC250_DXVK_ENGINE_FUNCS functions_{};
    PFN_vkGetInstanceProcAddr get_=nullptr;
};
}
