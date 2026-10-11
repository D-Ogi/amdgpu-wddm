/* Actual submit.c and PM4 builder, with only the Windows kernel calls replaced.
 * The test never opens a device or executes a GPU command. */
#define D3DKMTEscape JournalTestEscape
#define D3DKMTSubmitCommand JournalTestSubmit
#define D3DKMTWaitForSynchronizationObjectFromCpu JournalTestWait
#include "../../bc250hsa/submit.c"
#include <stdio.h>

static unsigned checks, failures, uploads, submits, cancels, last_count;
static int escape_mode, submit_fail, expect_record;
static uint64_t completed, next_upload, pending_upload;
static BC250_HIP_JOURNAL_UPLOAD saved_header;
static unsigned char saved_payload[BC250_HIP_JOURNAL_MAX_BYTES];
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

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
int bc250hsa_device_executing(struct bc250hsa_device* d) { return !d->device_lost; }
void bc250hsa_write_barrier(void) { MemoryBarrier(); }
bc250hsa_status bc250hsa_device_allocator(bc250hsa_device* d, bc250hsa_allocator* a)
{ (void)d; (void)a; return BC250HSA_EUNSUPPORTED; }

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
    completed = uploads = submits = cancels = last_count = 0;
    next_upload = pending_upload = 0; escape_mode = submit_fail = 0; expect_record = 1;
}
static void finish(struct bc250hsa_device* d)
{
    free(d->ring.host); free(d->slot_fence); free(d->last_ib); free(d->journal_upload);
    DeleteCriticalSection(&d->lock);
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

    init(&dev, 1);
    bytes = make_record(record, &d, 28); record[0] ^= 1;
    CHECK(SEND() == BC250HSA_EINVAL && !dev.batch_open && uploads == 0 && submits == 0);
    bytes = make_record(record, &d, 28); d.kernarg_va += 16;
    CHECK(SEND() == BC250HSA_EINVAL && !dev.batch_open && uploads == 0 && submits == 0);
    d.kernarg_va -= 16;
    finish(&dev);
    printf("submit journal: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
