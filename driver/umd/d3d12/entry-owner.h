// SPDX-License-Identifier: MIT
#pragma once
#include "engine-ddi/engine-ddi.h"

namespace native12 {
// The owner an entry thunk (ddi-entry.h) resolves from a slot's first handle, for the handles whose owner the
// handle itself or an engine-ddi record names. native-tables.cpp's EntryPolicy is EntryOwner<Device> plus the
// queue; device-table-test.cpp runs these same overloads with an owner type of its own.
template<class Owner> struct EntryOwner {
    static Owner* resolve(D3D12DDI_HDEVICE handle) noexcept {return static_cast<Owner*>(handle.pDrvPrivate);}
    static Owner* resolve(D3D12DDI_HCOMMANDLIST handle) noexcept {
        return static_cast<Owner*>(engine_ddi::command_list_shell(handle));
    }
    // The identifier and stack size slots of a state object carry no other handle.
    static Owner* resolve(D3D12DDI_HSTATEOBJECT_0054 handle) noexcept {
        return static_cast<Owner*>(engine_ddi::state_object_shell(handle));
    }
    // Unsupported object-specific slots (e.g. metacommands) never borrow another
    // device's scope. Their creation already refuses in the engine boundary.
    template<class T> static Owner* resolve(T) noexcept {return nullptr;}
};
} // namespace native12
