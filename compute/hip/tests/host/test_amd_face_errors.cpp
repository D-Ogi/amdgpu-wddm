// test_amd_face_errors.cpp - the error codes of the AMD face and their names.
//
// hipBLASLt loads its kernels with hipModuleLoad of Kernels.so-000-<arch>.hsaco and tries the
// -xnack- and -xnack+ names after a miss. On unit A (kit2 of the PyTorch route, step 2f) its
// error stream said "unknown error" for each miss, and the log of our DLL said hipErrorUnknown:
// the face returned AMD's hipErrorFileNotFound (301), and hipGetErrorName and hipGetErrorString
// had no entry for it. The contract of hipModuleLoad (hip_runtime_api.h of HIP 7.18) names
// hipErrorFileNotFound for a file that is not there.
//
// The test loads the amdhip64_7.dll that argv[1] names (the mock DLL of build-amd-abi.ps1) and
// fails when:
//   - hipModuleLoad of a missing file does not return hipErrorFileNotFound, or does not leave it
//     for hipGetLastError;
//   - hipModuleLoad of a file of 3 bytes does not return hipErrorInvalidImage;
//   - hipGetErrorName of a code that the face returns is not AMD's name of that code, or
//     hipGetErrorString of it is the text of hipErrorUnknown, or hipDrvGetErrorString disagrees
//     with hipGetErrorString.
//
// Compiled against AMD's hip_runtime_api.h, as amd_abi.cpp is: the names and numbers are AMD's.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <hip/hip_runtime_api.h>

using ModuleLoad = hipError_t (*)(hipModule_t*, const char*);
using GetLastError_ = hipError_t (*)();
using GetName = const char* (*)(hipError_t);
using DrvGetString = hipError_t (*)(hipError_t, const char**);

namespace {

int g_failed = 0;

void expect(bool ok, const char* what, const std::string& detail) {
    if (!ok) {
        std::printf("FAIL: %s: %s\n", what, detail.c_str());
        ++g_failed;
    }
}

std::string temp_path(const char* leaf) {
    char dir[MAX_PATH] = {};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    std::string out = (n > 0 && n < MAX_PATH) ? std::string(dir) : std::string(".\\");
    return out + leaf;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_amd_face_errors <amdhip64_7.dll>\n");
        return 2;
    }
    HMODULE dll = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (dll == nullptr) {
        std::printf("FAIL: LoadLibrary error %lu\n", GetLastError());
        return 1;
    }
    auto module_load = reinterpret_cast<ModuleLoad>(GetProcAddress(dll, "hipModuleLoad"));
    auto last_error = reinterpret_cast<GetLastError_>(GetProcAddress(dll, "hipGetLastError"));
    auto get_name = reinterpret_cast<GetName>(GetProcAddress(dll, "hipGetErrorName"));
    auto get_string = reinterpret_cast<GetName>(GetProcAddress(dll, "hipGetErrorString"));
    auto drv_string = reinterpret_cast<DrvGetString>(GetProcAddress(dll, "hipDrvGetErrorString"));
    if (module_load == nullptr || last_error == nullptr || get_name == nullptr || get_string == nullptr ||
        drv_string == nullptr) {
        std::printf("FAIL: one of hipModuleLoad, hipGetLastError, hipGetErrorName, hipGetErrorString, "
                    "hipDrvGetErrorString is not exported\n");
        return 1;
    }

    // 1. A missing file. The name has the form of hipBLASLt's first try on unit A.
    const std::string missing = temp_path("bc250-no-such-dir-7f3a\\Kernels.so-000-gfx1010.hsaco");
    (void)last_error();
    hipModule_t module = reinterpret_cast<hipModule_t>(static_cast<uintptr_t>(0x1234));
    const hipError_t miss = module_load(&module, missing.c_str());
    expect(miss == hipErrorFileNotFound, "hipModuleLoad of a missing file",
           "returned " + std::to_string(static_cast<int>(miss)) + " (" + get_name(miss) +
               "), the contract is hipErrorFileNotFound (301)");
    const hipError_t sticky = last_error();
    expect(sticky == hipErrorFileNotFound, "hipGetLastError after the miss",
           "returned " + std::to_string(static_cast<int>(sticky)));

    // 2. A file that is there and is no code object.
    const std::string tiny = temp_path("bc250-face-errors-tiny.hsaco");
    if (std::FILE* f = std::fopen(tiny.c_str(), "wb")) {
        std::fwrite("ELF", 1, 3, f);
        std::fclose(f);
        const hipError_t bad = module_load(&module, tiny.c_str());
        expect(bad == hipErrorInvalidImage, "hipModuleLoad of a 3-byte file",
               "returned " + std::to_string(static_cast<int>(bad)) + " (" + get_name(bad) + ")");
        (void)last_error();
        std::remove(tiny.c_str());
    } else {
        expect(false, "hipModuleLoad of a 3-byte file", "cannot write " + tiny);
    }

    // 3. The names and texts of every code that the face returns: AMD's enumerator name, a text
    //    that is not the one of hipErrorUnknown, and the same text through hipDrvGetErrorString.
    const struct {
        hipError_t code;
        const char* name;
    } codes[] = {
#define BC250_CODE(sym) {sym, #sym},
        BC250_CODE(hipSuccess) BC250_CODE(hipErrorInvalidValue) BC250_CODE(hipErrorOutOfMemory)
        BC250_CODE(hipErrorNotInitialized) BC250_CODE(hipErrorDeinitialized)
        BC250_CODE(hipErrorInvalidSymbol) BC250_CODE(hipErrorInvalidDevicePointer)
        BC250_CODE(hipErrorInvalidMemcpyDirection) BC250_CODE(hipErrorInvalidDeviceFunction)
        BC250_CODE(hipErrorNoDevice) BC250_CODE(hipErrorInvalidDevice) BC250_CODE(hipErrorInvalidImage)
        BC250_CODE(hipErrorInvalidContext) BC250_CODE(hipErrorFileNotFound)
        BC250_CODE(hipErrorInvalidHandle) BC250_CODE(hipErrorIllegalState) BC250_CODE(hipErrorNotFound)
        BC250_CODE(hipErrorNotReady) BC250_CODE(hipErrorIllegalAddress) BC250_CODE(hipErrorLaunchTimeOut)
        BC250_CODE(hipErrorPeerAccessAlreadyEnabled) BC250_CODE(hipErrorPeerAccessNotEnabled)
        BC250_CODE(hipErrorContextIsDestroyed) BC250_CODE(hipErrorHostMemoryAlreadyRegistered)
        BC250_CODE(hipErrorHostMemoryNotRegistered) BC250_CODE(hipErrorNotSupported)
#undef BC250_CODE
    };
    const std::string unknown_text = get_string(hipErrorUnknown);
    for (const auto& c : codes) {
        const char* name = get_name(c.code);
        expect(name != nullptr && std::strcmp(name, c.name) == 0, "hipGetErrorName",
               std::string(c.name) + " (" + std::to_string(static_cast<int>(c.code)) + ") is named '" +
                   (name ? name : "(null)") + "'");
        const char* text = get_string(c.code);
        expect(text != nullptr && unknown_text != text, "hipGetErrorString",
               std::string(c.name) + " has the text of hipErrorUnknown, '" + unknown_text + "'");
        const char* drv = nullptr;
        const hipError_t drv_err = drv_string(c.code, &drv);
        expect(drv_err == hipSuccess && drv != nullptr && text != nullptr && std::strcmp(drv, text) == 0,
               "hipDrvGetErrorString", std::string(c.name) + " differs from hipGetErrorString");
    }
    if (g_failed != 0) {
        return 1;
    }
    std::printf("PASS: hipModuleLoad gives hipErrorFileNotFound for a missing file and hipErrorInvalidImage "
                "for a 3-byte one; %zu codes have AMD's names and their own texts\n",
                sizeof(codes) / sizeof(codes[0]));
    return 0;
}
