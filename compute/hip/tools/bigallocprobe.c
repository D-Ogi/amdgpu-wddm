/* bigallocprobe.c - the lab check of BD-117: one device-local allocation above the VidMm
 * limit of one allocation (0xFFFF0000 bytes), through layer 1 alone.
 *
 * Layer 1 builds such an allocation from several child allocations under one GPU address
 * reservation (kmt_memory.c, alloc_aggregate). This tool checks on unit A what the host tests
 * can only check against a fake kernel driver:
 *
 *   1. the allocation succeeds and answers BC250HSA_ACCESS_SCATTER, and its parts
 *      (bc250hsa_memory_part_get) fill the range without a gap; each part is printed with its
 *      heap, because the layout cuts the child at the edge of the local budget;
 *   2. the host walkers write and read back bytes on both sides of every child boundary, at
 *      the 4 GiB and 8 GiB lines and at the last byte;
 *   3. a kernel (vadd of the committed fixture) reads two ranges that cross child boundaries
 *      and writes a third at the end of the last child, through the one contiguous GPU range;
 *   4. a GPU copy moves a range across a child boundary into another one, when the coherence
 *      control of layer 1 admits a GPU copy into this memory (else it says that it skipped);
 *   5. the free succeeds, and an allocation of the same size afterwards succeeds as well;
 * With --expect-oom the size lies above the local and non-local budgets, which it prints
 * first: step 1 must then answer BC250HSA_ENOMEM, and steps 2 to 5 do not run.
 *   6. a request far above the memory of the part fails as BC250HSA_ENOMEM, and a small
 *      allocation and a kernel afterwards still work.
 *
 * Every wait is bounded (--wait-total, default 20000 ms). A wait that runs out leaves the
 * device to the operating system and frees nothing, as hipprobe does. It prints one line per
 * step and "RESULT PASS" or "RESULT FAIL" last. Exit codes: 0 pass, 1 usage, 2 host failure,
 * 3 timeout, 4 device lost, 5 a step failed, 6 adapter failure.
 *
 * It needs no HIP, no ROCm and no AMD user-mode component.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "bc250hsa.h"
#include "../bc250hsa/allocation_plan.h"

#define EXIT_OK          0
#define EXIT_USAGE       1
#define EXIT_HOST        2
#define EXIT_TIMEOUT     3
#define EXIT_DEVICELOST  4
#define EXIT_FAILED      5
#define EXIT_OS          6

#define PYTORCH_BYTES    UINT64_C(11484656026)   /* the request of the BD-117 trace */
#define OOM_BYTES        (UINT64_C(48) << 30)
#define PROBE_BYTES      8192u
#define VADD_N           (1u << 20)                /* 4 MiB per vector */
#define KERNARG_BYTES    4096u
#define MIB              (UINT64_C(1) << 20)

static uint32_t g_wait_slice = 1000u;
static uint32_t g_wait_total = 20000u;
static int      g_failed;
static int      g_timeout;
static int      g_lost;

static double now_ms(void)
{
    static LARGE_INTEGER frequency;
    LARGE_INTEGER        counter;
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

static void log_sink(void* ctx, uint32_t level, const char* message)
{
    static const char* names[] = { "error", "warn", "info", "trace" };
    (void)ctx;
    fprintf(stderr, "[bc250hsa %s] %s\n", names[(level < 4u) ? level : 3u], message);
}

static void step(const char* name, int ok, const char* detail, double ms)
{
    printf("step %-22s %s  %.1f ms  %s\n", name, ok ? "ok  " : "FAIL", ms,
           detail != NULL ? detail : "");
    fflush(stdout);
    if (!ok) {
        g_failed++;
    }
}

static void step_status(const char* name, bc250hsa_status status, double ms)
{
    char detail[160];
    snprintf(detail, sizeof(detail), "%s (ntstatus 0x%08lx)", bc250hsa_status_string(status),
             (unsigned long)bc250hsa_last_os_status());
    step(name, status == BC250HSA_OK, detail, ms);
}

/* A byte that names its own offset, so a byte read from the wrong child shows. */
static uint8_t pattern(uint64_t offset, uint32_t seed)
{
    uint64_t x = offset * UINT64_C(0x9E3779B97F4A7C15) + seed;
    x ^= x >> 29;
    return (uint8_t)(x ^ (x >> 17));
}

static float vadd_a(uint32_t i) { return (float)(i & 1023u) * 0.5f; }
static float vadd_b(uint32_t i) { return (float)((i * 7u) & 511u) * 0.25f; }

static void* read_file(const char* path, size_t* bytes_out)
{
    FILE*  f = fopen(path, "rb");
    void*  buffer;
    long   length;
    *bytes_out = 0;
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (length = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buffer = malloc((size_t)length);
    if (buffer != NULL && fread(buffer, 1, (size_t)length, f) != (size_t)length) {
        free(buffer);
        buffer = NULL;
    }
    fclose(f);
    if (buffer != NULL) {
        *bytes_out = (size_t)length;
    }
    return buffer;
}

typedef struct probe {
    bc250hsa_device*        dev;
    struct bc250hsa_module* mod;
    const bc250hsa_kernel*  vadd;
    bc250hsa_mem            kernarg;
} probe;

/* vadd over three ranges of the given allocations. c receives a + b. 0 on any failure. */
static int run_vadd(probe* p, const bc250hsa_mem* a, uint64_t a_off, const bc250hsa_mem* b,
                    uint64_t b_off, const bc250hsa_mem* c, uint64_t c_off, const char* name)
{
    const uint64_t       bytes = (uint64_t)VADD_N * 4u;
    float*               host = (float*)malloc((size_t)bytes);
    bc250hsa_launch      launch;
    bc250hsa_pack_result pack;
    bc250hsa_dispatch    dispatch;
    bc250hsa_status      status;
    uint64_t             va_a = a->va + a_off;
    uint64_t             va_b = b->va + b_off;
    uint64_t             va_c = c->va + c_off;
    int32_t              n = (int32_t)VADD_N;
    void*                args[4];
    uint64_t             fence = 0;
    uint32_t             i;
    uint32_t             mismatches = 0;
    double               t0 = now_ms();
    char                 detail[200];

    if (host == NULL) {
        step(name, 0, "no host buffer", 0.0);
        return 0;
    }
    for (i = 0; i < VADD_N; i++) { host[i] = vadd_a(i); }
    status = bc250hsa_copy_to_device(p->dev, a, a_off, host, bytes);
    for (i = 0; i < VADD_N && status == BC250HSA_OK; i++) { host[i] = vadd_b(i); }
    if (status == BC250HSA_OK) { status = bc250hsa_copy_to_device(p->dev, b, b_off, host, bytes); }
    if (status == BC250HSA_OK) { status = bc250hsa_fill_device_cpu(p->dev, c, c_off, 0u, bytes); }
    if (status != BC250HSA_OK) {
        step_status(name, status, now_ms() - t0);
        free(host);
        return 0;
    }

    memset(&launch, 0, sizeof(launch));
    launch.struct_bytes = (uint32_t)sizeof(launch);
    launch.grid[0] = VADD_N / 256u; launch.grid[1] = 1u; launch.grid[2] = 1u;
    launch.block[0] = 256u; launch.block[1] = 1u; launch.block[2] = 1u;
    args[0] = &va_a; args[1] = &va_b; args[2] = &va_c; args[3] = &n;
    memset(&pack, 0, sizeof(pack));
    pack.struct_bytes = (uint32_t)sizeof(pack);
    status = bc250hsa_kernarg_pack(p->vadd, &launch, args, 4u, p->kernarg.host,
                                   (uint32_t)p->kernarg.bytes, p->kernarg.va, &pack);
    if (status == BC250HSA_OK) {
        bc250hsa_write_barrier();
        memset(&dispatch, 0, sizeof(dispatch));
        dispatch.struct_bytes = (uint32_t)sizeof(dispatch);
        dispatch.kernel = p->vadd;
        dispatch.kernarg_va = p->kernarg.va;
        if (pack.dispatch_packet_requested != 0u) {
            dispatch.dispatch_packet_va = p->kernarg.va + pack.dispatch_packet_offset;
        }
        dispatch.launch = launch;
        status = bc250hsa_dispatch_submit(p->dev, &dispatch, &fence);
    }
    if (status == BC250HSA_OK) {
        status = bc250hsa_wait(p->dev, fence, g_wait_slice, g_wait_total);
    }
    if (status == BC250HSA_ETIMEOUT) { g_timeout = 1; }
    if (status == BC250HSA_EDEVICELOST) { g_lost = 1; }
    if (status != BC250HSA_OK) {
        step_status(name, status, now_ms() - t0);
        free(host);
        return 0;
    }
    status = bc250hsa_copy_from_device(p->dev, host, c, c_off, bytes);
    if (status != BC250HSA_OK) {
        step_status(name, status, now_ms() - t0);
        free(host);
        return 0;
    }
    for (i = 0; i < VADD_N; i++) {
        if (host[i] != vadd_a(i) + vadd_b(i)) { mismatches++; }
    }
    snprintf(detail, sizeof(detail),
             "a 0x%llx b 0x%llx c 0x%llx, %u of %u elements wrong",
             (unsigned long long)a_off, (unsigned long long)b_off, (unsigned long long)c_off,
             mismatches, VADD_N);
    step(name, mismatches == 0u, detail, now_ms() - t0);
    free(host);
    return mismatches == 0u;
}

/* Writes PROBE_BYTES around every offset in `at` and reads every one back. */
static void run_boundaries(probe* p, const bc250hsa_mem* m, const uint64_t* at, uint32_t count,
                           uint32_t seed, const char* name)
{
    uint8_t         buffer[PROBE_BYTES];
    uint32_t        k;
    uint32_t        wrong = 0;
    bc250hsa_status status = BC250HSA_OK;
    double          t0 = now_ms();
    char            detail[160];

    for (k = 0; k < count && status == BC250HSA_OK; k++) {
        uint32_t j;
        for (j = 0; j < PROBE_BYTES; j++) { buffer[j] = pattern(at[k] + j, seed); }
        status = bc250hsa_copy_to_device(p->dev, m, at[k], buffer, PROBE_BYTES);
    }
    for (k = 0; k < count && status == BC250HSA_OK; k++) {
        uint32_t j;
        memset(buffer, 0, sizeof(buffer));
        status = bc250hsa_copy_from_device(p->dev, buffer, m, at[k], PROBE_BYTES);
        for (j = 0; j < PROBE_BYTES && status == BC250HSA_OK; j++) {
            if (buffer[j] != pattern(at[k] + j, seed)) { wrong++; }
        }
    }
    if (status != BC250HSA_OK) {
        step_status(name, status, now_ms() - t0);
        return;
    }
    snprintf(detail, sizeof(detail), "%u ranges of %u bytes, %u bytes wrong", count,
             PROBE_BYTES, wrong);
    step(name, wrong == 0u, detail, now_ms() - t0);
}

static void run_gpu_copy(probe* p, const bc250hsa_mem* m, uint64_t src, uint64_t dst,
                         uint64_t bytes)
{
    uint32_t            ready = 0;
    bc250hsa_op_receipt receipt;
    bc250hsa_status     status;
    uint8_t*            host;
    uint64_t            i;
    uint64_t            wrong = 0;
    double              t0 = now_ms();
    char                detail[200];

    status = bc250hsa_gpu_copy_ready(p->dev, &ready);
    if (status != BC250HSA_OK || ready == 0u) {
        snprintf(detail, sizeof(detail), "SKIPPED: the GPU copy control answers %s, ready %u",
                 bc250hsa_status_string(status), ready);
        printf("step %-22s skip  %s\n", "gpu-copy-across", detail);
        return;
    }
    host = (uint8_t*)malloc((size_t)bytes);
    if (host == NULL) {
        step("gpu-copy-across", 0, "no host buffer", 0.0);
        return;
    }
    for (i = 0; i < bytes; i++) { host[i] = pattern(src + i, 77u); }
    status = bc250hsa_copy_to_device(p->dev, m, src, host, bytes);
    if (status == BC250HSA_OK) { status = bc250hsa_fill_device_cpu(p->dev, m, dst, 0u, bytes); }
    if (status == BC250HSA_OK) {
        memset(&receipt, 0, sizeof(receipt));
        receipt.struct_bytes = (uint32_t)sizeof(receipt);
        status = bc250hsa_copy_device_to_device(p->dev, m, dst, m, src, bytes, 0u, &receipt);
        if (status == BC250HSA_ETIMEOUT) { g_timeout = 1; }
        if (status == BC250HSA_EDEVICELOST) { g_lost = 1; }
    }
    if (status == BC250HSA_OK) {
        memset(host, 0, (size_t)bytes);
        status = bc250hsa_copy_from_device(p->dev, host, m, dst, bytes);
    }
    if (status != BC250HSA_OK) {
        step_status("gpu-copy-across", status, now_ms() - t0);
        free(host);
        return;
    }
    for (i = 0; i < bytes; i++) {
        if (host[i] != pattern(src + i, 77u)) { wrong++; }
    }
    snprintf(detail, sizeof(detail), "0x%llx -> 0x%llx, %llu bytes, %llu wrong",
             (unsigned long long)src, (unsigned long long)dst, (unsigned long long)bytes,
             (unsigned long long)wrong);
    step("gpu-copy-across", wrong == 0u, detail, now_ms() - t0);
    free(host);
}

static void usage(void)
{
    printf("bigallocprobe - BD-117: one device-local allocation above 0xFFFF0000 bytes\n\n");
    printf("  bigallocprobe [--bytes <n>] [--co <file>] [--wait-total <ms>] [--verbose]"
           " [<data directory>]\n\n");
    printf("  --bytes <n>       the size of the allocation (default %llu, the PyTorch request)\n",
           (unsigned long long)PYTORCH_BYTES);
    printf("  --co <file>       the code object with vadd (default <data>/m16_kernels.gfx1013.co)\n");
    printf("  --wait-total <ms> the bound of one wait (default 20000)\n");
    printf("  --no-oom          leave out step 6\n");
    printf("  --expect-oom      the size is above the budget: step 1 must answer ENOMEM\n");
    printf("\nExit codes: 0 pass, 1 usage, 2 host failure, 3 timeout, 4 device lost,"
           " 5 a step failed, 6 adapter failure.\n");
}

int main(int argc, char** argv)
{
    bc250hsa_open_params params;
    probe                p;
    bc250hsa_mem         big;
    bc250hsa_status      status;
    const char*          data_dir = ".";
    const char*          code_object = NULL;
    char                 path[1024];
    uint64_t             bytes = PYTORCH_BYTES;
    uint64_t             children;
    uint64_t             at[32];
    uint32_t             count = 0;
    uint32_t             access = 0;
    void*                file = NULL;
    const void*          image = NULL;
    size_t               image_bytes = 0;
    size_t               file_bytes = 0;
    int                  verbose = 0;
    int                  oom = 1;
    int                  expect_oom = 0;
    int                  i;
    double               t0;
    char                 detail[200];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { usage(); return EXIT_OK; }
        if (strcmp(argv[i], "--verbose") == 0) { verbose = 1; continue; }
        if (strcmp(argv[i], "--no-oom") == 0) { oom = 0; continue; }
        if (strcmp(argv[i], "--expect-oom") == 0) { expect_oom = 1; continue; }
        if (strcmp(argv[i], "--bytes") == 0 && i + 1 < argc) {
            bytes = _strtoui64(argv[++i], NULL, 0);
            continue;
        }
        if (strcmp(argv[i], "--co") == 0 && i + 1 < argc) { code_object = argv[++i]; continue; }
        if (strcmp(argv[i], "--wait-total") == 0 && i + 1 < argc) {
            g_wait_total = (uint32_t)strtoul(argv[++i], NULL, 10);
            continue;
        }
        if (argv[i][0] == '-') { usage(); return EXIT_USAGE; }
        data_dir = argv[i];
    }
    /* Two children at least, and room for the three vectors of step 3 apart from each other. */
    if (bytes <= BC250HSA_ALLOCATION_CHILD_MAX + 16u * MIB || bytes > (UINT64_C(64) << 30)) {
        fprintf(stderr, "bigallocprobe: --bytes must lie above 0x%llx + 16 MiB and at most"
                        " 64 GiB\n", (unsigned long long)BC250HSA_ALLOCATION_CHILD_MAX);
        return EXIT_USAGE;
    }
    children = (bytes + BC250HSA_ALLOCATION_CHILD_MAX - 1u) / BC250HSA_ALLOCATION_CHILD_MAX;
    if (verbose) {
        bc250hsa_set_log(log_sink, NULL, 2u);
    }
    printf("bigallocprobe: %llu bytes, at least %llu children of at most 0x%llx bytes, ABI %u.%u\n",
           (unsigned long long)bytes, (unsigned long long)children,
           (unsigned long long)BC250HSA_ALLOCATION_CHILD_MAX,
           (unsigned)bc250hsa_abi_version_major(), (unsigned)bc250hsa_abi_version_minor());

    memset(&p, 0, sizeof(p));
    memset(&params, 0, sizeof(params));
    params.struct_bytes = (uint32_t)sizeof(params);
    status = bc250hsa_open(&params, &p.dev);
    if (status != BC250HSA_OK) {
        fprintf(stderr, "bigallocprobe: cannot open the adapter: %s (ntstatus 0x%08lx)\n",
                bc250hsa_status_string(status), (unsigned long)bc250hsa_last_os_status());
        return EXIT_OS;
    }
    if (code_object != NULL) {
        snprintf(path, sizeof(path), "%s", code_object);
    } else {
        snprintf(path, sizeof(path), "%s/m16_kernels.gfx1013.co", data_dir);
    }
    file = read_file(path, &file_bytes);
    if (file == NULL) {
        fprintf(stderr, "bigallocprobe: cannot read %s\n", path);
        bc250hsa_close(p.dev);
        return EXIT_HOST;
    }
    image = file;
    image_bytes = file_bytes;
    if (file_bytes >= 24u && memcmp(file, "__CLANG_OFFLOAD_BUNDLE__", 24) == 0 &&
        bc250hsa_unbundle(file, file_bytes, NULL, &image, &image_bytes) != BC250HSA_OK) {
        fprintf(stderr, "bigallocprobe: %s is a bundle without a gfx1013 entry\n", path);
        free(file);
        bc250hsa_close(p.dev);
        return EXIT_HOST;
    }
    status = bc250hsa_module_load(p.dev, image, image_bytes, &p.mod);
    if (status == BC250HSA_OK) {
        p.vadd = bc250hsa_module_kernel_by_name(p.mod, "vadd");
        status = p.vadd != NULL ? bc250hsa_alloc(p.dev, KERNARG_BYTES, 4096u,
                                                 BC250HSA_MEM_HOST | BC250HSA_MEM_ZERO,
                                                 &p.kernarg)
                                : BC250HSA_ENOTFOUND;
    }
    if (status != BC250HSA_OK) {
        fprintf(stderr, "bigallocprobe: no vadd kernel or kernel argument buffer: %s\n",
                bc250hsa_status_string(status));
        free(file);
        bc250hsa_close(p.dev);
        return EXIT_HOST;
    }

    {
        bc250hsa_memory_budget budget;
        memset(&budget, 0, sizeof(budget));
        budget.struct_bytes = (uint32_t)sizeof(budget);
        if (bc250hsa_memory_budget_read(p.dev, &budget) == BC250HSA_OK) {
            printf("budget: local %llu used %llu, non-local %llu used %llu (valid 0x%x)\n",
                   (unsigned long long)budget.local_budget, (unsigned long long)budget.local_used,
                   (unsigned long long)budget.non_local_budget,
                   (unsigned long long)budget.non_local_used, budget.valid);
        } else {
            printf("budget: the query failed\n");
        }
        fflush(stdout);
    }

    /* 1. The allocation. With --expect-oom the request is above the budget, and the step
     * passes only when layer 1 refuses it as out of memory; steps 2 to 5 then do not run. */
    memset(&big, 0, sizeof(big));
    t0 = now_ms();
    status = bc250hsa_alloc(p.dev, bytes, 0u, BC250HSA_MEM_MAPPABLE, &big);
    if (expect_oom) {
        snprintf(detail, sizeof(detail), "%s (ntstatus 0x%08lx); ENOMEM expected",
                 bc250hsa_status_string(status), (unsigned long)bc250hsa_last_os_status());
        step("alloc-above-budget", status == BC250HSA_ENOMEM, detail, now_ms() - t0);
        if (status == BC250HSA_OK) {
            bc250hsa_free(p.dev, &big);
        }
        memset(&big, 0, sizeof(big));
        status = BC250HSA_ENOMEM;
    } else {
        step_status("alloc", status, now_ms() - t0);
    }
    if (status == BC250HSA_OK) {
        status = bc250hsa_memory_access(p.dev, &big, &access);
        snprintf(detail, sizeof(detail), "%s, access %u (2 is scatter), va 0x%llx, host %p",
                 bc250hsa_status_string(status), access, (unsigned long long)big.va, big.host);
        step("memory-access", status == BC250HSA_OK && access == BC250HSA_ACCESS_SCATTER &&
                                  big.host == NULL, detail, 0.0);
    }

    if (status == BC250HSA_OK && access == BC250HSA_ACCESS_SCATTER) {
        /* The parts as layer 1 made them: their sizes follow the local budget, so the child
         * boundaries come from bc250hsa_memory_part_get, not from multiples of the cap. */
        uint64_t             bound[32];
        uint32_t             nb = 0u;
        uint32_t             k;
        uint64_t             a_off = 0u;
        uint64_t             b_off;
        uint64_t             in_carve_out = 0u;
        uint64_t             next = 0u;
        int                  parts_ok = 1;
        const uint64_t       c_off = big.bytes - 4u * MIB - 64u * 1024u;
        bc250hsa_memory_part part;
        for (k = 0; k < 32u; k++) {
            status = bc250hsa_memory_part_get(p.dev, &big, k, &part);
            if (status != BC250HSA_OK) {
                break;
            }
            printf("part %u: offset 0x%llx, %llu bytes, %s\n", (unsigned)k,
                   (unsigned long long)part.offset, (unsigned long long)part.bytes,
                   part.carve_out ? "carve-out" : "system memory");
            if (part.carve_out) {
                in_carve_out += part.bytes;
            }
            parts_ok = parts_ok && part.offset == next && part.bytes != 0u;
            next = part.offset + part.bytes;
            if (k > 0u) {
                bound[nb++] = part.offset;
            }
        }
        children = k;
        snprintf(detail, sizeof(detail), "%llu parts, %llu bytes in the carve-out, %llu in system"
                 " memory", (unsigned long long)children, (unsigned long long)in_carve_out,
                 (unsigned long long)(big.bytes - in_carve_out));
        step("parts", parts_ok && next == big.bytes && children >= 2u && k < 32u &&
                          status == BC250HSA_EINVAL, detail, 0.0);
        status = BC250HSA_OK;
        /* a crosses a boundary that leaves room for b and c after it; b crosses a later one when
         * there is one apart from both, else it lies 8 MiB past a's boundary. */
        for (k = 0; k < nb; k++) {
            if (bound[k] >= 2u * MIB && bound[k] + 10u * MIB <= c_off) {
                a_off = bound[k] - 2u * MIB;
                break;
            }
        }
        b_off = a_off + 8u * MIB;
        for (k = 0; k < nb; k++) {
            if (bound[k] >= a_off + 6u * MIB && bound[k] + 2u * MIB <= c_off) {
                b_off = bound[k] - 2u * MIB;
                break;
            }
        }
        printf("ranges: a 0x%llx, b 0x%llx, c 0x%llx, 4 MiB each\n", (unsigned long long)a_off,
               (unsigned long long)b_off, (unsigned long long)c_off);

        /* 2. Every child boundary, the 4 GiB and 8 GiB lines, the first and the last bytes. */
        at[count++] = 0u;
        for (k = 0; k < nb && count < 28u; k++) {
            at[count++] = bound[k] - PROBE_BYTES / 2u;
        }
        if ((UINT64_C(4) << 30) + PROBE_BYTES < big.bytes) {
            at[count++] = (UINT64_C(4) << 30) - PROBE_BYTES / 2u;
        }
        if ((UINT64_C(8) << 30) + PROBE_BYTES < big.bytes) {
            at[count++] = (UINT64_C(8) << 30) - PROBE_BYTES / 2u;
        }
        at[count++] = big.bytes - PROBE_BYTES;
        run_boundaries(&p, &big, at, count, 5u, "host-walk-boundaries");

        /* 3. A kernel over ranges that cross child boundaries. */
        if (!g_timeout && !g_lost) {
            run_vadd(&p, &big, a_off, &big, b_off, &big, c_off, "kernel-across");
        }
        /* 4. A GPU copy from one boundary-crossing range into another. */
        if (!g_timeout && !g_lost) {
            run_gpu_copy(&p, &big, a_off, b_off, 4u * MIB);
        }
    }
    if (g_timeout) {
        fprintf(stderr, "bigallocprobe: a wait ran out; this process frees nothing and leaves"
                        " the device to the operating system\n");
        printf("RESULT FAIL timeout\n");
        return EXIT_TIMEOUT;
    }

    /* 5. The free, and the same size again. */
    if (big.opaque != NULL) {
        t0 = now_ms();
        status = bc250hsa_free(p.dev, &big);
        step_status("free", status, now_ms() - t0);
        if (status == BC250HSA_OK) {
            t0 = now_ms();
            status = bc250hsa_alloc(p.dev, bytes, 0u, BC250HSA_MEM_MAPPABLE, &big);
            step_status("realloc", status, now_ms() - t0);
            if (status == BC250HSA_OK) {
                at[0] = big.bytes - PROBE_BYTES;
                count = 1;
                run_boundaries(&p, &big, at, count, 9u, "realloc-last-bytes");
                t0 = now_ms();
                status = bc250hsa_free(p.dev, &big);
                step_status("free-again", status, now_ms() - t0);
            }
        }
    }

    /* 6. A request the part cannot hold, and a working device after it. */
    if (oom && !g_lost) {
        bc250hsa_mem huge;
        bc250hsa_mem vecs[3];
        int          made = 0;
        memset(&huge, 0, sizeof(huge));
        t0 = now_ms();
        status = bc250hsa_alloc(p.dev, OOM_BYTES, 0u, BC250HSA_MEM_MAPPABLE, &huge);
        snprintf(detail, sizeof(detail), "48 GiB answers %s (ntstatus 0x%08lx); ENOMEM expected",
                 bc250hsa_status_string(status), (unsigned long)bc250hsa_last_os_status());
        step("oom-typed", status == BC250HSA_ENOMEM, detail, now_ms() - t0);
        if (status == BC250HSA_OK) {
            bc250hsa_free(p.dev, &huge);
        }
        memset(vecs, 0, sizeof(vecs));
        for (i = 0; i < 3; i++) {
            /* Below the child limit an allocation is one allocation, reached through its map,
             * as hipMalloc reaches it. */
            void* host = NULL;
            if (bc250hsa_alloc(p.dev, (uint64_t)VADD_N * 4u, 0u, BC250HSA_MEM_MAPPABLE,
                               &vecs[i]) == BC250HSA_OK &&
                bc250hsa_map(p.dev, &vecs[i], &host) == BC250HSA_OK) {
                vecs[i].host = host;
                made++;
            }
        }
        if (made == 3) {
            run_vadd(&p, &vecs[0], 0u, &vecs[1], 0u, &vecs[2], 0u, "recovery-kernel");
        } else {
            step("recovery-kernel", 0, "a small allocation after the refusal failed", 0.0);
        }
        if (!g_timeout) {
            for (i = 0; i < 3; i++) {
                if (vecs[i].opaque != NULL) { bc250hsa_free(p.dev, &vecs[i]); }
            }
        }
    }
    if (g_timeout) {
        printf("RESULT FAIL timeout\n");
        return EXIT_TIMEOUT;
    }

    bc250hsa_free(p.dev, &p.kernarg);
    bc250hsa_module_unload(p.mod);
    free(file);
    bc250hsa_close(p.dev);
    if (g_lost) {
        printf("RESULT FAIL device lost\n");
        return EXIT_DEVICELOST;
    }
    printf("RESULT %s %d step(s) failed\n", g_failed == 0 ? "PASS" : "FAIL", g_failed);
    return g_failed == 0 ? EXIT_OK : EXIT_FAILED;
}
