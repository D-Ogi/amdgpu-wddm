/* Actual submit.c and PM4 builder, with only the Windows kernel calls replaced.
 * The test never opens a device or executes a GPU command. */
#define D3DKMTEscape JournalTestEscape
#define D3DKMTSubmitCommand JournalTestSubmit
#define D3DKMTWaitForSynchronizationObjectFromCpu JournalTestWait
#define D3DKMTGetDeviceState JournalTestGetDeviceState
#include "../../bc250hsa/submit.c"
#include <stdio.h>

static unsigned checks, failures, uploads, submits, cancels, last_count;
static int escape_mode, submit_fail, expect_record;
static DWORD submit_sleep_ms; /* makes a fake submission long enough for its ticks to be seen */
static uint64_t completed, next_upload, pending_upload;
static D3DKMT_HANDLE last_context;
static BC250_HIP_JOURNAL_UPLOAD saved_header;
static unsigned char saved_payload[BC250_HIP_JOURNAL_MAX_BYTES];
/* The device of the case, for the device-state fake; the query's answer and its count. */
static struct bc250hsa_device* state_dev;
static NTSTATUS state_status;
static unsigned state_calls;
/* The last error line of the library. */
static char last_error[512];
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

static void capture_log(void* ctx, uint32_t level, const char* message)
{
    (void)ctx;
    if (level == BC250HSA_LOG_ERROR) {
        strncpy(last_error, message, sizeof(last_error) - 1u);
        last_error[sizeof(last_error) - 1u] = '\0';
    }
}

/* The real bc250hsa_device_execution_state and bc250hsa_device_executing of submit.c run over
 * this: the execution state of the device the case opened, 4 (stopped) once it is lost. */
NTSTATUS APIENTRY JournalTestGetDeviceState(D3DKMT_GETDEVICESTATE* s)
{
    ++state_calls;
    CHECK(state_dev != NULL && s->hDevice == state_dev->device);
    CHECK(s->StateType == D3DKMT_DEVICESTATE_EXECUTION);
    if (state_status != STATUS_SUCCESS) return state_status;
    s->ExecutionState = (state_dev != NULL && state_dev->device_lost)
                            ? D3DKMT_DEVICEEXECUTION_STOPPED : D3DKMT_DEVICEEXECUTION_ACTIVE;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY JournalTestEscape(const D3DKMT_ESCAPE* e)
{
    BC250_HIP_JOURNAL_UPLOAD* h = (BC250_HIP_JOURNAL_UPLOAD*)e->pPrivateDriverData;
    CHECK(e->hAdapter == 11 && e->hDevice == 12 && e->hContext == 13);
    CHECK(e->Type == D3DKMT_ESCAPE_DRIVERPRIVATE && e->Flags.Value == 8u);
    CHECK(h->Magic == BC250_HIP_JOURNAL_MAGIC && h->Command == 35u);
    CHECK(h->AbiVersion == 1 && h->TotalBytes == e->PrivateDriverDataSize);
    if (h->Operation == BC250_HIP_JOURNAL_CANCEL_OP) {
        ++cancels;
        CHECK(h->UploadId == pending_upload && h->TotalBytes == sizeof(*h));
        pending_upload = 0;
        h->Status = h->NtStatus = 0;
        return STATUS_SUCCESS;
    }
    ++uploads;
    if (escape_mode == 1) return STATUS_NOT_SUPPORTED;
    if (escape_mode == 2) return STATUS_SUCCESS; /* Old KMD did not process it. */
    CHECK(h->Operation == BC250_HIP_JOURNAL_UPLOAD_OP);
    CHECK(h->TotalBytes <= sizeof(saved_payload) && h->RecordCount > 0);
    {
        uint32_t offset = sizeof(*h), i;
        uint64_t previous_id = 0;
        for (i = 0; i < h->RecordCount; ++i) {
            BC250_HIP_DISPATCH_RECORD r;
            CHECK(offset <= h->TotalBytes && sizeof(r) <= h->TotalBytes - offset);
            memcpy(&r, (const unsigned char*)h + offset, sizeof(r));
            CHECK(r.Bytes <= h->TotalBytes - offset &&
                  Bc250HipRecordValid((const unsigned char*)h + offset, r.Bytes));
            CHECK(r.DispatchId > previous_id);
            previous_id = r.DispatchId;
            offset += r.Bytes;
        }
        CHECK(offset == h->TotalBytes);
    }
    saved_header = *h;
    memcpy(saved_payload, h, h->TotalBytes);
    last_count = h->RecordCount;
    pending_upload = h->UploadId = ++next_upload;
    h->Status = h->NtStatus = 0;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY JournalTestSubmit(const D3DKMT_SUBMITCOMMAND* c)
{
    const struct bc250hsa_submit_blob* b = (const struct bc250hsa_submit_blob*)c->pPrivateDriverData;
    ++submits;
    last_context = c->BroadcastContext[0];
    if (submit_sleep_ms) Sleep(submit_sleep_ms);
    if (expect_record) {
        CHECK(pending_upload != 0);
        CHECK(saved_header.IbVa == c->Commands && saved_header.IbVa == b->ib[0].va_start);
        CHECK(saved_header.FenceVa == b->fence_va && saved_header.FenceValue == b->fence_value);
    }
    if (submit_fail) return STATUS_INVALID_PARAMETER;
    pending_upload = 0;
    completed = b->fence_value;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY JournalTestWait(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU* w)
{
    (void)w;
    CHECK(0); /* All successful submissions retire at once in this fixture. */
    return STATUS_NOT_SUPPORTED;
}
bc250hsa_status bc250hsa_os_status(const char* what, NTSTATUS status)
{ (void)what; bc250hsa_set_os_status(status); return BC250HSA_EOS; }
void bc250hsa_mark_lost(struct bc250hsa_device* d, const char* why)
{ (void)why; d->device_lost = 1; }
void bc250hsa_write_barrier(void) { MemoryBarrier(); }
bc250hsa_status bc250hsa_device_allocator(bc250hsa_device* d, bc250hsa_allocator* a)
{ (void)d; (void)a; return BC250HSA_EUNSUPPORTED; }
bc250hsa_status bc250hsa_context_create(struct bc250hsa_device* d, D3DKMT_HANDLE* out)
{ (void)d; *out = 0x701; return BC250HSA_OK; }

static void init(struct bc250hsa_device* d, int batch)
{
    memset(d, 0, sizeof(*d));
    InitializeCriticalSection(&d->lock);
    d->adapter = 11; d->device = 12; d->context = 13;
    d->submit_capable = 1; d->lds_bytes_per_workgroup = 65536;
    d->ring_slots = 8; d->ring_slot_bytes = 65536;
    d->ring.host = calloc(d->ring_slots, d->ring_slot_bytes); d->ring.va = 0x4000000000ull;
    d->slot_fence = (uint64_t*)calloc(d->ring_slots, sizeof(uint64_t));
    d->last_ib_capacity = d->ring_slot_bytes / 4;
    d->last_ib = (uint32_t*)calloc(d->last_ib_capacity, 4);
    d->fence_cpu = &completed; d->fence_gpu_va = 0x4100000000ull;
    d->batch.enabled = batch; d->batch.max_dispatches = 100; d->batch.max_hold_us = UINT32_MAX;
    state_dev = d; state_status = STATUS_SUCCESS;
    completed = uploads = submits = cancels = last_count = 0;
    next_upload = pending_upload = 0; escape_mode = submit_fail = 0; expect_record = 1;
}
static void finish(struct bc250hsa_device* d)
{
    free(d->ring.host); free(d->slot_fence); free(d->last_ib); free(d->journal_upload);
    DeleteCriticalSection(&d->lock);
}
static int read_counters(bc250hsa_counters* c)
{
    memset(c, 0, sizeof(*c)); c->struct_bytes = sizeof(*c);
    return bc250hsa_counters_read(c) == BC250HSA_OK;
}
/* One of four threads that raise one maximum at once with interleaved values. */
#define MAX_THREADS 4u
#define MAX_ROUNDS 200000u
static DWORD WINAPI raise_max(LPVOID arg)
{
    const uint64_t first = (uint64_t)(uintptr_t)arg;
    uint64_t i;
    for (i = 0; i < MAX_ROUNDS; ++i) {
        bc250hsa_count_max(BC250HSA_C_WAITS_OTHER_TICKS_MAX, i * MAX_THREADS + first + 1u);
        bc250hsa_count_add(BC250HSA_C_WAITS_OTHER_TICKS, 1u);
    }
    return 0;
}
static uint32_t make_record(unsigned char* bytes, bc250hsa_dispatch* d, uint32_t kernarg_bytes)
{
    BC250_HIP_DISPATCH_RECORD r;
    memset(&r, 0, sizeof(r));
    r.SymbolOffset = sizeof(r); r.SymbolBytes = 5;
    r.KernargOffset = 104; r.KernargBytes = kernarg_bytes;
    r.BindingsOffset = (r.KernargOffset + kernarg_bytes + 7u) & ~7u;
    r.Bytes = r.BindingsOffset;
    r.EntryVa = d->kernel->entry_va; r.DescriptorVa = d->kernel->descriptor_va;
    r.KernargVa = d->kernarg_va; r.DispatchId = 1;
    memcpy(r.Grid, d->launch.grid, sizeof(r.Grid)); memcpy(r.Block, d->launch.block, sizeof(r.Block));
    memset(bytes, 0, r.Bytes); memcpy(bytes, &r, sizeof(r)); memcpy(bytes + r.SymbolOffset, "vadd", 5);
    return r.Bytes;
}
static bc250hsa_status send_record(struct bc250hsa_device* dev, bc250hsa_dispatch* dispatch,
                                   unsigned char* record, uint32_t bytes, uint64_t* fence)
{
    static uint64_t dispatch_id;
    uint64_t id = ++dispatch_id;
    memcpy(record + offsetof(BC250_HIP_DISPATCH_RECORD, DispatchId), &id, sizeof(id));
    return bc250hsa_dispatch_submit_recorded(dev, dispatch, record, bytes, fence);
}
int main(void)
{
    struct bc250hsa_device dev;
    bc250hsa_kernel k;
    bc250hsa_dispatch d;
    unsigned char record[BC250_HIP_JOURNAL_MAX_RECORD_BYTES];
    uint32_t bytes, i;
    uint64_t fence;
    memset(&k, 0, sizeof(k)); memset(&d, 0, sizeof(d));
    k.name = "vadd"; k.descriptor_va = 0x140ABCD0C80ull; k.entry_va = 0x140ABCD1E00ull;
    k.kernarg_bytes = 28; k.kernarg_align = 16; k.max_flat_workgroup_size = 1024;
    k.sgpr_count = 9; k.vgpr_count = 8; k.wave_size = 32; k.workgroup_processor_mode = 1;
    k.user_sgpr_count = 6; k.compute_pgm_rsrc1 = 0xE0AF0000u; k.compute_pgm_rsrc2 = 0x8Cu;
    k.kernel_code_properties = 0x0409;
    d.struct_bytes = sizeof(d); d.kernel = &k; d.kernarg_va = 0x140ABCD2000ull;
    d.launch.struct_bytes = sizeof(d.launch); d.launch.grid[0] = 4096;
    d.launch.grid[1] = d.launch.grid[2] = d.launch.block[1] = d.launch.block[2] = 1;
    d.launch.block[0] = 256;
    bytes = make_record(record, &d, 28);
#define SEND() send_record(&dev, &d, record, bytes, &fence)
    init(&dev, 0);
    CHECK(SEND() == BC250HSA_OK && fence == 1 && uploads == 1 && submits == 1);
    CHECK(last_count == 1 && !dev.journal_count);
    CHECK(bc250hsa_dispatch_submit(&dev, &d, &fence) == BC250HSA_EUNSUPPORTED && submits == 1);
    finish(&dev);

    for (i = 1; i <= 2; ++i) {
        init(&dev, 0); escape_mode = (int)i;
        CHECK(SEND() != BC250HSA_OK && fence == 0 && uploads == 1 && submits == 0);
        finish(&dev);
    }
    init(&dev, 0); submit_fail = 1;
    CHECK(SEND() != BC250HSA_OK && fence == 0 && cancels == 1 && pending_upload == 0);
    finish(&dev);

    init(&dev, 1);
    CHECK(SEND() == BC250HSA_OK && uploads == 0 && submits == 0);
    record[104] = 99; /* Mutation after acceptance must not alter the staged bytes. */
    CHECK(bc250hsa_flush(&dev, &fence) == BC250HSA_OK && uploads == 1 && submits == 1);
    CHECK(saved_payload[sizeof(BC250_HIP_JOURNAL_UPLOAD) + 104] == 0);
    record[104] = 0;
    for (i = 0; i < 33; ++i) CHECK(SEND() == BC250HSA_OK);
    CHECK(uploads == 2 && last_count == 32);
    CHECK(bc250hsa_flush(&dev, &fence) == BC250HSA_OK && uploads == 3 && last_count == 1);
    finish(&dev);

    init(&dev, 1);
    bytes = make_record(record, &d, BC250_HIP_JOURNAL_MAX_KERNARG);
    for (i = 0; i < 8; ++i) CHECK(SEND() == BC250HSA_OK);
    CHECK(uploads == 1 && last_count == 7);
    CHECK(bc250hsa_flush(&dev, &fence) == BC250HSA_OK && uploads == 2 && last_count == 1);
    finish(&dev);

    init(&dev, 1); escape_mode = 1;
    CHECK(SEND() == BC250HSA_OK);
    CHECK(bc250hsa_flush(&dev, &fence) != BC250HSA_OK && submits == 0 && dev.device_lost);
    CHECK(SEND() == BC250HSA_EDEVICELOST && submits == 0 && fence == 0);
    finish(&dev);

    /* The journal rule of submit_ib. Once a recorded dispatch turned the journal on, a buffer
     * with no record never goes to the main context, whose kernel-side journal would refuse
     * it and take the device with it: a copy goes to the copy context with no upload, and a
     * buffer with no record aimed at the main context is refused with nothing sent. */
    init(&dev, 0);
    CHECK(SEND() == BC250HSA_OK && dev.journal_required && last_context == 13);
    {
        bc250hsa_copy_op op;
        bc250hsa_op_receipt rc;
        memset(&op, 0, sizeof(op)); memset(&rc, 0, sizeof(rc));
        /* A DMA_DATA address has 40 bits. */
        op.kind = BC250HSA_COPY_OP_COPY; op.src_va = 0x1000000000ull;
        op.dst_va = 0x1000010000ull; op.bytes = 4096;
        expect_record = 0;
        CHECK(bc250hsa_copy_run_ops(&dev, &op, 1u, &rc) == BC250HSA_OK);
        CHECK(rc.state == BC250HSA_OP_RETIRED && rc.fence_value == 2 && completed == 2);
        CHECK(uploads == 1 && submits == 2 && last_context == 0x701 && dev.copy_context == 0x701);
        {
            /* The refusal is never silent: one error line names the context and the buffer,
             * and journal_guard_refusals counts it. */
            bc250hsa_counters g0, g1;
            CHECK(read_counters(&g0));
            last_error[0] = '\0';
            bc250hsa_set_log(capture_log, NULL, BC250HSA_LOG_ERROR);
            CHECK(submit_ib(&dev, 0u, 16u, dev.fence_last_submitted + 1u, dev.context, 1) ==
                  BC250HSA_EUNSUPPORTED);
            bc250hsa_set_log(NULL, NULL, 0u);
            CHECK(read_counters(&g1));
            CHECK(g1.journal_guard_refusals - g0.journal_guard_refusals == 1u);
            CHECK(g1.submissions_refused - g0.submissions_refused == 1u);
            CHECK(strstr(last_error, "journal guard: refused an indirect buffer with no"
                                     " dispatch record for the main context 0xd") != NULL);
            CHECK(strstr(last_error, "a copy-only buffer, fence value 3, ring slot 0 at"
                                     " 0x4000000000, 16 dwords, 0 dispatches open;") != NULL);
            CHECK(strstr(last_error, "last submission 2, copy context 0x701") != NULL);
            CHECK(strstr(last_error, "nothing is submitted") != NULL);
        }
        CHECK(submits == 2 && uploads == 1 && !dev.device_lost);
        expect_record = 1;
        CHECK(SEND() == BC250HSA_OK && fence == 3 && uploads == 2 && last_context == 13);
    }
    finish(&dev);

    /* The device-state query that the fence checks print: the real call of submit.c, with
     * the device's handle and the execution type, and a failed query that says its status
     * and is no evidence of a loss. */
    init(&dev, 0);
    state_calls = 0;
    CHECK(bc250hsa_device_execution_state(&dev) == D3DKMT_DEVICEEXECUTION_ACTIVE);
    CHECK(bc250hsa_device_executing(&dev) == 1 && state_calls == 2);
    dev.device_lost = 1;
    CHECK(bc250hsa_device_execution_state(&dev) == D3DKMT_DEVICEEXECUTION_STOPPED);
    CHECK(bc250hsa_device_executing(&dev) == 0);
    state_status = STATUS_INVALID_PARAMETER;
    CHECK(bc250hsa_device_execution_state(&dev) == (int32_t)STATUS_INVALID_PARAMETER);
    CHECK(bc250hsa_device_executing(&dev) == 1 && state_calls == 6);
    finish(&dev);

    init(&dev, 1);
    bytes = make_record(record, &d, 28); record[0] ^= 1;
    CHECK(SEND() == BC250HSA_EINVAL && !dev.batch_open && uploads == 0 && submits == 0);
    bytes = make_record(record, &d, 28); d.kernarg_va += 16;
    CHECK(SEND() == BC250HSA_EINVAL && !dev.batch_open && uploads == 0 && submits == 0);
    d.kernarg_va -= 16;
    finish(&dev);

    /* The duration counters of bc250hsa.h section 3. Every call into the kernel driver is
     * counted and timed, the refused one too, and only the time inside it: each fake call
     * sleeps 20 ms, so two of them are at least 10 ms of ticks on any timer resolution. A wait
     * adds its time to exactly the line waits_fast says it belongs to. Then four threads raise
     * one maximum at once, and the largest value must be the one that stays. */
    {
        bc250hsa_counters c0, c1;
        HANDLE threads[MAX_THREADS];
        bytes = make_record(record, &d, 28);
        bc250hsa_counters_reset();
        CHECK(read_counters(&c0) && c0.ticks_per_second != 0 && c0.submit_calls == 0);
        init(&dev, 0); submit_sleep_ms = 20;
        CHECK(SEND() == BC250HSA_OK && submits == 1);
        submit_fail = 1;
        CHECK(SEND() != BC250HSA_OK && submits == 2);
        submit_fail = 0; submit_sleep_ms = 0;
        CHECK(read_counters(&c1));
        CHECK(c1.submit_calls - c0.submit_calls == 2 && c1.submissions - c0.submissions == 1);
        CHECK(c1.submit_ticks - c0.submit_ticks >= c1.ticks_per_second / 100u);
        c0 = c1;
        CHECK(bc250hsa_wait(&dev, completed, 10, 10) == BC250HSA_OK);
        CHECK(read_counters(&c1) && c1.waits_fast - c0.waits_fast == 1);
        CHECK(c1.waits_fast_ticks >= c0.waits_fast_ticks);
        CHECK(c1.waits_other_ticks == c0.waits_other_ticks);
        CHECK(c1.waits_fast_ticks_max <= c1.waits_fast_ticks);
        c0 = c1;
        dev.device_lost = 1;
        CHECK(bc250hsa_wait(&dev, completed + 1, 10, 10) == BC250HSA_EDEVICELOST);
        CHECK(read_counters(&c1) && c1.waits - c0.waits == 1 && c1.waits_fast == c0.waits_fast);
        CHECK(c1.waits_fast_ticks == c0.waits_fast_ticks);
        CHECK(c1.waits_other_ticks >= c0.waits_other_ticks);
        CHECK(c1.waits_other_ticks_max <= c1.waits_other_ticks);
        finish(&dev);

        bc250hsa_counters_reset();
        bc250hsa_count_max(BC250HSA_C_WAITS_FAST_TICKS_MAX, 7u);
        bc250hsa_count_max(BC250HSA_C_WAITS_FAST_TICKS_MAX, 3u);
        CHECK(read_counters(&c1) && c1.waits_fast_ticks_max == 7u);
        bc250hsa_count_cpu_copy_fallback(4096u, 25u);
        CHECK(read_counters(&c1) && c1.cpu_copy_fallback == 1u &&
              c1.cpu_copy_fallback_bytes == 4096u && c1.cpu_copy_fallback_ticks == 25u);
        for (i = 0; i < MAX_THREADS; ++i) {
            threads[i] = CreateThread(NULL, 0, raise_max, (LPVOID)(uintptr_t)i, 0, NULL);
            CHECK(threads[i] != NULL);
        }
        CHECK(WaitForMultipleObjects(MAX_THREADS, threads, TRUE, 60000) == WAIT_OBJECT_0);
        for (i = 0; i < MAX_THREADS; ++i) CloseHandle(threads[i]);
        CHECK(read_counters(&c1));
        CHECK(c1.waits_other_ticks_max == (uint64_t)MAX_ROUNDS * MAX_THREADS);
        CHECK(c1.waits_other_ticks == (uint64_t)MAX_ROUNDS * MAX_THREADS);
        bc250hsa_counters_reset();
        CHECK(read_counters(&c1) && c1.waits_other_ticks_max == 0 && c1.waits_fast_ticks_max == 0);
        CHECK(c1.cpu_copy_fallback_ticks == 0 && c1.submit_calls == 0 && c1.ticks_per_second != 0);
    }
    printf("submit journal: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
