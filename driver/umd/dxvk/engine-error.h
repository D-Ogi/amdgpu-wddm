// SPDX-License-Identifier: MIT
#pragma once
#include <d3d11.h>
namespace bc250::umd {
enum class EngineErrorPolicy { allow_out_of_memory, device_removed_only };
// The engine clears its latch on read. Preserve partial-work failures locally;
// a later successful read must never resurrect a lost device.
class EngineErrorState {
public:
    template<typename Read> HRESULT poll(Read &&read,EngineErrorPolicy policy) {
        if (FAILED(terminal_)) return terminal_;
        const HRESULT hr=read();
        if (hr==S_OK || (hr==E_OUTOFMEMORY && policy==EngineErrorPolicy::allow_out_of_memory)) return hr;
        // E_FAIL denotes partially executed work. Strict DDI entries also
        // require removal for OOM; all other statuses violate ABI1.4.
        terminal_=DXGI_ERROR_DEVICE_REMOVED;
        return terminal_;
    }
private:
    HRESULT terminal_=S_OK;
};
}
