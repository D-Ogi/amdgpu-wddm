/* hipprobe.c - the step-1 tool of milestone M16: one compute dispatch on the BC-250
 * through our own kernel driver, with the result checked on the processor.
 *
 * Section 6 of docs/design/m16-hip-route-b.md. It loads a gfx1013 code object (a plain
 * object or a clang offload bundle), runs three kernels, checks every result on the
 * processor and prints one JSON line per kernel. It exits non-zero on any mismatch, on
 * a timeout and on a lost device.
 *
 * Every wait is bounded. The defaults are 1000 ms per slice and 10000 ms in all, so
 * that the whole trial stays well inside the three-minute bound of a lab trial, while
 * the library keeps its own 120000 ms default for a long-running client. A timeout
 * never ends quietly: the tool prints the fence values, the execution state and the
 * dword count of the indirect buffer that is still in flight, says that the submission
 * is still in flight, and leaves the device alone instead of freeing memory that the
 * command processor may still read.
 *
 * It needs no HIP, no ROCm and no AMD user-mode component.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "bc250hsa.h"

#define EXIT_OK          0
#define EXIT_USAGE       1
#define EXIT_HOST        2
#define EXIT_TIMEOUT     3
#define EXIT_DEVICELOST  4
#define EXIT_MISMATCH    5
#define EXIT_OS          6

#define DEFAULT_N          1048576u
#define REDUCE_BLOCK       256u
#define DEFAULT_WAIT_SLICE 1000u
#define DEFAULT_WAIT_TOTAL 10000u
#define KERNARG_BYTES      4096u

typedef struct options {
    const char* code_object;
    const char* data_dir;
    const char* only_kernel;
    uint32_t    n;
    uint32_t    grid[3];
    uint32_t    block[3];
    uint32_t    wait_slice_ms;
    uint32_t    wait_total_ms;
    int         selftest;
    int         info;
    int         verbose;
} options;

static int g_failures;
static int g_timeout;
static int g_device_lost;

/* -------------------------------------------------------------------------------
 * Small helpers
 * ----------------------------------------------------------------------------- */

static void log_sink(void* ctx, uint32_t level, const char* message)
{
    static const char* names[] = { "error", "warn", "info", "trace" };
    (void)ctx;
    fprintf(stderr, "[bc250hsa %s] %s\n", names[(level < 4u) ? level : 3u], message);
}

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

static void* read_file(const char* path, size_t* bytes_out)
{
    FILE*  f;
    void*  buffer;
    long   length;
    size_t read;

    *bytes_out = 0;
    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "hipprobe: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (length = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buffer = malloc((size_t)length);
    if (buffer == NULL) {
        fclose(f);
        return NULL;
    }
    read = fread(buffer, 1, (size_t)length, f);
    fclose(f);
    if (read != (size_t)length) {
        free(buffer);
        return NULL;
    }
    *bytes_out = read;
    return buffer;
}

/* The input values of the three kernels. They are plain functions of the index, so the
 * processor reference needs no second buffer and the trial is reproducible. */
static float vadd_a(uint32_t i) { return (float)(i & 1023u) * 0.5f; }
static float vadd_b(uint32_t i) { return (float)((i * 7u) & 511u) * 0.25f; }
/* The input of reduce256 must not repeat every 256 elements, or two workgroups would
 * reduce the same numbers and a wrong workgroup index would still compare equal. Every
 * value is a multiple of 2^-4 below 2^8, so every partial sum of 256 of them is exact
 * in a 32-bit float and the comparison needs no tolerance. */
static float reduce_in(uint32_t i) { return (float)(i & 4095u) * 0.0625f; }

/* The same tree the kernel walks, in the same order, so that the comparison is exact
 * and not a tolerance. The kernel adds tile[t] += tile[t + s] for s = 128 down to 1,
 * with a barrier between two levels. */
static float reduce_reference(uint32_t group)
{
    float    tile[REDUCE_BLOCK];
    uint32_t s;
    uint32_t t;

    for (t = 0; t < REDUCE_BLOCK; t++) {
        tile[t] = reduce_in(group * REDUCE_BLOCK + t);
    }
    for (s = REDUCE_BLOCK / 2u; s > 0u; s >>= 1) {
        for (t = 0; t < s; t++) {
            tile[t] += tile[t + s];
        }
    }
    return tile[0];
}

/* -------------------------------------------------------------------------------
 * One JSON line per kernel
 * ----------------------------------------------------------------------------- */

typedef struct kernel_report {
    const char* kernel;
    const char* mode;
    uint32_t    items;
    uint32_t    grid[3];
    uint32_t    block[3];
    uint32_t    kernarg_bytes;
    uint32_t    group_segment_bytes;
    uint32_t    dispatch_dwords;
    uint64_t    fence_value;
    uint64_t    fence_read;
    double      wait_ms;
    uint64_t    checked;
    uint64_t    mismatches;
    int64_t     first_mismatch;
    int         dangling;
    const char* status;
    const char* detail;
} kernel_report;

static void report_init(kernel_report* r, const char* kernel, const char* mode)
{
    memset(r, 0, sizeof(*r));
    r->kernel = kernel;
    r->mode = mode;
    r->first_mismatch = -1;
    r->status = "ok";
    r->detail = "";
}

static void report_print(const kernel_report* r)
{
    printf("{\"tool\":\"hipprobe\",\"kernel\":\"%s\",\"mode\":\"%s\",\"status\":\"%s\"",
           r->kernel, r->mode, r->status);
    printf(",\"items\":%lu", (unsigned long)r->items);
    printf(",\"grid\":[%lu,%lu,%lu]", (unsigned long)r->grid[0], (unsigned long)r->grid[1],
           (unsigned long)r->grid[2]);
    printf(",\"block\":[%lu,%lu,%lu]", (unsigned long)r->block[0], (unsigned long)r->block[1],
           (unsigned long)r->block[2]);
    printf(",\"kernarg_bytes\":%lu,\"group_segment_bytes\":%lu",
           (unsigned long)r->kernarg_bytes, (unsigned long)r->group_segment_bytes);
    printf(",\"dispatch_dwords\":%lu", (unsigned long)r->dispatch_dwords);
    printf(",\"fence_value\":%llu,\"fence_read\":%llu",
           (unsigned long long)r->fence_value, (unsigned long long)r->fence_read);
    printf(",\"wait_ms\":%.3f", r->wait_ms);
    printf(",\"checked\":%llu,\"mismatches\":%llu,\"first_mismatch\":%lld",
           (unsigned long long)r->checked, (unsigned long long)r->mismatches,
           (long long)r->first_mismatch);
    printf(",\"dangling_submission\":%s", r->dangling ? "true" : "false");
    printf(",\"detail\":\"%s\"}\n", r->detail);
    fflush(stdout);
}

/* -------------------------------------------------------------------------------
 * The host allocator of the self test
 * ----------------------------------------------------------------------------- */

static bc250hsa_status host_alloc(void* ctx, uint64_t bytes, uint64_t alignment,
                                  uint32_t flags, bc250hsa_mem* out)
{
    uint8_t*  raw;
    uintptr_t aligned;

    (void)ctx;
    if (alignment < 4096u) {
        alignment = 4096u;
    }
    raw = (uint8_t*)calloc(1, (size_t)(bytes + alignment));
    if (raw == NULL) {
        return BC250HSA_ENOMEM;
    }
    aligned = ((uintptr_t)raw + (uintptr_t)alignment - 1u) & ~((uintptr_t)alignment - 1u);
    memset(out, 0, sizeof(*out));
    out->host = (void*)aligned;
    out->va = (uint64_t)aligned;
    out->bytes = bytes;
    out->flags = flags;
    out->opaque = raw;
    return BC250HSA_OK;
}

static void host_free(void* ctx, bc250hsa_mem* mem)
{
    (void)ctx;
    free(mem->opaque);
    memset(mem, 0, sizeof(*mem));
}

/* -------------------------------------------------------------------------------
 * The code object
 * ----------------------------------------------------------------------------- */

/* Reads the file and, when it is a clang offload bundle, takes the gfx1013 entry out of
 * it. The caller frees `file`, never `image`. */
static int open_code_object(const char* path, void** file, const void** image,
                            size_t* image_bytes)
{
    size_t file_bytes = 0;

    *file = read_file(path, &file_bytes);
    if (*file == NULL) {
        return 0;
    }
    if (file_bytes >= 24u && memcmp(*file, "__CLANG_OFFLOAD_BUNDLE__", 24) == 0) {
        const bc250hsa_status status =
            bc250hsa_unbundle(*file, file_bytes, NULL, image, image_bytes);
        if (status != BC250HSA_OK) {
            fprintf(stderr, "hipprobe: %s is an offload bundle this build cannot read: %s\n",
                    path, bc250hsa_status_string(status));
            free(*file);
            *file = NULL;
            return 0;
        }
        return 1;
    }
    *image = *file;
    *image_bytes = file_bytes;
    return 1;
}

/* -------------------------------------------------------------------------------
 * The self test: everything that runs with no adapter
 * ----------------------------------------------------------------------------- */

static int selftest_kernel(const struct bc250hsa_module* mod, const char* name,
                           const bc250hsa_launch* launch, void* const* args,
                           uint32_t arg_count, uint32_t items)
{
    const bc250hsa_kernel* k = bc250hsa_module_kernel_by_name(mod, name);
    kernel_report          report;
    bc250hsa_pack_result   pack;
    bc250hsa_dispatch      dispatch;
    bc250hsa_pm4_env       env;
    uint8_t                kernarg[KERNARG_BYTES];
    uint32_t               dwords[BC250HSA_PM4_MAX_DWORDS];
    uint32_t               written = 0;
    bc250hsa_status        status;

    report_init(&report, name, "selftest");
    report.items = items;
    if (k == NULL) {
        report.status = "error";
        report.detail = "the code object holds no kernel of this name";
        report_print(&report);
        return 0;
    }
    report.grid[0] = launch->grid[0]; report.grid[1] = launch->grid[1];
    report.grid[2] = launch->grid[2];
    report.block[0] = launch->block[0]; report.block[1] = launch->block[1];
    report.block[2] = launch->block[2];
    report.kernarg_bytes = k->kernarg_bytes;
    report.group_segment_bytes = k->group_segment_bytes;

    memset(&pack, 0, sizeof(pack));
    pack.struct_bytes = (uint32_t)sizeof(pack);
    status = bc250hsa_kernarg_pack(k, launch, args, arg_count, kernarg, KERNARG_BYTES,
                                   0x0000004000002000ull, &pack);
    if (status != BC250HSA_OK) {
        report.status = "error";
        report.detail = bc250hsa_status_string(status);
        report_print(&report);
        return 0;
    }

    memset(&dispatch, 0, sizeof(dispatch));
    dispatch.struct_bytes = (uint32_t)sizeof(dispatch);
    dispatch.kernel = k;
    dispatch.kernarg_va = 0x0000004000002000ull;
    if (pack.dispatch_packet_requested != 0u) {
        dispatch.dispatch_packet_va = dispatch.kernarg_va + pack.dispatch_packet_offset;
    }
    dispatch.launch = *launch;
    memset(&env, 0, sizeof(env));
    env.struct_bytes = (uint32_t)sizeof(env);
    env.flags = BC250HSA_DISPATCH_GFX_RING;
    env.fence_va = 0x0000004000003000ull;
    env.fence_value = 1u;
    status = bc250hsa_buffer_resource(0x0000004000004000ull, 4096u, env.private_segment_rsrc);
    if (status == BC250HSA_OK) {
        status = bc250hsa_pm4_build_dispatch(&dispatch, &env, dwords,
                                             BC250HSA_PM4_MAX_DWORDS, &written);
    }
    if (status != BC250HSA_OK) {
        report.status = "error";
        report.detail = bc250hsa_status_string(status);
        report_print(&report);
        return 0;
    }
    report.dispatch_dwords = written;
    if ((written % 8u) != 0u) {
        report.status = "error";
        report.detail = "the indirect buffer is not padded to eight dwords";
        report_print(&report);
        return 0;
    }
    /* Every argument of the metadata list reached the buffer: the explicit ones from
     * the launch, the hidden ones from the launch or as a documented zero. */
    report.checked = (uint64_t)pack.explicit_args_written + k->hidden_arg_count;
    report_print(&report);
    return 1;
}

static int run_selftest(const options* opt)
{
    char                    path[1024];
    void*                   file = NULL;
    const void*             image = NULL;
    size_t                  image_bytes = 0;
    bc250hsa_allocator      alloc;
    struct bc250hsa_module* mod = NULL;
    bc250hsa_status         status;
    bc250hsa_launch         launch;
    uint64_t                fake_a = 0x0000004000010000ull;
    uint64_t                fake_b = 0x0000004000020000ull;
    uint64_t                fake_c = 0x0000004000030000ull;
    int32_t                 n = (int32_t)opt->n;
    void*                   args[4];
    int                     ok = 1;
    uint32_t                group;

    if (opt->code_object != NULL) {
        snprintf(path, sizeof(path), "%s", opt->code_object);
    } else {
        snprintf(path, sizeof(path), "%s/m16_kernels.gfx1013.co",
                 (opt->data_dir != NULL) ? opt->data_dir : ".");
    }
    if (!open_code_object(path, &file, &image, &image_bytes)) {
        return EXIT_HOST;
    }
    alloc.ctx = NULL;
    alloc.alloc = host_alloc;
    alloc.free = host_free;
    status = bc250hsa_module_load_alloc(&alloc, image, image_bytes, &mod);
    if (status != BC250HSA_OK) {
        fprintf(stderr, "hipprobe: the code object does not load: %s\n",
                bc250hsa_status_string(status));
        free(file);
        return EXIT_HOST;
    }
    if (bc250hsa_module_kernel_count(mod) != 3u) {
        fprintf(stderr, "hipprobe: the code object holds %lu kernels, three were expected\n",
                (unsigned long)bc250hsa_module_kernel_count(mod));
        ok = 0;
    }

    /* vadd */
    memset(&launch, 0, sizeof(launch));
    launch.struct_bytes = (uint32_t)sizeof(launch);
    launch.grid[0] = (opt->n + 255u) / 256u; launch.grid[1] = 1u; launch.grid[2] = 1u;
    launch.block[0] = 256u; launch.block[1] = 1u; launch.block[2] = 1u;
    args[0] = &fake_a; args[1] = &fake_b; args[2] = &fake_c; args[3] = &n;
    ok &= selftest_kernel(mod, "vadd", &launch, args, 4u, opt->n);

    /* reduce256 */
    launch.grid[0] = opt->n / REDUCE_BLOCK;
    if (launch.grid[0] == 0u) {
        launch.grid[0] = 1u;
    }
    args[0] = &fake_a; args[1] = &fake_b;
    ok &= selftest_kernel(mod, "reduce256", &launch, args, 2u,
                          launch.grid[0] * REDUCE_BLOCK);

    /* writeGridSize */
    launch.grid[0] = opt->grid[0]; launch.grid[1] = opt->grid[1]; launch.grid[2] = opt->grid[2];
    launch.block[0] = opt->block[0]; launch.block[1] = opt->block[1];
    launch.block[2] = opt->block[2];
    args[0] = &fake_c;
    ok &= selftest_kernel(mod, "writeGridSize", &launch, args, 1u, 3u);

    /* The processor reference of reduce256 must be a function of the group alone, or
     * the comparison of the lab run would mean nothing. */
    for (group = 0; group < 4u; group++) {
        if (reduce_reference(group) != reduce_reference(group)) {
            fprintf(stderr, "hipprobe: the reduce256 reference is not deterministic\n");
            ok = 0;
        }
    }
    if (reduce_reference(0u) == reduce_reference(1u)) {
        fprintf(stderr, "hipprobe: the reduce256 reference gives one value for two groups\n");
        ok = 0;
    }

    bc250hsa_module_unload(mod);
    free(file);
    return ok ? EXIT_OK : EXIT_HOST;
}

/* -------------------------------------------------------------------------------
 * The device run
 * ----------------------------------------------------------------------------- */

typedef struct run_state {
    bc250hsa_device*        dev;
    struct bc250hsa_module* mod;
    bc250hsa_mem            kernarg;
    uint32_t                wait_slice_ms;
    uint32_t                wait_total_ms;
} run_state;

/* Packs, submits and waits. It fills the report and returns 0 on any failure.
 *
 * On a timeout it sets the dangling flag: a submission is still in flight, so this
 * process makes no further call that unmaps or destroys the memory the command
 * processor may still read. It does not keep anything alive beyond this process. The
 * operating system destroys the device, unmaps every virtual address and frees every
 * allocation when the process ends; the flag only keeps this process from doing it
 * first, one allocation at a time, under a running dispatch.
 *
 * A lost device is not dangling: dxgkrnl has already destroyed the device, so nothing
 * reads that memory any more and the ordinary teardown is correct. */
static int dispatch_and_wait(run_state* st, const bc250hsa_kernel* k,
                             const bc250hsa_launch* launch, void* const* args,
                             uint32_t arg_count, kernel_report* report)
{
    bc250hsa_pack_result pack;
    bc250hsa_dispatch    dispatch;
    bc250hsa_status      status;
    const uint32_t*      ib = NULL;
    uint32_t             ib_dwords = 0;
    uint64_t             fence_value = 0;
    double               started;

    report->kernarg_bytes = k->kernarg_bytes;
    report->group_segment_bytes = k->group_segment_bytes;
    report->grid[0] = launch->grid[0]; report->grid[1] = launch->grid[1];
    report->grid[2] = launch->grid[2];
    report->block[0] = launch->block[0]; report->block[1] = launch->block[1];
    report->block[2] = launch->block[2];

    {
        /* The buffer holds the kernel arguments and, for a kernel that reads the AQL
         * dispatch packet, the packet behind them (section 7.1 of the header). */
        uint32_t needed = 0;
        uint32_t needed_align = 0;
        if (bc250hsa_kernarg_requirements(k, &needed, &needed_align) != BC250HSA_OK ||
            needed > st->kernarg.bytes) {
            report->status = "error";
            report->detail = "the kernel argument buffer is too small";
            return 0;
        }
    }
    memset(&pack, 0, sizeof(pack));
    pack.struct_bytes = (uint32_t)sizeof(pack);
    status = bc250hsa_kernarg_pack(k, launch, args, arg_count, st->kernarg.host,
                                   (uint32_t)st->kernarg.bytes, st->kernarg.va, &pack);
    if (status != BC250HSA_OK) {
        report->status = "error";
        report->detail = bc250hsa_status_string(status);
        return 0;
    }
    bc250hsa_write_barrier();

    memset(&dispatch, 0, sizeof(dispatch));
    dispatch.struct_bytes = (uint32_t)sizeof(dispatch);
    dispatch.kernel = k;
    dispatch.kernarg_va = st->kernarg.va;
    if (pack.dispatch_packet_requested != 0u) {
        dispatch.dispatch_packet_va = st->kernarg.va + pack.dispatch_packet_offset;
    }
    dispatch.launch = *launch;

    started = now_ms();
    status = bc250hsa_dispatch_submit(st->dev, &dispatch, &fence_value);
    if (status != BC250HSA_OK) {
        report->status = "error";
        report->detail = bc250hsa_status_string(status);
        if (status == BC250HSA_EDEVICELOST) {
            report->status = "device-lost";
            g_device_lost = 1;
        }
        return 0;
    }
    report->fence_value = fence_value;
    if (bc250hsa_last_ib(st->dev, &ib, &ib_dwords) == BC250HSA_OK) {
        report->dispatch_dwords = ib_dwords;
    }

    status = bc250hsa_wait(st->dev, fence_value, st->wait_slice_ms, st->wait_total_ms);
    report->wait_ms = now_ms() - started;
    report->fence_read = bc250hsa_fence_read(st->dev);
    if (status == BC250HSA_ETIMEOUT) {
        bc250hsa_fault fault;
        fault.struct_bytes = (uint32_t)sizeof(fault);
        report->status = "timeout";
        report->detail = "the bound ran out and the submission is still in flight";
        report->dangling = 1;
        g_timeout = 1;
        if (bc250hsa_query_fault(st->dev, &fault) == BC250HSA_OK) {
            fprintf(stderr,
                    "hipprobe: timeout on %s after %.0f ms; fence %llu of %llu, device_lost %lu,"
                    " faulted_va 0x%llx, general_error %lu, pipeline_stage %lu,"
                    " indirect buffer %lu dwords still in flight\n",
                    k->name, report->wait_ms, (unsigned long long)report->fence_read,
                    (unsigned long long)fence_value, (unsigned long)fault.device_lost,
                    (unsigned long long)fault.faulted_va, (unsigned long)fault.general_error,
                    (unsigned long)fault.pipeline_stage, (unsigned long)ib_dwords);
        }
        return 0;
    }
    if (status == BC250HSA_EDEVICELOST) {
        report->status = "device-lost";
        report->detail = bc250hsa_status_string(status);
        g_device_lost = 1;
        return 0;
    }
    if (status != BC250HSA_OK) {
        report->status = "error";
        report->detail = bc250hsa_status_string(status);
        return 0;
    }
    return 1;
}

static int run_vadd(run_state* st, const options* opt)
{
    const bc250hsa_kernel* k = bc250hsa_module_kernel_by_name(st->mod, "vadd");
    kernel_report          report;
    bc250hsa_launch        launch;
    bc250hsa_mem           a;
    bc250hsa_mem           b;
    bc250hsa_mem           c;
    const uint64_t         bytes = (uint64_t)opt->n * 4u;
    void*                  args[4];
    int32_t                n = (int32_t)opt->n;
    uint32_t               i;
    int                    ok;

    report_init(&report, "vadd", "gpu");
    report.items = opt->n;
    if (k == NULL) {
        report.status = "error";
        report.detail = "no kernel vadd in the code object";
        report_print(&report);
        return 0;
    }
    if (bc250hsa_alloc(st->dev, bytes, 256u, BC250HSA_MEM_HOST, &a) != BC250HSA_OK ||
        bc250hsa_alloc(st->dev, bytes, 256u, BC250HSA_MEM_HOST, &b) != BC250HSA_OK ||
        bc250hsa_alloc(st->dev, bytes, 256u, BC250HSA_MEM_HOST | BC250HSA_MEM_ZERO, &c) !=
            BC250HSA_OK) {
        report.status = "error";
        report.detail = "a device allocation failed";
        report_print(&report);
        return 0;
    }
    for (i = 0; i < opt->n; i++) {
        ((float*)a.host)[i] = vadd_a(i);
        ((float*)b.host)[i] = vadd_b(i);
    }

    memset(&launch, 0, sizeof(launch));
    launch.struct_bytes = (uint32_t)sizeof(launch);
    launch.grid[0] = (opt->n + 255u) / 256u; launch.grid[1] = 1u; launch.grid[2] = 1u;
    launch.block[0] = 256u; launch.block[1] = 1u; launch.block[2] = 1u;
    args[0] = &a.va; args[1] = &b.va; args[2] = &c.va; args[3] = &n;

    ok = dispatch_and_wait(st, k, &launch, args, 4u, &report);
    if (ok) {
        for (i = 0; i < opt->n; i++) {
            const float expected = vadd_a(i) + vadd_b(i);
            report.checked++;
            if (((const float*)c.host)[i] != expected) {
                report.mismatches++;
                if (report.first_mismatch < 0) {
                    report.first_mismatch = (int64_t)i;
                }
            }
        }
        if (report.mismatches != 0u) {
            report.status = "mismatch";
            report.detail = "the processor reference and the device disagree";
            ok = 0;
        }
    }
    report_print(&report);
    if (!report.dangling) {
        bc250hsa_free(st->dev, &a);
        bc250hsa_free(st->dev, &b);
        bc250hsa_free(st->dev, &c);
    }
    return ok;
}

static int run_reduce256(run_state* st, const options* opt)
{
    const bc250hsa_kernel* k = bc250hsa_module_kernel_by_name(st->mod, "reduce256");
    kernel_report          report;
    bc250hsa_launch        launch;
    bc250hsa_mem           in;
    bc250hsa_mem           out;
    uint32_t               groups = opt->n / REDUCE_BLOCK;
    uint32_t               items;
    void*                  args[2];
    uint32_t               i;
    int                    ok;

    if (groups == 0u) {
        groups = 1u;
    }
    items = groups * REDUCE_BLOCK;
    report_init(&report, "reduce256", "gpu");
    report.items = items;
    if (k == NULL) {
        report.status = "error";
        report.detail = "no kernel reduce256 in the code object";
        report_print(&report);
        return 0;
    }
    if (bc250hsa_alloc(st->dev, (uint64_t)items * 4u, 256u, BC250HSA_MEM_HOST, &in) !=
            BC250HSA_OK ||
        bc250hsa_alloc(st->dev, (uint64_t)groups * 4u, 256u,
                       BC250HSA_MEM_HOST | BC250HSA_MEM_ZERO, &out) != BC250HSA_OK) {
        report.status = "error";
        report.detail = "a device allocation failed";
        report_print(&report);
        return 0;
    }
    for (i = 0; i < items; i++) {
        ((float*)in.host)[i] = reduce_in(i);
    }

    memset(&launch, 0, sizeof(launch));
    launch.struct_bytes = (uint32_t)sizeof(launch);
    launch.grid[0] = groups; launch.grid[1] = 1u; launch.grid[2] = 1u;
    launch.block[0] = REDUCE_BLOCK; launch.block[1] = 1u; launch.block[2] = 1u;
    args[0] = &in.va; args[1] = &out.va;

    ok = dispatch_and_wait(st, k, &launch, args, 2u, &report);
    if (ok) {
        for (i = 0; i < groups; i++) {
            const float expected = reduce_reference(i);
            report.checked++;
            if (((const float*)out.host)[i] != expected) {
                report.mismatches++;
                if (report.first_mismatch < 0) {
                    report.first_mismatch = (int64_t)i;
                }
            }
        }
        if (report.mismatches != 0u) {
            report.status = "mismatch";
            report.detail = "the local memory reduction disagrees with the reference";
            ok = 0;
        }
    }
    report_print(&report);
    if (!report.dangling) {
        bc250hsa_free(st->dev, &in);
        bc250hsa_free(st->dev, &out);
    }
    return ok;
}

static int run_write_grid_size(run_state* st, const options* opt)
{
    const bc250hsa_kernel* k = bc250hsa_module_kernel_by_name(st->mod, "writeGridSize");
    kernel_report          report;
    bc250hsa_launch        launch;
    bc250hsa_mem           out;
    void*                  args[1];
    uint32_t               i;
    int                    ok;

    report_init(&report, "writeGridSize", "gpu");
    report.items = 3u;
    if (k == NULL) {
        report.status = "error";
        report.detail = "no kernel writeGridSize in the code object";
        report_print(&report);
        return 0;
    }
    if (bc250hsa_alloc(st->dev, 256u, 256u, BC250HSA_MEM_HOST | BC250HSA_MEM_ZERO, &out) !=
        BC250HSA_OK) {
        report.status = "error";
        report.detail = "a device allocation failed";
        report_print(&report);
        return 0;
    }

    memset(&launch, 0, sizeof(launch));
    launch.struct_bytes = (uint32_t)sizeof(launch);
    launch.grid[0] = opt->grid[0]; launch.grid[1] = opt->grid[1]; launch.grid[2] = opt->grid[2];
    launch.block[0] = opt->block[0]; launch.block[1] = opt->block[1];
    launch.block[2] = opt->block[2];
    args[0] = &out.va;

    ok = dispatch_and_wait(st, k, &launch, args, 1u, &report);
    if (ok) {
        /* The kernel returns hidden_block_count_x, _y and _z: the work-group count of
         * the dispatch, which the packer wrote from the metadata list. AMDGPUUsage.rst
         * says this field is not the grid in work items. */
        for (i = 0; i < 3u; i++) {
            const uint32_t expected = launch.grid[i];
            report.checked++;
            if (((const uint32_t*)out.host)[i] != expected) {
                report.mismatches++;
                if (report.first_mismatch < 0) {
                    report.first_mismatch = (int64_t)i;
                }
                fprintf(stderr,
                        "hipprobe: writeGridSize dimension %lu is %lu, %lu was expected\n",
                        (unsigned long)i, (unsigned long)((const uint32_t*)out.host)[i],
                        (unsigned long)expected);
            }
        }
        if (report.mismatches != 0u) {
            report.status = "mismatch";
            report.detail = "the implicit argument block did not reach the kernel";
            ok = 0;
        }
    }
    report_print(&report);
    if (!report.dangling) {
        bc250hsa_free(st->dev, &out);
    }
    return ok;
}

static void print_info(bc250hsa_device* dev)
{
    bc250hsa_props props;

    props.struct_bytes = (uint32_t)sizeof(props);
    if (bc250hsa_props_read(dev, &props) != BC250HSA_OK) {
        printf("{\"tool\":\"hipprobe\",\"info\":\"unavailable\"}\n");
        return;
    }
    printf("{\"tool\":\"hipprobe\",\"info\":\"device\",\"name\":\"%s\"", props.name);
    printf(",\"gfx_ip\":[%lu,%lu,%lu]", (unsigned long)props.gfx_ip_major,
           (unsigned long)props.gfx_ip_minor, (unsigned long)props.gfx_ip_rev);
    printf(",\"cu_count\":%lu,\"se_count\":%lu,\"wave_size\":%lu",
           (unsigned long)props.cu_count, (unsigned long)props.se_count,
           (unsigned long)props.wave_size);
    printf(",\"lds_bytes_per_workgroup\":%lu,\"max_workgroup_size\":%lu",
           (unsigned long)props.lds_bytes_per_workgroup,
           (unsigned long)props.max_workgroup_size);
    printf(",\"gfx_clock_khz\":%lu,\"mem_clock_khz\":%lu", (unsigned long)props.gfx_clock_khz,
           (unsigned long)props.mem_clock_khz);
    printf(",\"vram_bytes\":%llu,\"visible_vram_bytes\":%llu,\"gtt_bytes\":%llu",
           (unsigned long long)props.vram_bytes, (unsigned long long)props.visible_vram_bytes,
           (unsigned long long)props.gtt_bytes);
    printf(",\"kmd_version\":[%lu,%lu,%lu,%lu]", (unsigned long)props.kmd_version[0],
           (unsigned long)props.kmd_version[1], (unsigned long)props.kmd_version[2],
           (unsigned long)props.kmd_version[3]);
    printf(",\"abi\":[%lu,%lu]}\n", (unsigned long)bc250hsa_abi_version_major(),
           (unsigned long)bc250hsa_abi_version_minor());
    fflush(stdout);
}

static void print_counters(void)
{
    bc250hsa_counters counters;

    counters.struct_bytes = (uint32_t)sizeof(counters);
    if (bc250hsa_counters_read(&counters) != BC250HSA_OK) {
        return;
    }
    printf("{\"tool\":\"hipprobe\",\"counters\":{\"modules_loaded\":%llu"
           ",\"dispatches_built\":%llu,\"submissions\":%llu,\"submissions_refused\":%llu"
           ",\"waits\":%llu,\"waits_fast\":%llu,\"waits_timed_out\":%llu"
           ",\"device_losses\":%llu,\"hidden_args_zeroed\":%llu,\"unknown_arg_kinds\":%llu"
           ",\"hostcall_buffer_requests\":%llu,\"dynamic_stack_refusals\":%llu}}\n",
           (unsigned long long)counters.modules_loaded,
           (unsigned long long)counters.dispatches_built,
           (unsigned long long)counters.submissions,
           (unsigned long long)counters.submissions_refused,
           (unsigned long long)counters.waits, (unsigned long long)counters.waits_fast,
           (unsigned long long)counters.waits_timed_out,
           (unsigned long long)counters.device_losses,
           (unsigned long long)counters.hidden_args_zeroed,
           (unsigned long long)counters.unknown_arg_kinds,
           (unsigned long long)counters.hostcall_buffer_requests,
           (unsigned long long)counters.dynamic_stack_refusals);
    fflush(stdout);
}

static int run_device(const options* opt)
{
    bc250hsa_open_params params;
    run_state            st;
    char                 path[1024];
    void*                file = NULL;
    const void*          image = NULL;
    size_t               image_bytes = 0;
    bc250hsa_status      status;
    int                  exit_code = EXIT_OK;

    memset(&st, 0, sizeof(st));
    st.wait_slice_ms = opt->wait_slice_ms;
    st.wait_total_ms = opt->wait_total_ms;

    memset(&params, 0, sizeof(params));
    params.struct_bytes = (uint32_t)sizeof(params);
    status = bc250hsa_open(&params, &st.dev);
    if (status != BC250HSA_OK) {
        fprintf(stderr, "hipprobe: cannot open the BC-250 adapter: %s (ntstatus 0x%08lx)\n",
                bc250hsa_status_string(status), (unsigned long)bc250hsa_last_os_status());
        return EXIT_OS;
    }
    print_info(st.dev);
    if (opt->info) {
        bc250hsa_close(st.dev);
        return EXIT_OK;
    }

    if (opt->code_object != NULL) {
        snprintf(path, sizeof(path), "%s", opt->code_object);
    } else {
        snprintf(path, sizeof(path), "%s/m16_kernels.gfx1013.co",
                 (opt->data_dir != NULL) ? opt->data_dir : ".");
    }
    if (!open_code_object(path, &file, &image, &image_bytes)) {
        bc250hsa_close(st.dev);
        return EXIT_HOST;
    }
    status = bc250hsa_module_load(st.dev, image, image_bytes, &st.mod);
    if (status != BC250HSA_OK) {
        fprintf(stderr, "hipprobe: the code object does not load: %s\n",
                bc250hsa_status_string(status));
        free(file);
        bc250hsa_close(st.dev);
        return EXIT_HOST;
    }
    status = bc250hsa_alloc(st.dev, KERNARG_BYTES, 4096u,
                            BC250HSA_MEM_HOST | BC250HSA_MEM_ZERO, &st.kernarg);
    if (status != BC250HSA_OK) {
        fprintf(stderr, "hipprobe: no kernel argument buffer: %s\n",
                bc250hsa_status_string(status));
        bc250hsa_module_unload(st.mod);
        free(file);
        bc250hsa_close(st.dev);
        return EXIT_HOST;
    }

    if (opt->only_kernel == NULL || strcmp(opt->only_kernel, "vadd") == 0) {
        if (!run_vadd(&st, opt)) { g_failures++; }
    }
    if (!g_timeout && !g_device_lost &&
        (opt->only_kernel == NULL || strcmp(opt->only_kernel, "reduce256") == 0)) {
        if (!run_reduce256(&st, opt)) { g_failures++; }
    }
    if (!g_timeout && !g_device_lost &&
        (opt->only_kernel == NULL || strcmp(opt->only_kernel, "writeGridSize") == 0)) {
        if (!run_write_grid_size(&st, opt)) { g_failures++; }
    }
    print_counters();

    if (g_timeout) {
        /* A submission is still in flight. This process frees nothing and does not
         * close the device: it issues no unmap and no destroy under a running
         * dispatch, and bc250hsa_close would wait for that submission again. The
         * operating system reclaims the allocations, the addresses and the device
         * when this process ends, a moment later. */
        fprintf(stderr, "hipprobe: a submission is still in flight; this process unmaps and"
                        " frees nothing and leaves the device to the operating system."
                        " Restart the display driver or the machine before the next"
                        " trial.\n");
        free(file);
        return EXIT_TIMEOUT;
    }
    if (g_device_lost) {
        exit_code = EXIT_DEVICELOST;
    } else if (g_failures != 0) {
        exit_code = EXIT_MISMATCH;
    }
    bc250hsa_free(st.dev, &st.kernarg);
    bc250hsa_module_unload(st.mod);
    free(file);
    bc250hsa_close(st.dev);
    return exit_code;
}

/* -------------------------------------------------------------------------------
 * The command line
 * ----------------------------------------------------------------------------- */

static void usage(void)
{
    printf("hipprobe - one gfx1013 compute dispatch through the BC-250 kernel driver\n");
    printf("\n");
    printf("  hipprobe [options] [<data directory>]\n");
    printf("\n");
    printf("  --selftest            run everything that needs no adapter, then stop\n");
    printf("  --info                print the device properties, then stop\n");
    printf("  --co <file>           the code object or the clang offload bundle to run\n");
    printf("  --n <count>           elements of vadd and reduce256 (default %u)\n", DEFAULT_N);
    printf("  --grid <x,y,z>        workgroups of writeGridSize (default 7,3,2)\n");
    printf("  --block <x,y,z>       work items per workgroup of writeGridSize"
           " (default 64,2,1)\n");
    printf("  --kernel <name>       run one kernel only\n");
    printf("  --wait-slice <ms>     one wait slice (default %u)\n", DEFAULT_WAIT_SLICE);
    printf("  --wait-total <ms>     the whole bound of one wait (default %u)\n",
           DEFAULT_WAIT_TOTAL);
    printf("  --verbose             send the library log to the error stream\n");
    printf("\n");
    printf("It prints one JSON line per kernel. Exit codes: 0 ok, 1 usage, 2 host failure,"
           " 3 timeout, 4 device lost, 5 result mismatch, 6 adapter failure.\n");
    fflush(stdout);
}

static int parse_triple(const char* text, uint32_t out[3])
{
    unsigned long a = 0;
    unsigned long b = 0;
    unsigned long c = 0;
    if (sscanf(text, "%lu,%lu,%lu", &a, &b, &c) != 3) {
        return 0;
    }
    out[0] = (uint32_t)a; out[1] = (uint32_t)b; out[2] = (uint32_t)c;
    return 1;
}

int main(int argc, char** argv)
{
    options opt;
    int     i;

    memset(&opt, 0, sizeof(opt));
    opt.n = DEFAULT_N;
    opt.grid[0] = 7u; opt.grid[1] = 3u; opt.grid[2] = 2u;
    opt.block[0] = 64u; opt.block[1] = 2u; opt.block[2] = 1u;
    opt.wait_slice_ms = DEFAULT_WAIT_SLICE;
    opt.wait_total_ms = DEFAULT_WAIT_TOTAL;

    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
            return EXIT_OK;
        }
        if (strcmp(a, "--selftest") == 0) { opt.selftest = 1; continue; }
        if (strcmp(a, "--info") == 0) { opt.info = 1; continue; }
        if (strcmp(a, "--verbose") == 0) { opt.verbose = 1; continue; }
        if (strcmp(a, "--co") == 0 && i + 1 < argc) { opt.code_object = argv[++i]; continue; }
        if (strcmp(a, "--kernel") == 0 && i + 1 < argc) { opt.only_kernel = argv[++i]; continue; }
        if (strcmp(a, "--n") == 0 && i + 1 < argc) {
            opt.n = (uint32_t)strtoul(argv[++i], NULL, 10);
            continue;
        }
        if (strcmp(a, "--wait-slice") == 0 && i + 1 < argc) {
            opt.wait_slice_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
            continue;
        }
        if (strcmp(a, "--wait-total") == 0 && i + 1 < argc) {
            opt.wait_total_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
            continue;
        }
        if (strcmp(a, "--grid") == 0 && i + 1 < argc) {
            if (!parse_triple(argv[++i], opt.grid)) { usage(); return EXIT_USAGE; }
            continue;
        }
        if (strcmp(a, "--block") == 0 && i + 1 < argc) {
            if (!parse_triple(argv[++i], opt.block)) { usage(); return EXIT_USAGE; }
            continue;
        }
        if (a[0] == '-') {
            fprintf(stderr, "hipprobe: unknown option %s\n", a);
            usage();
            return EXIT_USAGE;
        }
        opt.data_dir = a;
    }
    if (opt.n == 0u) {
        fprintf(stderr, "hipprobe: --n must be at least 1\n");
        return EXIT_USAGE;
    }
    /* No wait of this tool may run longer than ten seconds, whatever the command line
     * says: a lab trial of step 1 is bounded to three minutes in all. */
    if (opt.wait_total_ms == 0u || opt.wait_total_ms > DEFAULT_WAIT_TOTAL) {
        opt.wait_total_ms = DEFAULT_WAIT_TOTAL;
    }
    if (opt.wait_slice_ms == 0u || opt.wait_slice_ms > opt.wait_total_ms) {
        opt.wait_slice_ms = (opt.wait_total_ms < DEFAULT_WAIT_SLICE) ? opt.wait_total_ms
                                                                     : DEFAULT_WAIT_SLICE;
    }
    if (opt.verbose) {
        bc250hsa_set_log(log_sink, NULL, BC250HSA_LOG_TRACE);
    } else {
        bc250hsa_set_log(log_sink, NULL, BC250HSA_LOG_WARN);
    }

    if (opt.selftest) {
        return run_selftest(&opt);
    }
    return run_device(&opt);
}
