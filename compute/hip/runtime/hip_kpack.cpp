// hip_kpack.cpp - the code objects of HIPK modules, out of .kpack archives (AMD ABI face only).
//
// A ROCm multi-architecture build (TheRock, RFC0008) keeps no device code in the host binary.
// Its fat binary wrapper has the magic HIPK and names msgpack metadata in a .kpackrf section,
// which names an archive such as ../.kpack/torch_gfx1010.kpack beside the binary. rocm_kpack.dll
// (MIT, ROCm/rocm-systems) reads the archive: kpack_load_code_object takes the metadata, the path
// of the binary that holds it, the code object index of the wrapper and a list of processor
// names in priority order, and returns one decompressed code object.
//
// The library is AMD's and is open source; it is loaded from the directory of this DLL on the
// first HIPK load and never from a search path, so that no other copy can answer instead. It
// touches no device: it reads files and decompresses. MEASURED on the nightly 10.3.0a20261030
// archives: the TOC names the processor "gfx1010" exactly, in every archive.

#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "runtime_internal.h"

namespace {

// The part of kpack.h (rocm_kpack, 2025, MIT) that this file calls. kpack_error_t is an enum
// whose KPACK_SUCCESS is 0, and kpack_cache_t is an opaque pointer.
typedef int kpack_error_t;
typedef void* kpack_cache_t;
typedef kpack_error_t (*kpack_cache_create_fn)(kpack_cache_t* cache);
typedef kpack_error_t (*kpack_discover_binary_path_fn)(const void* address_in_binary,
                                                       char* path_out, size_t path_out_size,
                                                       size_t* offset_out);
typedef kpack_error_t (*kpack_load_code_object_fn)(kpack_cache_t cache, const void* hipk_metadata,
                                                   const char* binary_path, uint32_t co_index,
                                                   const char* const* arch_list,
                                                   size_t arch_count, void** code_object_out,
                                                   size_t* code_object_size_out);
typedef void (*kpack_free_code_object_fn)(void* code_object);

// The processors this device runs code of, in the order a code object is preferred: its own,
// the generic family target, and gfx1010 under the target alias of bc250hsa.h.
const char* const kArchList[] = {"gfx1013", "gfx10-1-generic", "gfx1010"};

struct Kpack {
    bool tried = false;
    HMODULE library = nullptr;
    kpack_cache_t cache = nullptr;
    kpack_discover_binary_path_fn discover = nullptr;
    kpack_load_code_object_fn load = nullptr;
    kpack_free_code_object_fn free_object = nullptr;
};

// The process lock is held by every caller, so the record needs no lock of its own.
Kpack g_kpack;

// The directory of this DLL, with a trailing separator.
bool own_directory(wchar_t* out, DWORD out_chars) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&g_kpack), &self)) {
        return false;
    }
    const DWORD n = GetModuleFileNameW(self, out, out_chars);
    if (n == 0 || n >= out_chars) {
        return false;
    }
    for (DWORD i = n; i > 0; --i) {
        if (out[i - 1] == L'\\' || out[i - 1] == L'/') {
            out[i] = L'\0';
            return true;
        }
    }
    return false;
}

bool kpack_open() {
    if (g_kpack.tried) {
        return g_kpack.cache != nullptr;
    }
    g_kpack.tried = true;
    wchar_t path[MAX_PATH + 32];
    if (!own_directory(path, MAX_PATH)) {
        return false;
    }
    wcscat_s(path, L"rocm_kpack.dll");
    g_kpack.library = LoadLibraryExW(path, nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                         LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_kpack.library == nullptr) {
        std::fprintf(stderr, "amdhip64: rocm_kpack.dll is not beside this DLL; the device code of "
                             "HIPK modules cannot be read\n");
        return false;
    }
    const auto create = reinterpret_cast<kpack_cache_create_fn>(
        reinterpret_cast<void*>(GetProcAddress(g_kpack.library, "kpack_cache_create")));
    g_kpack.discover = reinterpret_cast<kpack_discover_binary_path_fn>(
        reinterpret_cast<void*>(GetProcAddress(g_kpack.library, "kpack_discover_binary_path")));
    g_kpack.load = reinterpret_cast<kpack_load_code_object_fn>(
        reinterpret_cast<void*>(GetProcAddress(g_kpack.library, "kpack_load_code_object")));
    g_kpack.free_object = reinterpret_cast<kpack_free_code_object_fn>(
        reinterpret_cast<void*>(GetProcAddress(g_kpack.library, "kpack_free_code_object")));
    if (create == nullptr || g_kpack.discover == nullptr || g_kpack.load == nullptr ||
        g_kpack.free_object == nullptr) {
        std::fprintf(stderr, "amdhip64: rocm_kpack.dll lacks an entry point this runtime calls\n");
        return false;
    }
    const kpack_error_t err = create(&g_kpack.cache);
    if (err != 0 || g_kpack.cache == nullptr) {
        g_kpack.cache = nullptr;
        bc250hip::log_line(BC250HSA_LOG_ERROR, "kpack_cache_create returns %d", err);
        return false;
    }
    return true;
}

}  // namespace

namespace bc250hip {

bc250hsa_status kpack_fetch(Module* module, void** image, size_t* bytes) {
    State& s = state();
    *image = nullptr;
    *bytes = 0;
    if (!kpack_open()) {
        s.face.kpack_library_missing++;
        return BC250HSA_EUNSUPPORTED;
    }
    char binary[MAX_PATH * 2];
    size_t offset = 0;
    kpack_error_t err = g_kpack.discover(module->hipk_metadata, binary, sizeof(binary), &offset);
    if (err != 0) {
        s.face.kpack_refusals++;
        s.face.last_kpack_error = err;
        log_line(BC250HSA_LOG_ERROR, "kpack_discover_binary_path returns %d", err);
        return BC250HSA_ENOTFOUND;
    }
    void* object = nullptr;
    size_t object_bytes = 0;
    err = g_kpack.load(g_kpack.cache, module->hipk_metadata, binary, module->co_index, kArchList,
                       sizeof(kArchList) / sizeof(kArchList[0]), &object, &object_bytes);
    if (err != 0 || object == nullptr) {
        s.face.kpack_refusals++;
        s.face.last_kpack_error = err;
        log_line(BC250HSA_LOG_ERROR,
                 "kpack_load_code_object returns %d for code object %u of %s", err,
                 module->co_index, binary);
        return BC250HSA_ENOTFOUND;
    }
    s.face.kpack_loads++;
    log_line(BC250HSA_LOG_INFO, "kpack code object %u of %s: %zu bytes", module->co_index, binary,
             object_bytes);
    *image = object;
    *bytes = object_bytes;
    return BC250HSA_OK;
}

void kpack_release_image(void* image) {
    if (image != nullptr && g_kpack.free_object != nullptr) {
        g_kpack.free_object(image);
    }
}

}  // namespace bc250hip
