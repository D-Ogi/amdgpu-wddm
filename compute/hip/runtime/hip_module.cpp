// hip_module.cpp - the HIP registration interface that clang's module constructor calls.
//
// Design docs/design/m16-hip-route-b.md section 4.2. MEASURED facts that this file depends on:
//   - the 24 bytes of .hipFatBinSegment are { int magic; int version; void* gpu_binary;
//     void* unused; }, magic 0x48495046 ("HIPF"), version 1, a null fourth field;
//   - __hipRegisterFatBinary receives the address of that wrapper and not the fat binary;
//   - the bundle inside .hip_fatbin starts with "__CLANG_OFFLOAD_BUNDLE__", a 64-bit entry
//     count, and one (offset, size, id length, id) record per entry;
//   - the host entry of a HIP bundle has size 0, and both entries can name the same offset.
//
// The bundle is read here, so that a wrong target is reported at registration. The code object
// is loaded on the first launch, so a process that starts no kernel pays nothing.

#include <cstdio>
#include <cstring>

#include "runtime_internal.h"

namespace {

constexpr uint32_t kHipFatBinMagic = 0x48495046u;  // 'HIPF'
// 'HIPK': the wrapper of a ROCm multi-architecture build (TheRock rocm_kpack). Its third field
// names msgpack metadata in the .kpackrf section, and its fourth field is the index of this
// translation unit's code object inside the .kpack archive. MEASURED on the 212 wrappers of
// torch_hip.dll of torch 2.16.0a0+rocm10.3.0a20261030: all name the same metadata block.
constexpr uint32_t kHipKpackMagic = 0x4B504948u;
constexpr uint32_t kCudaFatBinMagic = 0x466243B1u;
// A compressed offload bundle starts with these four bytes in place of the bundle magic.
constexpr char kCompressedBundleMagic[] = "CCOB";
#if BC250_HIP_AMD_ABI_BUILD
// e_flags & EF_AMDGPU_MACH of a gfx1010 code object (LLVM 23.1.2, llvm/BinaryFormat/ELF.h).
constexpr uint32_t kElfMachMask = 0x0FFu;
constexpr uint32_t kElfMachGfx1010 = 0x033u;
constexpr size_t kElfFlagsOffset = 48;   // e_flags in Elf64_Ehdr
#endif
constexpr char kBundleMagic[] = "__CLANG_OFFLOAD_BUNDLE__";
constexpr size_t kBundleMagicBytes = sizeof(kBundleMagic) - 1;
constexpr uint64_t kMaxBundleEntries = 64;
constexpr uint64_t kMaxBundleBytes = 64ull * 1024ull * 1024ull;

struct FatBinWrapper {
    int32_t  magic;
    int32_t  version;
    const void* gpu_binary;
    const void* unused;
};

uint64_t read_u64(const unsigned char* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// The size of the bundle, from its own header. The wrapper gives a pointer and no length, so
// the reader must find the end itself: it is the largest offset plus size of any entry, and
// never less than the header. The caps keep the result bounded, but they cannot make the walk
// safe: the entry table is read before any cap can refuse it, so a wrapper that names a wrong
// or freed image can send this function up to 64 x (24 + 1024) bytes past the pointer. Only
// clang's own module constructor passes this pointer, and it passes the image of the same
// binary, which is the whole defence we have.
bool bundle_extent(const void* blob, size_t* out_bytes) {
    const unsigned char* p = static_cast<const unsigned char*>(blob);
    if (std::memcmp(p, kBundleMagic, kBundleMagicBytes) != 0) {
        return false;
    }
    const uint64_t count = read_u64(p + kBundleMagicBytes);
    if (count == 0 || count > kMaxBundleEntries) {
        return false;
    }
    uint64_t cursor = kBundleMagicBytes + 8;
    uint64_t end = cursor;
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t offset = read_u64(p + cursor);
        const uint64_t size = read_u64(p + cursor + 8);
        const uint64_t id_bytes = read_u64(p + cursor + 16);
        if (id_bytes > 1024 || offset > kMaxBundleBytes || size > kMaxBundleBytes) {
            return false;
        }
        cursor += 24 + id_bytes;
        if (cursor > end) {
            end = cursor;
        }
        if (offset + size > end) {
            end = offset + size;
        }
    }
    if (end > kMaxBundleBytes) {
        return false;
    }
    *out_bytes = static_cast<size_t>(end);
    return true;
}

#if BC250_HIP_AMD_ABI_BUILD
// The size of an ELF image from its own headers: hipModuleLoadData passes a pointer and no
// length. It is the end of the section header table or of the last segment, whichever is
// later. The same caveat as for bundle_extent applies: the headers are read before anything
// can refuse them, and the caller's pointer is the whole defence.
bool elf_extent(const void* image, size_t* out_bytes) {
    const unsigned char* p = static_cast<const unsigned char*>(image);
    if (std::memcmp(p, "\177ELF", 4) != 0 || p[4] != 2 /* ELFCLASS64 */) {
        return false;
    }
    uint64_t phoff = 0;
    uint64_t shoff = 0;
    uint16_t phentsize = 0;
    uint16_t phnum = 0;
    uint16_t shentsize = 0;
    uint16_t shnum = 0;
    std::memcpy(&phoff, p + 32, 8);
    std::memcpy(&shoff, p + 40, 8);
    std::memcpy(&phentsize, p + 54, 2);
    std::memcpy(&phnum, p + 56, 2);
    std::memcpy(&shentsize, p + 58, 2);
    std::memcpy(&shnum, p + 60, 2);
    uint64_t end = 64;
    if (phoff > kMaxBundleBytes || shoff > kMaxBundleBytes) {
        return false;
    }
    if (shnum != 0) {
        end = shoff + static_cast<uint64_t>(shnum) * shentsize;
    }
    if (phoff + static_cast<uint64_t>(phnum) * phentsize > end) {
        end = phoff + static_cast<uint64_t>(phnum) * phentsize;
    }
    for (uint16_t i = 0; i < phnum && phentsize >= 56; ++i) {
        const unsigned char* ph = p + phoff + static_cast<uint64_t>(i) * phentsize;
        uint64_t offset = 0;
        uint64_t filesz = 0;
        std::memcpy(&offset, ph + 8, 8);
        std::memcpy(&filesz, ph + 32, 8);
        if (offset > kMaxBundleBytes || filesz > kMaxBundleBytes) {
            return false;
        }
        if (offset + filesz > end) {
            end = offset + filesz;
        }
    }
    if (end > kMaxBundleBytes) {
        return false;
    }
    *out_bytes = static_cast<size_t>(end);
    return true;
}
#endif

void report(const char* message) {
    // One line on the error stream, and nothing else: a registration failure happens before
    // main() and a HIP program has no way to ask about it.
    std::fprintf(stderr, "amdhip64: %s\n", message);
}

}  // namespace

namespace bc250hip {

bc250hsa_status module_load_image(Module* module, const void* image, size_t bytes) {
    bc250hsa_device* dev = nullptr;
    if (device(&dev) != hipSuccess) {
        const State& s = state();
        return s.open_status == BC250HSA_OK ? BC250HSA_ENODEV : s.open_status;
    }
    bool gfx1010 = false;
#if BC250_HIP_AMD_ABI_BUILD
    // The ROCm user libraries ship gfx1010 code and no gfx1013 code (bc250hsa.h, the target
    // alias). Only this face loads them, so only this face turns the alias on.
    bc250hsa_target_alias_set(BC250HSA_TARGET_ALIAS_GFX1010);
    if (image != nullptr && bytes >= kElfFlagsOffset + 4) {
        uint32_t flags = 0;
        std::memcpy(&flags, static_cast<const unsigned char*>(image) + kElfFlagsOffset,
                    sizeof(flags));
        gfx1010 = (flags & kElfMachMask) == kElfMachGfx1010;
    }
#endif
    const bc250hsa_status status = bc250hsa_module_load(dev, image, bytes, &module->loaded);
    if (status != BC250HSA_OK) {
        module->loaded = nullptr;
        return status;
    }
    if (gfx1010) {
        state().face.alias_loads++;
    }
    return BC250HSA_OK;
}

bc250hsa_status module_ensure_loaded(Module* module) {
    if (module->load_tried) {
        return module->load_status;
    }
    module->load_tried = true;
    if (module->bundle_status != BC250HSA_OK) {
        module->load_status = module->bundle_status;
        return module->load_status;
    }
    bc250hsa_device* dev = nullptr;
    if (device(&dev) != hipSuccess) {
        State& s = state();
        module->load_tried = false;  // a later call may find a device
        return s.open_status == BC250HSA_OK ? BC250HSA_ENODEV : s.open_status;
    }
    if (module->kind == ModuleKind::kKpack) {
#if !BC250_HIP_AMD_ABI_BUILD
        // Unreachable: only the AMD face registers a HIPK wrapper.
        module->load_status = BC250HSA_EUNSUPPORTED;
        return module->load_status;
#else
        void* image = nullptr;
        size_t bytes = 0;
        module->load_status = kpack_fetch(module, &image, &bytes);
        if (module->load_status == BC250HSA_OK) {
            // The loader copies what it needs and keeps no pointer into the image.
            module->load_status = module_load_image(module, image, bytes);
            kpack_release_image(image);
        }
#endif
    } else {
        module->load_status = module_load_image(module, module->image, module->image_bytes);
    }
    if (module->load_status != BC250HSA_OK) {
        return module->load_status;
    }
    // Resolve every device global that clang registered for this module.
    for (Var& var : module->vars) {
        uint64_t va = 0;
        uint64_t bytes = 0;
        if (bc250hsa_module_symbol(module->loaded, var.device_name.c_str(), &va, &bytes) ==
            BC250HSA_OK) {
            var.va = va;
            var.resolved = true;
        }
    }
    return BC250HSA_OK;
}

#if BC250_HIP_AMD_ABI_BUILD
hipError_t program_module_load(const void* image, size_t bytes, Module** out) {
    State& s = state();
    *out = nullptr;
    if (image == nullptr) {
        return hipErrorInvalidValue;
    }
    const unsigned char* p = static_cast<const unsigned char*>(image);
    const void* elf = nullptr;
    size_t elf_bytes = 0;
    bc250hsa_status status = BC250HSA_OK;
    if (static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24)) ==
        kHipFatBinMagic) {
        // A fat binary wrapper: the bundle is behind its third field.
        FatBinWrapper wrapper;
        std::memcpy(&wrapper, image, sizeof(wrapper));
        p = static_cast<const unsigned char*>(wrapper.gpu_binary);
        bytes = 0;
        if (p == nullptr) {
            return hipErrorInvalidImage;
        }
    }
    if (std::memcmp(p, "\177ELF", 4) == 0) {
        elf = p;
        elf_bytes = bytes;
        if (elf_bytes == 0 && !elf_extent(p, &elf_bytes)) {
            s.face.program_refusals++;
            return hipErrorInvalidImage;
        }
    } else if (std::memcmp(p, kCompressedBundleMagic, 4) == 0) {
        s.face.compressed_refusals++;
        s.face.program_refusals++;
        log_line(BC250HSA_LOG_ERROR, "hipModuleLoadData refuses a compressed offload bundle (CCOB)");
        return hipErrorInvalidImage;
    } else {
        size_t bundle_bytes = 0;
        if (!bundle_extent(p, &bundle_bytes)) {
            s.face.program_refusals++;
            return hipErrorInvalidImage;
        }
        status = bc250hsa_unbundle(p, bundle_bytes, nullptr, &elf, &elf_bytes);
        if (status == BC250HSA_ENOTFOUND) {
            status = bc250hsa_unbundle(p, bundle_bytes, "gfx1010", &elf, &elf_bytes);
            if (status == BC250HSA_OK) {
                s.face.bundle_alias_picks++;
            }
        }
        if (status != BC250HSA_OK) {
            s.face.program_refusals++;
            log_line(BC250HSA_LOG_ERROR, "hipModuleLoadData: the bundle holds no code object for "
                                         "this device (%s)", bc250hsa_status_string(status));
            return kErrorNoBinaryForGpu;
        }
    }
    Module* module = new Module();
    module->kind = ModuleKind::kProgram;
    module->load_tried = true;
    module->load_status = module_load_image(module, elf, elf_bytes);
    if (module->load_status != BC250HSA_OK) {
        status = module->load_status;
        delete module;
        s.face.program_refusals++;
        log_line(BC250HSA_LOG_ERROR, "hipModuleLoadData: the code object does not load (%s)",
                 bc250hsa_status_string(status));
        return status == BC250HSA_EWRONGTARGET ? kErrorNoBinaryForGpu : translate(status);
    }
    s.modules.push_back(module);
    s.face.program_loads++;
    *out = module;
    return hipSuccess;
}
#endif

}  // namespace bc250hip

using bc250hip::Function;
using bc250hip::Module;
using bc250hip::state;
using bc250hip::Var;

extern "C" {

void** __hipRegisterFatBinary(const void* data) {
    if (data == nullptr) {
        report("__hipRegisterFatBinary received a null wrapper");
        return nullptr;
    }
    FatBinWrapper wrapper;
    std::memcpy(&wrapper, data, sizeof(wrapper));
    if (static_cast<uint32_t>(wrapper.magic) == kCudaFatBinMagic) {
        report("this is a CUDA fat binary, not a HIP one; compile with clang -x hip");
        return nullptr;
    }
    const bool kpack = static_cast<uint32_t>(wrapper.magic) == kHipKpackMagic;
    if (kpack && !BC250_HIP_AMD_ABI_BUILD) {
        report("this is a HIPK wrapper of a ROCm multi-architecture build, which only "
               "amdhip64_7.dll loads");
        return nullptr;
    }
    if (!kpack && static_cast<uint32_t>(wrapper.magic) != kHipFatBinMagic) {
        report("the fat binary wrapper has no HIPF magic");
        return nullptr;
    }
    if (wrapper.gpu_binary == nullptr) {
        report("the fat binary wrapper names no device image");
        return nullptr;
    }

    bc250hip::Guard guard;
    bc250hip::State& s = state();
    const auto known = s.module_by_wrapper.find(data);
    if (known != s.module_by_wrapper.end()) {
        // Clang's module constructor registers one time per process, because it guards on the
        // null handle. A shared library of its own brings its own wrapper, so this path only
        // runs when the same wrapper arrives twice.
        known->second->registrations++;
        return reinterpret_cast<void**>(known->second);
    }

    Module* module = new Module();
    module->wrapper = data;
    module->registrations = 1;

    size_t bundle_bytes = 0;
    if (kpack) {
        // The archive is read on the first launch of a kernel of this module (hip_kpack.cpp):
        // torch_hip.dll alone registers 212 of them, and a process that starts few kernels
        // should not decompress them all before main().
        module->kind = bc250hip::ModuleKind::kKpack;
        module->hipk_metadata = wrapper.gpu_binary;
        module->co_index = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(wrapper.unused));
        s.face.kpack_modules++;
    } else if (BC250_HIP_AMD_ABI_BUILD &&
               std::memcmp(wrapper.gpu_binary, kCompressedBundleMagic, 4) == 0) {
        module->bundle_status = BC250HSA_ECOMPRESSEDBUNDLE;
        s.face.compressed_refusals++;
        report("the device image is a compressed offload bundle (CCOB), which this build does "
               "not read");
    } else if (!bundle_extent(wrapper.gpu_binary, &bundle_bytes)) {
        module->bundle_status = BC250HSA_EBADBUNDLE;
        report("the device image is not a clang offload bundle");
    } else {
        module->bundle_status = bc250hsa_unbundle(wrapper.gpu_binary, bundle_bytes, nullptr,
                                                  &module->image, &module->image_bytes);
#if BC250_HIP_AMD_ABI_BUILD
        // A ROCm library built for gfx1010 has no gfx1013 entry: take the gfx1010 one, which
        // the target alias then loads (bc250hsa.h).
        if (module->bundle_status == BC250HSA_ENOTFOUND) {
            module->bundle_status = bc250hsa_unbundle(wrapper.gpu_binary, bundle_bytes, "gfx1010",
                                                      &module->image, &module->image_bytes);
            if (module->bundle_status == BC250HSA_OK) {
                s.face.bundle_alias_picks++;
            }
        }
#endif
        if (module->bundle_status != BC250HSA_OK) {
            char line[160];
            std::snprintf(line, sizeof(line), "the offload bundle holds no usable gfx1013 code object (%s)",
                          bc250hsa_status_string(module->bundle_status));
            report(line);
        }
    }

    s.modules.push_back(module);
    s.module_by_wrapper.emplace(data, module);
    return reinterpret_cast<void**>(module);
}

void __hipUnregisterFatBinary(void** modules) {
    if (modules == nullptr) {
        return;
    }
    bc250hip::Guard guard;
    bc250hip::State& s = state();
    Module* module = reinterpret_cast<Module*>(modules);
    size_t at = s.modules.size();
    for (size_t i = 0; i < s.modules.size(); ++i) {
        if (s.modules[i] == module) {
            at = i;
            break;
        }
    }
    if (at == s.modules.size() || module->kind == bc250hip::ModuleKind::kProgram) {
        return;   // hipModuleUnload owns a program module
    }
    if (module->registrations > 1) {
        // The same wrapper is still registered. Take one registration away and keep the module
        // and its kernels: one unregister must not destroy what another registration uses.
        module->registrations--;
        return;
    }
    module->registrations = 0;
    s.modules.erase(s.modules.begin() + static_cast<ptrdiff_t>(at));
    if (module->wrapper != nullptr) {
        s.module_by_wrapper.erase(module->wrapper);
    }
    for (auto it = s.functions.begin(); it != s.functions.end();) {
        it = it->second.module == module ? s.functions.erase(it) : std::next(it);
    }
    if (module->loaded != nullptr) {
        bc250hsa_module_unload(module->loaded);
    }
    delete module;
}

int __hipRegisterFunction(void** modules, const void* hostFunction, char* deviceFunction,
                          const char* deviceName, int threadLimit, void* tid, void* bid,
                          void* blockDim, void* gridDim, int* workgroupSizeHint) {
    // MEASURED: clang passes the host stub, the device name twice, -1 and six null pointers.
    (void)deviceFunction;
    (void)threadLimit;
    (void)tid;
    (void)bid;
    (void)blockDim;
    (void)gridDim;
    (void)workgroupSizeHint;
    if (modules == nullptr || hostFunction == nullptr || deviceName == nullptr) {
        return 1;
    }
    bc250hip::Guard guard;
    Function entry;
    entry.module = reinterpret_cast<Module*>(modules);
    entry.device_name = deviceName;
    entry.kernel = nullptr;
    state().functions[hostFunction] = entry;
    return 0;
}

void __hipRegisterVar(void** modules, void* hostVar, char* deviceVar, const char* deviceName,
                      int isExtern, size_t size, int constant, int global) {
    (void)deviceVar;
    (void)isExtern;
    (void)constant;
    (void)global;
    if (modules == nullptr || deviceName == nullptr) {
        return;
    }
    bc250hip::Guard guard;
    Module* module = reinterpret_cast<Module*>(modules);
    Var var;
    var.host_var = hostVar;
    var.device_name = deviceName;
    var.bytes = size;
    module->vars.push_back(var);
}

void __hipRegisterManagedVar(void** modules, void* pointer, void* initValue, const char* name,
                             size_t size, unsigned align) {
    // Managed memory needs a page migration service, which this build does not have. The stub
    // reports the missing capability instead of pretending.
    (void)modules;
    (void)pointer;
    (void)initValue;
    (void)size;
    (void)align;
    char line[160];
    std::snprintf(line, sizeof(line), "managed variable '%s' needs managed memory, which this build does not support",
                  name != nullptr ? name : "?");
    report(line);
    bc250hip::log_line(BC250HSA_LOG_ERROR, "__hipRegisterManagedVar refuses variable '%s':"
                                           " managed memory needs page migration",
                       name != nullptr ? name : "?");
    bc250hip::last_error_set(hipErrorNotSupported);
}

}  // extern "C"
