#include "runtime_internal.h"
#include "../../../driver/shim/include/bc250_hip_journal.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL CHECK %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)
#if defined(_MSC_VER)
__declspec(noinline)
#endif
static uint64_t copy_baseline(const unsigned char* bytes, size_t size) {
    std::vector<unsigned char> copy(size);
    std::memcpy(copy.data(), bytes, size);
    return copy.front() + copy.back() + copy.size();
}
int main() {
    unsigned char packed[128]{};
    uint64_t values[] = {0x1000, 0x1020, 0, 0x2000, 0x1000};
    std::memcpy(packed, values, sizeof(values));
    for (unsigned i = 64; i < sizeof(packed); ++i) packed[i] = static_cast<unsigned char>(i);
    bc250hsa_arg args[5]{};
    for (unsigned i = 0; i < 5; ++i) {
        args[i].offset = i * 8; args[i].size = 8;
        args[i].kind = static_cast<uint16_t>(i == 4 ? BC250HSA_ARG_BY_VALUE : BC250HSA_ARG_GLOBAL_BUFFER);
    }
    bc250hsa_kernel kernel{};
    kernel.name = "known_symbol"; kernel.entry_va = 0x10000; kernel.descriptor_va = 0x11000;
    kernel.args = args; kernel.arg_count = 5;
    bc250hsa_dispatch dispatch{};
    dispatch.kernel = &kernel; dispatch.kernarg_va = 0x12000;
    dispatch.launch.grid[0] = 7; dispatch.launch.grid[1] = 3; dispatch.launch.grid[2] = 2;
    dispatch.launch.block[0] = 64; dispatch.launch.block[1] = 2; dispatch.launch.block[2] = 1;
    dispatch.launch.dynamic_group_bytes = 512;
    std::map<uint64_t, bc250hip::Allocation> allocations;
    allocations[0x1000].mem.va = 0x1000; allocations[0x1000].mem.bytes = 0x100;
    std::vector<unsigned char> output;
    auto prepare = [&]() { return bc250hip::prepare_dispatch_record(dispatch, packed, sizeof(packed), 1, allocations, output); };
    CHECK(prepare() == hipSuccess);
    BC250_HIP_DISPATCH_RECORD record{};
    std::memcpy(&record, output.data(), sizeof(record));
    CHECK(Bc250HipRecordValid(output.data(), static_cast<uint32_t>(output.size())));
    CHECK(record.BindingCount == 4); // Scalar with identical bits is excluded.
    CHECK(record.EntryVa == kernel.entry_va && record.DescriptorVa == kernel.descriptor_va);
    CHECK(record.KernargVa == dispatch.kernarg_va && record.KernargBytes == sizeof(packed));
    CHECK(!std::memcmp(output.data() + record.KernargOffset, packed, sizeof(packed)));
    CHECK(!std::memcmp(record.Grid, dispatch.launch.grid, sizeof(record.Grid)));
    CHECK(!std::memcmp(record.Block, dispatch.launch.block, sizeof(record.Block)));
    CHECK(record.DynamicLdsBytes == 512 && record.DispatchId == 1);
    CHECK(!std::strcmp(reinterpret_cast<const char*>(output.data() + record.SymbolOffset), kernel.name));
    BC250_HIP_POINTER_BINDING binding[4];
    std::memcpy(binding, output.data() + record.BindingsOffset, sizeof(binding));
    CHECK(binding[0].Flags == 1 && binding[0].Base == 0x1000 && binding[0].Bytes == 0x100);
    CHECK(binding[1].Flags == 1 && binding[1].Value == 0x1020);
    CHECK(binding[2].Flags == 0 && binding[2].Value == 0 && binding[2].Bytes == 0);
    CHECK(binding[3].Flags == 0 && binding[3].Value == 0x2000 && binding[3].Base == 0);
    packed[127] ^= 1;
    CHECK(output[record.KernargOffset + 127] != packed[127]); // Immutable, including packet tail.
    args[0].offset = 125;
    CHECK(prepare() == hipErrorNotSupported && output.empty());
    args[0].offset = 0; args[0].size = 4;
    CHECK(prepare() == hipErrorNotSupported && output.empty());
    args[0].size = 8;
    std::string oversized(BC250_HIP_JOURNAL_MAX_SYMBOL, 'a'); kernel.name = oversized.c_str();
    CHECK(prepare() == hipErrorNotSupported && output.empty());
    kernel.name = "known_symbol";
    CHECK(bc250hip::prepare_dispatch_record(dispatch, packed, BC250_HIP_JOURNAL_MAX_KERNARG + 1,
                                           1, allocations, output) == hipErrorNotSupported);
    CHECK(bc250hip::prepare_dispatch_record(dispatch, packed, sizeof(packed), 0, allocations, output) == hipErrorNotSupported);
    allocations[0x1000].mem.bytes = UINT64_MAX;
    CHECK(prepare() == hipSuccess);
    std::memcpy(&record, output.data(), sizeof(record));
    std::memcpy(binding, output.data() + record.BindingsOffset, sizeof(binding));
    CHECK(binding[0].Flags == 0); // Wrapped interval is never claimed known.
    allocations[0x1000].mem.bytes = 0x100;
    std::string longest(BC250_HIP_JOURNAL_MAX_SYMBOL - 1, 'x');
    kernel.name = longest.c_str();
    CHECK(prepare() == hipSuccess);
    CHECK(Bc250HipRecordValid(output.data(), static_cast<uint32_t>(output.size())));
    kernel.name = "known_symbol";
    std::vector<bc250hsa_arg> many(BC250_HIP_JOURNAL_MAX_BINDINGS + 1, args[0]);
    kernel.args = many.data(); kernel.arg_count = static_cast<uint32_t>(many.size());
    CHECK(prepare() == hipErrorNotSupported && output.empty());
    kernel.args = args; kernel.arg_count = 5;
    uint64_t one_past = 0x1100;
    std::memcpy(packed, &one_past, sizeof(one_past));
    CHECK(prepare() == hipSuccess);
    std::memcpy(&record, output.data(), sizeof(record));
    std::memcpy(binding, output.data() + record.BindingsOffset, sizeof(binding));
    CHECK(binding[0].Flags == 0 && binding[0].Base == 0 && binding[0].Bytes == 0);
    std::memcpy(packed, values, sizeof(values));
    constexpr unsigned repeats = 50000;
    uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < repeats; ++i) {
        std::vector<unsigned char> fresh;
        if (bc250hip::prepare_dispatch_record(dispatch, packed, sizeof(packed), i + 1, allocations, fresh) != hipSuccess) return 2;
        checksum += fresh.size() + fresh.back();
    }
    const auto end = std::chrono::steady_clock::now();
    std::printf("packing benchmark: %u iterations, %.3f us/record, checksum %llu; host packing/allocation only, no KMT or GPU\n",
        repeats, std::chrono::duration<double, std::micro>(end - start).count() / repeats,
        static_cast<unsigned long long>(checksum));
    const auto copy_start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < repeats; ++i) checksum += copy_baseline(packed, sizeof(packed));
    const auto copy_end = std::chrono::steady_clock::now();
    std::printf("copy/allocation reference: %u iterations, %.3f us/128-byte copy, checksum %llu; not a pre-journal GPU benchmark\n",
        repeats, std::chrono::duration<double, std::micro>(copy_end - copy_start).count() / repeats,
        static_cast<unsigned long long>(checksum));
    std::printf("journal packing: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
