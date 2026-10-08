// dllmain.cpp - the entry point of amdhip64.dll.
//
// It does nothing on purpose. The device must not be closed here: DllMain runs under the
// loader lock, bc250hsa_close waits for the last submission, and a wait under the loader lock
// is how a process stops answering. Windows frees the device when the process ends, and the
// kernel driver frees its own objects with the process handle table.
//
// A HIP program that wants a clean stop calls hipDeviceSynchronize before it returns from
// main(). clang's module destructor calls __hipUnregisterFatBinary, which unloads the code
// object of that module only.

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        // No thread notifications: this runtime keeps no per-thread object that needs one.
        DisableThreadLibraryCalls(instance);
        break;
    case DLL_PROCESS_DETACH:
    default:
        break;
    }
    return TRUE;
}
