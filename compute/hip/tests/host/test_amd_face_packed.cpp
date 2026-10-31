// test_amd_face_packed.cpp - a packed argument buffer that is shorter than the kernel's arguments.
//
// rocBLAS launches its Tensile kernels with hipModuleLaunchKernel and the `extra` form
// { HIP_LAUNCH_PARAM_BUFFER_POINTER, buffer, HIP_LAUNCH_PARAM_BUFFER_SIZE, &size, END }. For the
// fp16 kernel Cijk_Ailk_Bjlk_HHS_BH_MT64x1x64_..._WGM1 it gives 96 bytes, and the metadata
// declares explicit arguments that end at 100 (kit3 of the PyTorch route, the VAE decode of
// ComfyUI on the mock DLL). AMD's runtime gives an argument that ends after the end of the buffer
// the value zero, all of its bytes (KernelParameters::captureHIPArgs, ROCm clr
// rocclr/platform/kernel.cpp), so rocBLAS works there. This runtime refused the launch.
//
// The test loads the amdhip64_7.dll that argv[1] names (the mock DLL of build-amd-abi.ps1) with
// BC250_HIP_MOCK_RECORD set, so that the mock writes every dispatch and its kernel argument bytes
// to a file. A child process (the same program with --child) makes the launches, because the
// mock holds the file without sharing until its process ends; the parent then reads it. It loads the code object argv[2] (m16_kernels.gfx1010.co: vadd has 4 explicit
// arguments, a, b and c at 0, 8 and 16 and n at 24, 28 bytes, and no hidden ones), launches
// vadd four times through hipModuleLaunchKernel with a packed buffer whose n is 0x01020304, and
// fails when:
//   - the buffer of 28 bytes does not give n = 0x01020304;
//   - the buffer of 24 bytes (n after its end) is refused, or does not give n = 0, or gives other
//     pointer bytes than the buffer of 28;
//   - the buffer of 26 bytes (n half inside) is refused, or gives other bytes than 0 for n;
//   - the buffer of 0 bytes is not refused with hipErrorInvalidValue (AMD's runtime refuses a
//     kernel with arguments and no buffer).
//
// Compiled against AMD's hip_runtime_api.h, as amd_abi.cpp is: the names and numbers are AMD's.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <hip/hip_runtime_api.h>

using ModuleLoadData = hipError_t (*)(hipModule_t*, const void*);
using ModuleGetFunction = hipError_t (*)(hipFunction_t*, hipModule_t, const char*);
using ModuleLaunch = hipError_t (*)(hipFunction_t, unsigned, unsigned, unsigned, unsigned, unsigned,
                                    unsigned, unsigned, hipStream_t, void**, void**);
using Malloc = hipError_t (*)(void**, size_t);
using Free = hipError_t (*)(void*);
using Sync = hipError_t (*)();
using GetLastError_ = hipError_t (*)();

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

std::vector<unsigned char> read_all(const wchar_t* path) {
    std::vector<unsigned char> out;
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) {
        return out;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 0) {
        out.resize(static_cast<size_t>(size));
        if (std::fread(out.data(), 1u, out.size(), f) != out.size()) {
            out.clear();
        }
    }
    std::fclose(f);
    return out;
}

// The kernel argument bytes of each vadd dispatch of the record, by its grid width (the test
// gives each launch its own width). A line: "dispatch kernel=vadd grid=W,1,1 ... kernarg=N
// bytes=<hex>"; the hex is the last " bytes=" of the line.
std::map<unsigned, std::vector<unsigned char>> vadd_dispatches(const std::string& path) {
    std::map<unsigned, std::vector<unsigned char>> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("dispatch kernel=vadd grid=", 0) != 0) {
            continue;
        }
        unsigned width = static_cast<unsigned>(std::strtoul(line.c_str() + 26, nullptr, 10));
        if (width >= 256u && width % 256u == 0u) {
            width /= 256u;   // a record that counts work items, not work groups
        }
        const size_t at = line.rfind(" bytes=");
        if (at == std::string::npos) {
            continue;
        }
        std::vector<unsigned char> bytes;
        for (size_t i = at + 7; i + 1 < line.size(); i += 2) {
            bytes.push_back(static_cast<unsigned char>(std::stoul(line.substr(i, 2), nullptr, 16)));
        }
        out[width] = bytes;
    }
    return out;
}

uint32_t u32_at(const std::vector<unsigned char>& b, size_t at) {
    uint32_t v = 0xFFFFFFFFu;
    if (b.size() >= at + 4u) {
        std::memcpy(&v, b.data() + at, 4u);
    }
    return v;
}

std::string hex32(uint32_t v) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", v);
    return text;
}

}  // namespace

const uint32_t kN = 0x01020304u;

// The child process: the launches. The mock opens its record with fopen_s, which shares the file
// with no other writer or reader, so the parent reads the record after this process has ended.
int run_launches(const wchar_t* dll_path, const wchar_t* image_path) {
    const std::vector<unsigned char> image = read_all(image_path);
    if (image.empty()) {
        std::printf("FAIL: cannot read the code object\n");
        return 1;
    }
    HMODULE dll = LoadLibraryExW(dll_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (dll == nullptr) {
        std::printf("FAIL: LoadLibrary error %lu\n", GetLastError());
        return 1;
    }
    auto load = reinterpret_cast<ModuleLoadData>(GetProcAddress(dll, "hipModuleLoadData"));
    auto get_function = reinterpret_cast<ModuleGetFunction>(GetProcAddress(dll, "hipModuleGetFunction"));
    auto launch = reinterpret_cast<ModuleLaunch>(GetProcAddress(dll, "hipModuleLaunchKernel"));
    auto malloc_ = reinterpret_cast<Malloc>(GetProcAddress(dll, "hipMalloc"));
    auto free_ = reinterpret_cast<Free>(GetProcAddress(dll, "hipFree"));
    auto sync = reinterpret_cast<Sync>(GetProcAddress(dll, "hipDeviceSynchronize"));
    auto last_error = reinterpret_cast<GetLastError_>(GetProcAddress(dll, "hipGetLastError"));
    if (!load || !get_function || !launch || !malloc_ || !free_ || !sync || !last_error) {
        std::printf("FAIL: an entry point of the test is not exported\n");
        return 1;
    }

    hipModule_t module = nullptr;
    hipError_t err = load(&module, image.data());
    if (err != hipSuccess) {
        std::printf("FAIL: hipModuleLoadData returned %d\n", static_cast<int>(err));
        return 1;
    }
    hipFunction_t vadd = nullptr;
    err = get_function(&vadd, module, "vadd");
    if (err != hipSuccess) {
        std::printf("FAIL: hipModuleGetFunction vadd returned %d\n", static_cast<int>(err));
        return 1;
    }
    void* buffers[3] = {};
    for (void*& b : buffers) {
        if (malloc_(&b, 1024u) != hipSuccess) {
            std::printf("FAIL: hipMalloc\n");
            return 1;
        }
    }
    // The packed buffer, laid out at the metadata offsets of vadd; 32 bytes of memory in every
    // case, so that a copy of n past the given size would find 0x01020304 there.
    alignas(8) unsigned char packed[32] = {};
    std::memcpy(packed + 0, &buffers[0], 8u);
    std::memcpy(packed + 8, &buffers[1], 8u);
    std::memcpy(packed + 16, &buffers[2], 8u);
    const uint32_t n = kN;
    std::memcpy(packed + 24, &n, 4u);

    const struct {
        size_t bytes;
        unsigned width;   // the grid width that names the dispatch in the record
    } cases[] = { {28u, 1u}, {24u, 2u}, {26u, 3u} };
    for (const auto& c : cases) {
        size_t size = c.bytes;
        void* extra[] = { HIP_LAUNCH_PARAM_BUFFER_POINTER, packed, HIP_LAUNCH_PARAM_BUFFER_SIZE, &size,
                          HIP_LAUNCH_PARAM_END };
        err = launch(vadd, c.width, 1u, 1u, 256u, 1u, 1u, 0u, nullptr, nullptr, extra);
        expect(err == hipSuccess, "hipModuleLaunchKernel with a packed buffer",
               std::to_string(c.bytes) + " bytes returned " + std::to_string(static_cast<int>(err)));
        (void)last_error();
    }
    {
        size_t size = 0u;
        void* extra[] = { HIP_LAUNCH_PARAM_BUFFER_POINTER, packed, HIP_LAUNCH_PARAM_BUFFER_SIZE, &size,
                          HIP_LAUNCH_PARAM_END };
        err = launch(vadd, 4u, 1u, 1u, 256u, 1u, 1u, 0u, nullptr, nullptr, extra);
        expect(err == hipErrorInvalidValue, "hipModuleLaunchKernel with a packed buffer of 0 bytes",
               "returned " + std::to_string(static_cast<int>(err)) + ", AMD's runtime refuses it (1)");
        (void)last_error();
    }
    expect(sync() == hipSuccess, "hipDeviceSynchronize", "failed");
    for (void* b : buffers) {
        (void)free_(b);
    }
    return g_failed != 0 ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wcscmp(argv[3], L"--child") == 0) {
        return run_launches(argv[1], argv[2]);
    }
    if (argc != 3) {
        std::fprintf(stderr, "usage: test_amd_face_packed <amdhip64_7.dll> <m16_kernels.gfx1010.co>\n");
        return 2;
    }
    // The child inherits the variable; the C runtime of the DLL copies the environment at its start.
    const std::string record = temp_path("bc250-face-packed-record.txt");
    std::remove(record.c_str());
    SetEnvironmentVariableA("BC250_HIP_MOCK_RECORD", record.c_str());
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" \"" + argv[1] + L"\" \"" + argv[2] + L"\" --child";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::printf("FAIL: CreateProcess error %lu\n", GetLastError());
        return 1;
    }
    DWORD child = 1;
    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 10000);
        expect(false, "the child process", "did not end in 60 s");
    }
    GetExitCodeProcess(pi.hProcess, &child);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    expect(child == 0, "the launches of the child process", "exit " + std::to_string(child));

    const auto seen = vadd_dispatches(record);
    const auto full = seen.find(1u);
    expect(full != seen.end() && full->second.size() >= 28u, "the dispatch of 28 bytes",
           "is not in the record " + record);
    if (full != seen.end() && full->second.size() >= 28u) {
        expect(u32_at(full->second, 24u) == kN, "n of the buffer of 28 bytes",
               hex32(u32_at(full->second, 24u)) + ", want " + hex32(kN));
        for (unsigned width : {2u, 3u}) {
            const auto got = seen.find(width);
            const std::string name = width == 2u ? "the buffer of 24 bytes" : "the buffer of 26 bytes";
            expect(got != seen.end() && got->second.size() >= 28u, name.c_str(), "has no dispatch in the record");
            if (got == seen.end() || got->second.size() < 28u) {
                continue;
            }
            expect(std::memcmp(got->second.data(), full->second.data(), 24u) == 0, name.c_str(),
                   "the pointer bytes differ from the buffer of 28 bytes");
            expect(u32_at(got->second, 24u) == 0u, name.c_str(),
                   "n is " + hex32(u32_at(got->second, 24u)) + ", want 0 (the argument ends after the buffer)");
        }
    }
    expect(seen.find(4u) == seen.end(), "the buffer of 0 bytes", "reached a dispatch");
    std::remove(record.c_str());
    if (g_failed != 0) {
        return 1;
    }
    std::printf("PASS: a packed buffer that ends before an argument ends gives that argument 0 "
                "(24 and 26 bytes of 28), a full one its value, an empty one hipErrorInvalidValue\n");
    return 0;
}