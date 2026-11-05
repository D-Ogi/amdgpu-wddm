// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
namespace bc250::umd {
// Pins a hosted module (the engine or the Vulkan ICD) that a shell loaded with LoadLibraryExW: the module then stays
// loaded until the process ends, and the FreeLibrary of the adapter close only drops the shell's reference.
// Why (BD-315): the hosted modules are Mesa and vkd3d/DXVK builds. They keep process-wide state (locks, caches, hash
// tables, heap allocations) that they do not release when they are unloaded. The D3D runtimes open and close an
// adapter many times in one process, and each close unloaded them; Application Verifier stops on that unload with
// LOCK_IN_UNLOADED_DLL (0x201) or with a leaked allocation (0x900), and without the verifier the next load starts
// over with state that was never released. A pinned module is the same module on the next OpenAdapter: its
// process-wide state stays, and the shell makes its per-adapter objects again.
// Returns false when the pin fails; the shell then keeps the earlier behaviour (unload at the last FreeLibrary).
inline bool pin_hosted_module(HMODULE module) noexcept {
    if (!module) return false;
    HMODULE pinned{};
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(module), &pinned) && pinned == module;
}
}
