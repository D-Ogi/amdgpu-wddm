/* Allocation-only diagnostic. Never submits shaders or accesses allocation data. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bc250hsa.h"

#define MAX_BYTES (UINT64_C(7) << 30)

static int parse_size(const char *s, uint64_t *out)
{
    uint64_t n = 0;
    unsigned base = 10, digits = 0;
    if (!s || !out) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    for (; *s; ++s) {
        unsigned d;
        if (*s >= '0' && *s <= '9') d = (unsigned)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') d = (unsigned)(*s - 'a') + 10;
        else if (*s >= 'A' && *s <= 'F') d = (unsigned)(*s - 'A') + 10;
        else return 0;
        if (d >= base || n > (MAX_BYTES - d) / base) return 0;
        n = n * base + d;
        ++digits;
    }
    if (!digits || !n || n > MAX_BYTES) return 0;
    *out = n;
    return 1;
}

static int selftest(void)
{
    static const struct { const char *s; uint64_t expected; } cases[] = {
        {"1", 1}, {"6960120064", UINT64_C(6960120064)},
        {"7516192768", MAX_BYTES}, {"0x1C0000000", MAX_BYTES},
        {"0Xff", 255}, {"00016", 16}, {"4294967296", UINT64_C(4294967296)},
        {"", 0}, {"0", 0}, {"0x", 0}, {"0x0", 0}, {"-1", 0}, {"+1", 0},
        {" 1", 0}, {"1 ", 0}, {"1\n", 0}, {"1GiB", 0}, {"1.0", 0},
        {"0xg", 0}, {"0x1C0000001", 0}, {"7516192769", 0},
        {"18446744073709551615", 0}, {"18446744073709551616", 0}
    };
    unsigned i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint64_t n = UINT64_MAX;
        int ok = parse_size(cases[i].s, &n);
        if (ok != (cases[i].expected != 0) || (ok && n != cases[i].expected)) {
            fprintf(stderr, "FAIL parser case %u\n", i); return 1;
        }
    }
    printf("PASS %u parser cases; no device opened\n", i);
    return 0;
}

static long long qpc(void)
{
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return n.QuadPart;
}

static void log_line(void *ctx, uint32_t level, const char *message)
{
    (void)ctx;
    fprintf(stderr, "qpc=%lld layer1_level=%u %s\n", qpc(), level, message);
    fflush(stderr);
}

static void report(const char *stage, bc250hsa_status status, int32_t os,
                   uint64_t requested, uint32_t flags, const bc250hsa_mem *mem)
{
    printf("stage=%s qpc=%lld status=%d status_name=%s os_valid=%u os_status=0x%08X "
           "requested=%llu flags=0x%X bytes=%llu va=0x%llX\n", stage, qpc(),
           (int)status, bc250hsa_status_string(status), status == BC250HSA_EOS ? 1u : 0u,
           (unsigned)os, (unsigned long long)requested, flags,
           (unsigned long long)mem->bytes, (unsigned long long)mem->va);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    bc250hsa_device *dev = NULL;
    bc250hsa_mem mem = {0};
    bc250hsa_open_params params = {0};
    bc250hsa_status status;
    LARGE_INTEGER frequency;
    uint64_t bytes;
    uint32_t flags;
    int32_t os;
    int result = 0;
    if (argc == 2 && strcmp(argv[1], "--selftest") == 0) return selftest();
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("allocprobe --bytes <decimal|0xhex> --mode <mappable|device>\n"
             "One allocation, 1 byte..7 GiB. Mappable also tests map/unmap. No memory access or GPU submission.");
        return 0;
    }
    if (argc != 5 || strcmp(argv[1], "--bytes") || strcmp(argv[3], "--mode") ||
        !parse_size(argv[2], &bytes) ||
        (strcmp(argv[4], "mappable") && strcmp(argv[4], "device"))) {
        fputs("Invalid arguments; use --help.\n", stderr); return 2;
    }
    flags = strcmp(argv[4], "mappable") == 0 ? BC250HSA_MEM_MAPPABLE : BC250HSA_MEM_DEVICE;
    QueryPerformanceFrequency(&frequency);
    printf("qpc_frequency=%lld requested=%llu flags=0x%X open_flags=0x%X\n",
           frequency.QuadPart, (unsigned long long)bytes, flags, BC250HSA_OPEN_NO_GPU_SUBMIT);
    bc250hsa_set_log(log_line, NULL, BC250HSA_LOG_TRACE);
    params.struct_bytes = sizeof(params);
    params.flags = BC250HSA_OPEN_NO_GPU_SUBMIT;
    report("open_begin", BC250HSA_OK, 0, bytes, flags, &mem);
    status = bc250hsa_open(&params, &dev);
    os = status == BC250HSA_EOS ? bc250hsa_last_os_status() : 0;
    report("open_end", status, os, bytes, flags, &mem);
    if (status != BC250HSA_OK) { result = 1; goto cleanup; }
    report("alloc_begin", BC250HSA_OK, 0, bytes, flags, &mem);
    status = bc250hsa_alloc(dev, bytes, 0, flags, &mem);
    os = status == BC250HSA_EOS ? bc250hsa_last_os_status() : 0;
    report("alloc_end", status, os, bytes, flags, &mem);
    if (status != BC250HSA_OK) { result = 1; goto cleanup; }
    if (flags & BC250HSA_MEM_MAPPABLE) {
        void *mapping = NULL;
        report("map_begin", BC250HSA_OK, 0, bytes, flags, &mem);
        status = bc250hsa_map(dev, &mem, &mapping);
        os = status == BC250HSA_EOS ? bc250hsa_last_os_status() : 0;
        report("map_end", status, os, bytes, flags, &mem);
        if (status != BC250HSA_OK) result = 1;
        else {
            printf("mapping_nonnull=%u\n", mapping != NULL ? 1u : 0u);
            if (!mapping) result = 1;
            status = bc250hsa_unmap(dev, &mem);
            os = status == BC250HSA_EOS ? bc250hsa_last_os_status() : 0;
            report("unmap_end", status, os, bytes, flags, &mem);
            if (status != BC250HSA_OK) result = 1;
        }
    }
cleanup:
    if (mem.opaque) {
        report("free_begin", BC250HSA_OK, 0, bytes, flags, &mem);
        status = bc250hsa_free(dev, &mem);
        os = status == BC250HSA_EOS ? bc250hsa_last_os_status() : 0;
        report("free_end", status, os, bytes, flags, &mem);
        if (status != BC250HSA_OK) result = 1;
    }
    report("close_begin", BC250HSA_OK, 0, bytes, flags, &mem);
    bc250hsa_close(dev);
    report("close_returned", BC250HSA_OK, 0, bytes, flags, &mem);
    return result;
}
