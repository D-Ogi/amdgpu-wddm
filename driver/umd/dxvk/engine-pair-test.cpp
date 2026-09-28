// SPDX-License-Identifier: MIT
#include "engine-abi.h"
#include "engine-modules.h"
#include <cstdio>
using namespace bc250::umd;
int wmain(int argc, wchar_t **argv) {
    if (argc!=3) return 2;
    EngineModules modules;
    const HRESULT hr=modules.open(argv[1],argv[2]);
    std::printf("shell_abi=%08X engine_abi=%08X hr=%08X\n",
        BC250_DXVK_ENGINE_ABI_VERSION,modules.functions().AbiVersion,static_cast<UINT>(hr));
    // Loading and export-table admission only: never create an instance/device.
    return SUCCEEDED(hr) ? 0 : 1;
}
