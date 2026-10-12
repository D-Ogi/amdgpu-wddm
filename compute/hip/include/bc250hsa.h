/* bc250hsa.h - the gfx1013 compute submission layer of the BC-250 Windows driver.
 *
 * Milestone M16, gate G5, route B (our own thin HIP runtime). The design that this
 * header belongs to is docs/design/m16-hip-route-b.md. Layer 1 of that design builds
 * this interface. Layer 2 (amdhip64.dll) uses this interface and nothing else of the
 * submission path. The hipBLAS shim and the step-1 host tool use it as well.
 *
 * What this layer owns: one WDDM device on the BC-250 adapter, its memory, the code
 * object loader, the kernel argument packer, the PM4 dispatch stream, the batching of
 * dispatches into one indirect buffer, one monitored fence and one bounded wait.
 *
 * What this layer does not own: HIP semantics, streams, events, modules per process,
 * and the offload bundle policy of a compiler. Those belong to layer 2.
 *
 * Rules of the interface:
 *   1. Every function returns BC250HSA_OK (0) or a negative bc250hsa_status.
 *      bc250hsa_fence_read and bc250hsa_status_string are the two exceptions; they
 *      cannot fail.
 *   2. No call waits without a bound. A wait states its slice and its total.
 *   3. No HIP type, no Mesa type and no D3DKMT type appears here. A test build can
 *      therefore replace the device half with a mock and keep the loader.
 *   4. Every structure that the caller allocates and fills carries struct_bytes as its
 *      first field. The library refuses a size it does not know with BC250HSA_EINVAL.
 *      Structures that the library owns (bc250hsa_kernel, bc250hsa_arg) carry no size
 *      field; BC250HSA_ABI_VERSION covers them. bc250hsa_mem is a plain handle.
 *   5. One device serialises its submissions. The kernel driver runs one indirect
 *      buffer at a time (driver/kmd/wddm.c, "One IB is already the ring's whole
 *      capacity"), so a second dispatch waits for a free ring slot under the same
 *      bound as bc250hsa_wait.
 *   6. Nothing in this header reads or writes a registry value, a file or an
 *      environment variable. The caller passes every policy as a parameter.
 */

#ifndef BC250HSA_H
#define BC250HSA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version of this interface. A build of layer 2 records it and refuses a library
 * that reports another major value. bc250hsa_abi_version() returns the value that the
 * library was built with. */
#define BC250HSA_ABI_VERSION_MAJOR 1u
#define BC250HSA_ABI_VERSION_MINOR 8u   /* 1.1 added section 8.1, batching;
                                         * 1.2 added section 7.1, the AQL dispatch packet
                                         * that ENABLE_SGPR_DISPATCH_PTR needs;
                                         * 1.3 adds fixed-private scratch PM4 environments;
                                         * 1.4 adds retained dispatch records;
                                         * 1.5 adds section 5.1, the GPU copy and fill:
                                         * bc250hsa_copy_device_to_device,
                                         * bc250hsa_copy_from_device_gpu,
                                         * bc250hsa_fill_device,
                                         * bc250hsa_gpu_copy_ready, and the copy
                                         * counters of bc250hsa_counters;
                                         * 1.6 adds the duration counters of
                                         * bc250hsa_counters, bc250hsa_ticks, and the
                                         * ticks argument of
                                         * bc250hsa_count_cpu_copy_fallback;
                                         * 1.7 adds bc250hsa_op_receipt, the last
                                         * argument of the three calls of section 5.1;
                                         * 1.8 adds the exclusion of an unretired copy:
                                         * bc250hsa_copy_unretired,
                                         * bc250hsa_count_cpu_copy_exclusion_refusal,
                                         * and the counters after
                                         * cpu_copy_fallback_ticks */

uint32_t bc250hsa_abi_version_major(void);
uint32_t bc250hsa_abi_version_minor(void);

/* ---------------------------------------------------------------------------------
 * 1. Status
 * ------------------------------------------------------------------------------- */

typedef enum bc250hsa_status {
    BC250HSA_OK = 0,

    /* General */
    BC250HSA_EINVAL = -1,        /* a parameter, a size field or an alignment is wrong */
    BC250HSA_ENOMEM = -2,        /* no host memory, or no device memory of that size */
    BC250HSA_ENODEV = -3,        /* no BC-250 adapter, or the device is already closed */
    BC250HSA_ETIMEOUT = -4,      /* the bounded wait ran out and the device still lives */
    BC250HSA_EDEVICELOST = -5,   /* a reset, a page fault, or the fence read UINT64_MAX */
    BC250HSA_EUNSUPPORTED = -6,  /* the request needs a part of the design not built yet */
    BC250HSA_EOS = -7,           /* a Windows call failed; bc250hsa_last_os_status() has it */
    BC250HSA_ENOTFOUND = -8,     /* no kernel, no symbol or no bundle entry of that name */
    BC250HSA_EBUSY = -9,         /* no free command ring slot inside the bound */

    /* Code object and bundle */
    BC250HSA_EBADELF = -20,          /* not ELF64, not EM_AMDGPU, or not ELFOSABI_AMDGPU_HSA */
    BC250HSA_EWRONGTARGET = -21,     /* e_flags names another processor than this device */
    BC250HSA_EBADABIVERSION = -22,   /* EI_ABIVERSION is not 3 (v5) and not 4 (v6) */
    BC250HSA_EBADRELOC = -23,        /* a type other than R_AMDGPU_RELATIVE64, or a named symbol */
    BC250HSA_EBADMETADATA = -24,     /* no NT_AMDGPU_METADATA note, or it does not parse */
    BC250HSA_EBADBUNDLE = -25,       /* the offload bundle header does not parse */
    BC250HSA_ECOMPRESSEDBUNDLE = -26 /* a compressed bundle ("CCOB"); rebuild without
                                      * --offload-compress */
} bc250hsa_status;

/* A short English name of a status. Never NULL. */
const char* bc250hsa_status_string(bc250hsa_status status);

/* The NTSTATUS of the last Windows call that failed on this thread, or 0. Only
 * meaningful right after a call returned BC250HSA_EOS. */
int32_t bc250hsa_last_os_status(void);

/* ---------------------------------------------------------------------------------
 * 2. Log hook
 * ------------------------------------------------------------------------------- */

#define BC250HSA_LOG_ERROR 0u
#define BC250HSA_LOG_WARN  1u
#define BC250HSA_LOG_INFO  2u
#define BC250HSA_LOG_TRACE 3u

typedef void (*bc250hsa_log_fn)(void* ctx, uint32_t level, const char* message);

/* Installs one log sink for the process. fn NULL removes it. The library never writes
 * to a stream of its own. Messages hold no application data. */
void bc250hsa_set_log(bc250hsa_log_fn fn, void* ctx, uint32_t max_level);

/* ---------------------------------------------------------------------------------
 * 3. Counters
 * ------------------------------------------------------------------------------- */

/* Process counters. They answer design questions without a debugger, and
 * hostcall_buffer_requests is the measurement that answers kill criterion K4 of the
 * route document, which is device-side printf. */
typedef struct bc250hsa_counters {
    uint32_t struct_bytes;
    uint64_t modules_loaded;
    uint64_t dispatches_built;
    uint64_t submissions;
    uint64_t submissions_refused;
    uint64_t waits;
    uint64_t waits_fast;              /* the fence mapping already held the value */
    uint64_t waits_timed_out;
    uint64_t device_losses;
    uint64_t hidden_args_zeroed;
    uint64_t unknown_arg_kinds;
    uint64_t hostcall_buffer_requests;
    uint64_t dynamic_stack_refusals;
    /* Section 8.1. submissions / dispatches_built is the number this interface is
     * measured by: one submission per dispatch is one user-to-kernel transition per
     * kernel. batches_submitted counts the submissions that carried more than one
     * dispatch, and dispatches_batched the dispatches that rode in them. */
    uint64_t batches_submitted;
    uint64_t dispatches_batched;
    /* Section 5.1, the GPU copy and fill. A call of section 5.1 that succeeds lands on
     * exactly one of gpu_copy_d2h, gpu_copy_d2h_staged, gpu_copy_d2d and gpu_fill.
     * cpu_copy_fallback is what a caller reports through
     * bc250hsa_count_cpu_copy_fallback: a device-to-host or device-to-device copy, or a
     * fill, that the caller did with the host processor. The five lines are not the whole
     * copy traffic: a host-to-device copy, a host-to-host copy and a host-processor fill
     * that was never sent to the GPU route are on none of them. A cpu_copy_fallback above
     * zero is expected below a caller's size threshold and with the route switched off;
     * the caller's log names the reason one time per reason.
     *
     * gpu_copy_packets over the three GPU lines is bytes per packet, which is how a
     * chunked copy becomes visible. gpu_copy_forced_flushes counts the open batches a
     * copy submitted before they were full: a copy waits for everything submitted before
     * it, so a batch still open goes out first. gpu_copy_control_failed is 0 or 1: the
     * coherence control of bc250hsa_gpu_copy_ready. */
    uint64_t gpu_copy_d2h;
    uint64_t gpu_copy_d2h_bytes;
    uint64_t gpu_copy_d2h_staged;
    uint64_t gpu_copy_d2h_staged_bytes;
    uint64_t gpu_copy_d2d;
    uint64_t gpu_copy_d2d_bytes;
    uint64_t gpu_fill;
    uint64_t gpu_fill_bytes;
    uint64_t cpu_copy_fallback;
    uint64_t cpu_copy_fallback_bytes;
    uint64_t gpu_copy_packets;
    uint64_t gpu_copy_forced_flushes;
    uint64_t gpu_copy_control_failed;
    /* Durations, in ticks of bc250hsa_ticks. ticks_per_second converts them, so one read
     * is enough to turn every total into time. A total divided by its count is the mean
     * cost of one call.
     *
     * The waits are the waits counted in waits, split as waits_fast splits them:
     * waits_fast_ticks is the time of the waits the fence mapping answered, and
     * waits_other_ticks the time of every other wait, which includes the timed-out and
     * the lost ones. A wait that has to submit the open batch first carries that
     * submission in its own time. Each of the two has its longest single wait beside it.
     *
     * submit_calls and submit_ticks are the D3DKMTSubmitCommand calls, the refused ones
     * included, and the time spent inside them and nothing else: the user-to-kernel
     * transition and the kernel driver's work for one indirect buffer.
     *
     * The five copy totals belong to the five copy counts above and are added where those
     * are: one call, from its entry to its return, its submission and its fence wait
     * included. The first call of a process that writes a host-readable destination
     * carries the coherence control of section 5.1. A call that fails adds nothing; when
     * layer 2 then
     * takes the host-processor path, the time of the failed attempt is part of the
     * cpu_copy_fallback_ticks that bc250hsa_count_cpu_copy_fallback reports. */
    uint64_t ticks_per_second;
    uint64_t waits_fast_ticks;
    uint64_t waits_fast_ticks_max;
    uint64_t waits_other_ticks;
    uint64_t waits_other_ticks_max;
    uint64_t submit_calls;
    uint64_t submit_ticks;
    uint64_t gpu_copy_d2h_ticks;
    uint64_t gpu_copy_d2h_staged_ticks;
    uint64_t gpu_copy_d2d_ticks;
    uint64_t gpu_fill_ticks;
    uint64_t cpu_copy_fallback_ticks;
    /* What a GPU copy costs the other threads of the process. A copy of section 5.1 holds
     * the device lock from the submission of the open batch to the retirement of its own
     * fence value, because both of the device's contexts write one fence. gpu_copy_lock_holds
     * counts those holds (a staged copy is one hold for all its rounds), and
     * gpu_copy_lock_ticks is their time, the longest beside it. gpu_copy_lock_waits counts
     * the calls of another thread that found the lock held by a copy and waited for it, and
     * gpu_copy_lock_wait_ticks is the time they waited, the longest beside it. A process that
     * calls layer 1 from one thread only has 0 waits whatever it copies.
     *
     * journal_guard_refusals counts the indirect buffers that layer 1 refused itself because
     * they carry no dispatch record and were meant for the main context while its journal is
     * on. Each one is also an error line that names the context and the buffer. It is 0 in a
     * correct build.
     *
     * gpu_copy_unretired counts the copies of section 5.1 whose wait ended without
     * establishing retirement on a device that still lives (receipt SUBMITTED). Each one holds
     * every later submission of the device until the fence shows that it retired.
     * gpu_copy_exclusion_refusals counts the dispatches and copies refused because such a copy
     * still had not retired after a wait for it under the device's bound, and
     * gpu_copy_late_retirements the unretired copies that a later submission or a
     * bc250hsa_copy_unretired found retired, after which submissions went on. Each of the
     * three is also a "copy exclusion:" line.
     *
     * cpu_copy_exclusion_refusals counts the host-processor copies and fills that a caller
     * refused, with no byte moved, because such a copy had not retired or the device was
     * lost (bc250hsa_copy_unretired, section 5.1). The caller reports each one through
     * bc250hsa_count_cpu_copy_exclusion_refusal. */
    uint64_t gpu_copy_lock_holds;
    uint64_t gpu_copy_lock_ticks;
    uint64_t gpu_copy_lock_ticks_max;
    uint64_t gpu_copy_lock_waits;
    uint64_t gpu_copy_lock_wait_ticks;
    uint64_t gpu_copy_lock_wait_ticks_max;
    uint64_t journal_guard_refusals;
    uint64_t gpu_copy_unretired;
    uint64_t gpu_copy_exclusion_refusals;
    uint64_t gpu_copy_late_retirements;
    uint64_t cpu_copy_exclusion_refusals;
} bc250hsa_counters;

bc250hsa_status bc250hsa_counters_read(bc250hsa_counters* out);
void            bc250hsa_counters_reset(void);

/* The clock of every duration counter: QueryPerformanceCounter, in its own ticks.
 * bc250hsa_counters::ticks_per_second is its frequency. A caller that reports a duration
 * of its own (bc250hsa_count_cpu_copy_fallback) measures it with this clock. */
uint64_t bc250hsa_ticks(void);

/* The caller says that a copy or a fill of this many bytes took the host-processor path
 * of section 5 instead of the command processor of section 5.1, and that it took this
 * many ticks of bc250hsa_ticks from the caller's decision to its last byte. The choice
 * belongs to the caller, so the count has to as well; without this call cpu_copy_fallback
 * could never reach the number of bytes that a run moved slowly. A caller reports a copy
 * here only after the host processor finished it: a host-processor copy that failed moved
 * nothing and is not counted. */
void bc250hsa_count_cpu_copy_fallback(uint64_t bytes, uint64_t ticks);

/* The caller refused a host-processor copy or fill because bc250hsa_copy_unretired did not
 * return BC250HSA_OK, and moved no byte (section 5.1). */
void bc250hsa_count_cpu_copy_exclusion_refusal(void);

/* ---------------------------------------------------------------------------------
 * 4. Device
 * ------------------------------------------------------------------------------- */

typedef struct bc250hsa_device bc250hsa_device;
typedef struct bc250hsa_module bc250hsa_module;

#define BC250HSA_OPEN_NO_GPU_SUBMIT 0x1u /* open everything but the context and the fence;
                                          * for a host test of the loader on a machine
                                          * with no BC-250 adapter */

typedef struct bc250hsa_open_params {
    uint32_t struct_bytes;
    uint32_t flags;
    uint32_t luid_low;            /* 0 and 0: the first BC-250 adapter */
    int32_t  luid_high;
    uint32_t ring_slots;          /* command buffer slots, 0 takes 8 */
    uint32_t ring_slot_bytes;     /* 0 takes 65536 */
    uint32_t va_window_gib;       /* the GPU address window of this device, 0 takes 64 */
} bc250hsa_open_params;

/* Opens the adapter, creates the device, the paging queue, one node-0 context and one
 * unshared monitored fence, and allocates the command ring and the zero page that backs
 * the private segment buffer. params NULL takes every default.
 *
 * A note for a caller that tells a device pointer from a host pointer by its value,
 * which is what layer 2 does for hipMemcpyDefault: while the GPU address window lies
 * inside the host user address space of a 64-bit Windows process (0 to
 * 0x00007FFFFFFFFFFF), a GPU virtual address can hold the same value as a host pointer
 * of the same process. A window above that range removes the ambiguity. Until the
 * window is there, the caller must pass the direction of a copy and must not guess it. */
bc250hsa_status bc250hsa_open(const bc250hsa_open_params* params, bc250hsa_device** out);

/* Waits for the last submission under the default bound, frees every allocation this
 * device still owns, and closes the adapter. Safe with dev NULL. */
/* Consumes the handle. If flush or retirement cannot be proved, resources stay
 * quarantined until process exit rather than being unmapped under GPU work.
 * Repeated failed closes can retain multiple devices. */
void bc250hsa_close(bc250hsa_device* dev);

typedef struct bc250hsa_props {
    uint32_t struct_bytes;
    char     name[64];                  /* "AMD BC-250" */
    uint32_t gfx_ip_major;              /* 10 */
    uint32_t gfx_ip_minor;              /* 1 */
    uint32_t gfx_ip_rev;                /* 3 */
    uint32_t pci_bus, pci_device, pci_function;
    uint32_t wave_size;                 /* 32 */
    uint32_t cu_count;                  /* compute units the driver reports as enabled */
    uint32_t se_count;
    uint32_t max_workgroup_size;
    uint32_t max_workgroups_per_dim;
    uint32_t lds_bytes_per_workgroup;   /* 65536 on this part */
    uint32_t waves_per_cu;
    uint64_t vram_bytes;
    uint64_t visible_vram_bytes;
    uint64_t gtt_bytes;
    uint32_t gfx_clock_khz;             /* the clock the driver reports now */
    uint32_t mem_clock_khz;
    uint32_t mem_bus_width;
    uint32_t kmd_version[4];            /* the kernel driver version, four parts */
} bc250hsa_props;

bc250hsa_status bc250hsa_props_read(bc250hsa_device* dev, bc250hsa_props* out);

/* ---------------------------------------------------------------------------------
 * 5. Memory
 * ------------------------------------------------------------------------------- */

#define BC250HSA_MEM_DEVICE    0x0u /* device local, no host mapping asked for */
#define BC250HSA_MEM_HOST      0x1u /* host visible and cached */
#define BC250HSA_MEM_HOST_WC   0x2u /* host visible and write combined */
#define BC250HSA_MEM_EXEC      0x4u /* holds instructions: 4096-aligned base, never
                                     * write combined */
#define BC250HSA_MEM_MAPPABLE  0x8u /* device local, and a later bc250hsa_map may work */
#define BC250HSA_MEM_ZERO      0x10u/* the library zeroes the range before it returns */

typedef struct bc250hsa_mem {
    uint64_t va;        /* the GPU virtual address that the packets use */
    void*    host;      /* the host mapping, or NULL */
    uint64_t bytes;     /* the size the device sees, after alignment */
    uint32_t flags;     /* the flags this allocation was made with */
    uint32_t reserved;
    void*    opaque;    /* the library's handle; never read it */
} bc250hsa_mem;

/* Creates the allocation, maps a GPU address inside this device's window, makes it
 * resident, and waits for the paging fence. On return the memory is usable by a
 * submission. alignment 0 takes the natural one for the flags. */
bc250hsa_status bc250hsa_alloc(bc250hsa_device* dev, uint64_t bytes, uint64_t alignment,
                               uint32_t flags, bc250hsa_mem* out);

/* Frees the allocation. The caller must know that no submission still reads it. */
bc250hsa_status bc250hsa_free(bc250hsa_device* dev, bc250hsa_mem* mem);

/* A host mapping for an allocation made without one. Refuses BC250HSA_MEM_DEVICE
 * without BC250HSA_MEM_MAPPABLE, with BC250HSA_EUNSUPPORTED.
 *
 * out is the one result of this call. On BC250HSA_OK the mapping is in *out, and
 * out NULL is BC250HSA_EINVAL. This implementation also stores the pointer in
 * mem->host, but that field is an implementation detail: a caller reads *out and
 * keeps the pointer in its own copy of the handle. A caller that reads mem->host
 * after a map instead of *out works with this implementation of the header and can
 * fail with the next. bc250hsa_unmap takes the mapping back, and it clears
 * mem->host of the handle the caller gave it. */
bc250hsa_status bc250hsa_map(bc250hsa_device* dev, bc250hsa_mem* mem, void** out);
bc250hsa_status bc250hsa_unmap(bc250hsa_device* dev, bc250hsa_mem* mem);

/* Drains the write buffers of the host. Call it after a write into a write combined
 * allocation and before the submission that reads it. */
void bc250hsa_write_barrier(void);

/* Host to device and device to host copies through the host mapping. They refuse an
 * allocation with no mapping, with BC250HSA_EUNSUPPORTED: a copy engine and a copy
 * kernel are later work, named in the design. */
bc250hsa_status bc250hsa_copy_to_device(bc250hsa_device* dev, const bc250hsa_mem* dst,
                                        uint64_t dst_offset, const void* src, uint64_t bytes);
bc250hsa_status bc250hsa_copy_from_device(bc250hsa_device* dev, void* dst,
                                          const bc250hsa_mem* src, uint64_t src_offset,
                                          uint64_t bytes);

/* ---------------------------------------------------------------------------------
 * 5.1 The GPU copy and fill
 * -------------------------------------------------------------------------------
 *
 * The three calls above move bytes with the host processor through a mapping. A read of
 * device local memory that way is an uncached read stream: MEASURED on unit A,
 * evidence/m16/perf-rerun-2026-10-09, 0.21 GB/s out of an allocation that takes writes
 * at 15.6 GB/s. The three calls below move the bytes with the command processor instead,
 * as one PM4 DMA_DATA packet per 67,108,832 bytes in an indirect buffer of their own,
 * which goes to a second context of the device. That buffer holds no dispatch and so no
 * dispatch record, and the kernel driver refuses a buffer without a record on a context
 * whose dispatch journal is on (driver/kmd/hip_journal.c, HipJournalBind), which the main
 * context is from its first recorded dispatch; the operating system then puts the whole
 * device in an error state. Nothing reads or writes the memory with the host processor,
 * so the direction of the mapping stops mattering.
 *
 * All three are ordered and complete: the open batch is submitted, everything this device
 * submitted retires, the copy's buffer is submitted, and the call returns after the fence
 * of that submission, all under one hold of the device lock, so that no work of the main
 * context runs beside it (both contexts write the one fence). A caller therefore needs no
 * wait of its own, and a program may read the destination the moment the call returns.
 * Another thread that calls this library on the same device during that hold waits for
 * it; gpu_copy_lock_waits and gpu_copy_lock_wait_ticks of section 3 count those waits.
 *
 * A copy whose wait ends without retirement (receipt SUBMITTED on a device that still lives)
 * keeps that order after the call returns. Until the fence shows that it retired, every
 * submission of the device, a dispatch, a batch or another copy, first waits for it under
 * the device's wait bound (bc250hsa_wait_policy_set). If it still has not retired, the
 * submission is refused with the status of that wait (BC250HSA_ETIMEOUT, BC250HSA_EOS or
 * BC250HSA_EDEVICELOST), and nothing is submitted, no fence value is handed out and no batch
 * is opened. Once it has retired, submissions go on as before. This library holds back only
 * its own submissions: a caller that moves bytes with the host processor reads the same state
 * through bc250hsa_copy_unretired (below) and moves nothing while such a copy is unretired.
 *
 * Alignment: a copy is byte granular. A fill of a range that starts or ends between two
 * dword boundaries carries those one to three bytes in a copy packet of their own, out of
 * a staging slot this library writes, so a caller needs no alignment of its own.
 *
 * An allocation needs no host mapping for any of them: the packet needs an address.
 * That is why a device-to-device copy and a fill of device local memory, which the three
 * calls above refuse with BC250HSA_EUNSUPPORTED, work here.
 *
 * The coherence gate. A destination that the host processor can read is written by the
 * command processor only after bc250hsa_gpu_copy_ready reported 1. The library decides
 * that from the destination allocation itself and not from the name of the operation: an
 * allocation with a host mapping, or one made with BC250HSA_MEM_HOST, BC250HSA_MEM_HOST_WC,
 * BC250HSA_MEM_EXEC or BC250HSA_MEM_MAPPABLE, is such a destination. A device-to-device
 * copy into the GPU address of a host allocation is therefore gated exactly as a
 * device-to-host copy is. While the answer is 0 each of the three calls refuses such a
 * destination with BC250HSA_EUNSUPPORTED before it submits anything. */

/* The bytes one DMA_DATA packet carries, so that a caller can read bytes per packet off
 * the counters. Mesa cp_dma_max_byte_count for GFX9 and later. */
#define BC250HSA_GPU_COPY_MAX_BYTES 67108832u

/* What became of the GPU work of one call of this section. The status of a call says
 * whether the call did its work; the receipt says whether the device may still be doing
 * some of it, and a caller needs both. A wait that could not establish retirement is not a
 * completion (CONTRACT.md section 7), and the memory such work names stays in use until
 * its fence value retires.
 *
 *   BC250HSA_OP_NOT_SUBMITTED  nothing of this call is outstanding: either nothing was
 *                              submitted, or every submission of it retired before the
 *                              call stopped. A failed call with this state refused before
 *                              any outstanding work, and on a device that is not lost
 *                              (status other than BC250HSA_EDEVICELOST) the caller may do
 *                              the work another way.
 *   BC250HSA_OP_SUBMITTED      the device accepted work of this call and its retirement was
 *                              not established. fence_value is the value to wait for. The
 *                              caller must not do the work another way, must not report it
 *                              done, and must not free, reuse or read either range until
 *                              that value retires.
 *   BC250HSA_OP_RETIRED        every submission of this call retired. The status is then
 *                              BC250HSA_OK.
 *
 * A failed call on a device this library marked lost always answers BC250HSA_EDEVICELOST,
 * whatever Windows call failed first; the first failure is in the log. */
#define BC250HSA_OP_NOT_SUBMITTED 0u
#define BC250HSA_OP_SUBMITTED     1u
#define BC250HSA_OP_RETIRED       2u

typedef struct bc250hsa_op_receipt {
    uint32_t struct_bytes;   /* sizeof(bc250hsa_op_receipt) */
    uint32_t state;          /* BC250HSA_OP_* */
    uint64_t fence_value;    /* the last value this call submitted, or 0 */
} bc250hsa_op_receipt;

/* A copy between two allocations of this device, in either direction and within one
 * allocation as well, as long as the two ranges do not overlap. An overlapping copy is
 * BC250HSA_EINVAL and never a half-done packet.
 *
 * This is also the call for a device-to-host copy whose destination is one of this
 * library's own host-visible allocations: that destination has a GPU address, so the
 * copy needs no staging buffer at all. */
/* The one flag of bc250hsa_copy_device_to_device: the host processor is going to read the
 * destination of this copy. It says which counter the bytes go to, gpu_copy_d2h and not
 * gpu_copy_d2d, and it gates the call as a host-readable destination would. It is not the
 * only thing that gates it: a destination the host processor can read is gated with or
 * without the flag (the coherence gate above). */
#define BC250HSA_COPY_HOST_READS_DST 0x1u

/* receipt: a caller's bc250hsa_op_receipt with struct_bytes set, or NULL for a caller that
 * waits for everything this device owes before it touches either range again. A receipt
 * whose struct_bytes this library does not know is BC250HSA_EINVAL and stays untouched;
 * every other return of the three calls below writes it. */
bc250hsa_status bc250hsa_copy_device_to_device(bc250hsa_device* dev, const bc250hsa_mem* dst,
                                              uint64_t dst_offset, const bc250hsa_mem* src,
                                              uint64_t src_offset, uint64_t bytes,
                                              uint32_t flags, bc250hsa_op_receipt* receipt);

/* A device-to-host copy into memory that is not this library's: a program's own buffer.
 * The bytes go through a ring of host-visible staging allocations, one 1 MiB round at a
 * time: the command processor writes a staging slot, and the host processor copies that
 * slot into the caller's destination with ordinary cached reads. The ring is allocated
 * on the first call that needs it and freed with the device.
 *
 * It refuses with BC250HSA_EUNSUPPORTED when bc250hsa_gpu_copy_ready reports 0.
 *
 * A round whose wait fails keeps its slot until the round's fence value retires, and the
 * receipt names that value. The rounds before it retired and wrote part of dst; a caller
 * that sees BC250HSA_OP_NOT_SUBMITTED and does the copy another way writes all of dst
 * again.
 *
 * Concurrency: this call holds the device lock for the whole copy, every round's
 * submission and every round's wait included. A 64 MiB copy is 64 rounds, so another
 * thread's bc250hsa_flush or dispatch, and a bc250hsa_wait for a value that is not yet
 * submitted, waits behind all of them. The call above holds the lock for one packet and
 * one wait, which is the shorter hold, so a caller that needs a device-to-host copy to
 * block other threads as briefly as possible keeps its destination in one of this
 * library's own host-visible allocations and uses that call. */
bc250hsa_status bc250hsa_copy_from_device_gpu(bc250hsa_device* dev, void* dst,
                                              const bc250hsa_mem* src, uint64_t src_offset,
                                              uint64_t bytes, bc250hsa_op_receipt* receipt);

/* A fill of an allocation with one byte value, as memset defines it. The 4-byte aligned
 * interior is one DMA_DATA per chunk, and the one to three bytes at either end of a range
 * that does not begin or end on a dword boundary are copied out of a staging slot this
 * library writes, in the same list and under the same fence, so a fill is never partial
 * and no byte outside the range is touched.
 *
 * A destination the host processor can read is gated as the coherence gate above says. */
bc250hsa_status bc250hsa_fill_device(bc250hsa_device* dev, const bc250hsa_mem* dst,
                                     uint64_t dst_offset, uint8_t value, uint64_t bytes,
                                     bc250hsa_op_receipt* receipt);

/* Is a copy of this device unretired (the exclusion above)? Such a copy may still read its
 * source and write its destination. A host-processor copy or fill of the caller is no
 * submission, so this library cannot hold it back: it could read the destination before the
 * copy's bytes land, or write over the source the copy still reads, and report success with
 * wrong bytes. A caller asks this before every copy or fill it does with the host processor,
 * the ones it does after a refusal of the three calls above included, and moves no byte
 * unless the answer is BC250HSA_OK.
 *
 * BC250HSA_EDEVICELOST on a device this library marked lost, whether a copy is unretired or
 * not and whatever the fence reads: a loss is the end, and no host-processor copy goes on
 * after it. *pending is then the unretired copy's fence value, or 0 when there is none.
 * Otherwise BC250HSA_OK with *pending 0: no copy is unretired, or the fence has reached the
 * one that was, which this call then retires as the next submission would. Otherwise
 * *pending is the copy's fence value and the status is the one the last wait for it ended
 * with (BC250HSA_ETIMEOUT or BC250HSA_EOS). The call takes the device lock and reads the
 * fence. It never waits for a fence and never submits, so it costs one compare while no copy
 * is unretired; it can wait for the device lock behind a copy in progress. */
bc250hsa_status bc250hsa_copy_unretired(bc250hsa_device* dev, uint64_t* pending);

/* May the host processor read what the command processor wrote into a host-visible
 * allocation of this library, with no cache maintenance of its own?
 *
 * The page table entry of such an allocation is expected to snoop the host caches, but
 * the chain that produces it runs through the video memory manager and is not proved by
 * reading our own source. This call answers the question by measurement, once per device:
 * it writes a pattern into a device local buffer, reads the host buffer first so that its
 * lines are in the host cache, copies the one into the other with the command processor,
 * waits for the fence and compares, and then does it again with a second pattern.
 *
 * *ready is 1 on a match and 0 otherwise. A mismatch is the one coherence answer: it writes
 * one line at BC250HSA_LOG_ERROR naming the first differing offset, raises the counter
 * gpu_copy_control_failed, keeps the answer for the life of the device, and the call
 * returns BC250HSA_OK. A caller that sees 0 then keeps the host-processor path for every
 * host-readable destination, which is correct and slow.
 *
 * A control that could not ask the question is not an answer. When an allocation, a
 * mapping, a submission or a wait of the control fails, the call returns that status with
 * *ready 0, the counter stays, and the next call tries again. The control's buffers are
 * freed only once no submission of the control is outstanding; after a wait that could
 * not establish that, the device keeps them, the next call waits for their fence value
 * first under the device's wait bound, and bc250hsa_close frees or quarantines them with
 * every other allocation. A device-to-device copy into memory the host processor cannot
 * read never asks this question. */
bc250hsa_status bc250hsa_gpu_copy_ready(bc250hsa_device* dev, uint32_t* ready);

/* An allocator seam, so that the loader runs with no device. The device path passes
 * its own implementation. A host test passes one over malloc with a synthetic base
 * address. */
typedef struct bc250hsa_allocator {
    void* ctx;
    bc250hsa_status (*alloc)(void* ctx, uint64_t bytes, uint64_t alignment, uint32_t flags,
                             bc250hsa_mem* out);
    void            (*free)(void* ctx, bc250hsa_mem* mem);
} bc250hsa_allocator;

/* The device's own allocator, for a caller that holds the loader and the device apart. */
bc250hsa_status bc250hsa_device_allocator(bc250hsa_device* dev, bc250hsa_allocator* out);

/* ---------------------------------------------------------------------------------
 * 6. Code objects
 * ------------------------------------------------------------------------------- */

/* The kinds of the metadata .args list. The packer knows every field by its kind and
 * never by a fixed offset: in a measured kernel hidden_block_count_z and
 * hidden_group_size_x share one offset. */
typedef enum bc250hsa_arg_kind {
    BC250HSA_ARG_BY_VALUE = 0,
    BC250HSA_ARG_GLOBAL_BUFFER,
    BC250HSA_ARG_DYNAMIC_SHARED_POINTER,
    BC250HSA_ARG_SAMPLER,
    BC250HSA_ARG_IMAGE,
    BC250HSA_ARG_PIPE,
    BC250HSA_ARG_QUEUE,
    BC250HSA_ARG_HIDDEN_BLOCK_COUNT_X,
    BC250HSA_ARG_HIDDEN_BLOCK_COUNT_Y,
    BC250HSA_ARG_HIDDEN_BLOCK_COUNT_Z,
    BC250HSA_ARG_HIDDEN_GROUP_SIZE_X,
    BC250HSA_ARG_HIDDEN_GROUP_SIZE_Y,
    BC250HSA_ARG_HIDDEN_GROUP_SIZE_Z,
    BC250HSA_ARG_HIDDEN_REMAINDER_X,
    BC250HSA_ARG_HIDDEN_REMAINDER_Y,
    BC250HSA_ARG_HIDDEN_REMAINDER_Z,
    BC250HSA_ARG_HIDDEN_GLOBAL_OFFSET_X,
    BC250HSA_ARG_HIDDEN_GLOBAL_OFFSET_Y,
    BC250HSA_ARG_HIDDEN_GLOBAL_OFFSET_Z,
    BC250HSA_ARG_HIDDEN_GRID_DIMS,
    BC250HSA_ARG_HIDDEN_DYNAMIC_LDS_SIZE,
    BC250HSA_ARG_HIDDEN_PRINTF_BUFFER,
    BC250HSA_ARG_HIDDEN_HOSTCALL_BUFFER,
    BC250HSA_ARG_HIDDEN_HEAP_V1,
    BC250HSA_ARG_HIDDEN_DEFAULT_QUEUE,
    BC250HSA_ARG_HIDDEN_COMPLETION_ACTION,
    BC250HSA_ARG_HIDDEN_MULTIGRID_SYNC_ARG,
    BC250HSA_ARG_HIDDEN_QUEUE_PTR,
    BC250HSA_ARG_HIDDEN_PRIVATE_BASE,
    BC250HSA_ARG_HIDDEN_SHARED_BASE,
    BC250HSA_ARG_HIDDEN_OTHER   /* a key this build does not know: zero fill and count */
} bc250hsa_arg_kind;

#define BC250HSA_AS_NONE     0u
#define BC250HSA_AS_GLOBAL   1u
#define BC250HSA_AS_PRIVATE  2u
#define BC250HSA_AS_LOCAL    3u
#define BC250HSA_AS_CONSTANT 4u
#define BC250HSA_AS_GENERIC  5u

typedef struct bc250hsa_arg {
    uint32_t offset;        /* .offset, bytes from the start of the kernarg buffer */
    uint32_t size;          /* .size */
    uint16_t kind;          /* bc250hsa_arg_kind */
    uint8_t  address_space; /* BC250HSA_AS_* */
    uint8_t  value_align;   /* .value_align, or 0 */
} bc250hsa_arg;

/* Everything a dispatch and a packer need for one kernel. The library owns this
 * structure; it lives as long as its module. */
typedef struct bc250hsa_kernel {
    const char* name;                   /* the metadata .name, the device side name */
    uint64_t descriptor_va;             /* the loaded .symbol object, 64-byte aligned */
    uint64_t entry_va;                  /* descriptor_va + KERNEL_CODE_ENTRY_BYTE_OFFSET */
    uint32_t kernarg_bytes;             /* .kernarg_segment_size */
    uint32_t kernarg_align;             /* max(.kernarg_segment_align, 16) */
    uint32_t group_segment_bytes;       /* .group_segment_fixed_size */
    uint32_t private_segment_bytes;     /* .private_segment_fixed_size */
    uint32_t max_flat_workgroup_size;
    uint16_t sgpr_count;
    uint16_t vgpr_count;
    uint8_t  wave_size;                 /* 32 on this part */
    uint8_t  workgroup_processor_mode;
    uint8_t  uses_dynamic_stack;
    uint8_t  user_sgpr_count;           /* read out of COMPUTE_PGM_RSRC2 bits 5:1 */
    uint32_t compute_pgm_rsrc1;         /* copied out of the kernel descriptor */
    uint32_t compute_pgm_rsrc2;         /* copied; the dispatch writes LDS_SIZE into it */
    uint32_t compute_pgm_rsrc3;         /* copied */
    uint16_t kernel_code_properties;    /* the ENABLE_SGPR_* bits */
    uint16_t reserved;
    uint32_t arg_count;                 /* every entry, explicit and hidden */
    uint32_t explicit_arg_count;        /* the entries that hipLaunchKernel supplies */
    uint32_t hidden_arg_count;
    const bc250hsa_arg* args;           /* arg_count entries, in metadata order */
} bc250hsa_kernel;

/* The offload bundle reader. It copies nothing: image points into fatbin.
 * target_id NULL takes "gfx1013", and an exact processor wins over a generic one.
 * Refuses a compressed bundle by name, with BC250HSA_ECOMPRESSEDBUNDLE. */
bc250hsa_status bc250hsa_unbundle(const void* fatbin, size_t fatbin_bytes,
                                  const char* target_id,
                                  const void** image, size_t* image_bytes);

/* Loads one ELF code object onto the device: checks the header, allocates one
 * executable range that covers every PT_LOAD segment, copies p_filesz and zeroes the
 * rest of p_memsz, applies the R_AMDGPU_RELATIVE64 relocations, reads the
 * NT_AMDGPU_METADATA note and resolves every kernel descriptor from .dynsym. It does
 * not keep the image pointer, and it does not unbundle. */
bc250hsa_status bc250hsa_module_load(bc250hsa_device* dev, const void* image,
                                     size_t image_bytes, bc250hsa_module** out);

/* The same loader against a caller's allocator, so that a host test runs it with no
 * device and no GPU. */
bc250hsa_status bc250hsa_module_load_alloc(const bc250hsa_allocator* alloc,
                                           const void* image, size_t image_bytes,
                                           bc250hsa_module** out);

void bc250hsa_module_unload(bc250hsa_module* mod);

uint32_t                bc250hsa_module_kernel_count(const bc250hsa_module* mod);
const bc250hsa_kernel*  bc250hsa_module_kernel_at(const bc250hsa_module* mod, uint32_t index);
const bc250hsa_kernel*  bc250hsa_module_kernel_by_name(const bc250hsa_module* mod,
                                                       const char* name);

/* A device global of the loaded object, for __hipRegisterVar. */
bc250hsa_status bc250hsa_module_symbol(const bc250hsa_module* mod, const char* name,
                                       uint64_t* va, uint64_t* bytes);

/* The loaded range of the object, for a fault report. */
bc250hsa_status bc250hsa_module_range(const bc250hsa_module* mod, uint64_t* va,
                                      uint64_t* bytes);

/* ---------------------------------------------------------------------------------
 * 7. Kernel arguments
 * ------------------------------------------------------------------------------- */

typedef struct bc250hsa_launch {
    uint32_t struct_bytes;
    uint32_t grid[3];               /* workgroups, not work items */
    uint32_t block[3];              /* work items per workgroup */
    uint32_t dynamic_group_bytes;   /* added to group_segment_bytes */
} bc250hsa_launch;

typedef struct bc250hsa_pack_result {
    uint32_t struct_bytes;
    uint32_t bytes_written;
    uint32_t explicit_args_written;
    uint32_t hidden_args_zeroed;
    uint32_t unknown_arg_kinds;
    uint32_t hostcall_buffer_requested; /* 1: the kernel asks for a host call service */
    uint32_t dispatch_packet_requested; /* 1: the kernel enables ENABLE_SGPR_DISPATCH_PTR,
                                         * and the packet was written at the offset below */
    uint32_t dispatch_packet_offset;    /* where in the buffer the 64-byte packet is */
    char     first_unknown_kind[32];    /* the metadata key, for the log */
} bc250hsa_pack_result;

/* ---------------------------------------------------------------------------------
 * 7.1 The AQL dispatch packet
 *
 * A kernel may enable ENABLE_SGPR_DISPATCH_PTR, and the compiler does so for every
 * kernel that reads its own workgroup size: `__builtin_amdgcn_workgroup_size_x`, which
 * is what HIP's blockDim becomes, is a 16-bit load from the AQL kernel dispatch packet
 * and not an implicit kernel argument. MEASURED on the ggml-hip backend of llama.cpp:
 * 1752 of its 7105 gfx1013 kernels enable the bit, among them every k_get_rows kernel,
 * which is the first launch of a decode (evidence/m16/step3b-2026-10-09, defect BD-110).
 *
 * A PM4 dispatch has no packet of its own, so this layer writes one. It belongs to the
 * launch, so it goes at the end of the kernel argument buffer the caller already
 * allocates per launch: bc250hsa_kernarg_requirements adds the 64 bytes and the
 * alignment, bc250hsa_kernarg_pack fills it from the same bc250hsa_launch the kernel
 * arguments come from, and the caller passes its address in bc250hsa_dispatch. One
 * allocation, one lifetime, and the packet retires with the dispatch that reads it.
 *
 * Every field the packet holds is one this layer knows: the workgroup size, the grid in
 * work items, the two segment sizes, the kernel object and the kernel argument address.
 * completion_signal is 0, because this layer completes through its own fence and not
 * through an HSA signal, and the two reserved fields are 0.
 * ------------------------------------------------------------------------------- */

#define BC250HSA_AQL_PACKET_BYTES 64u
#define BC250HSA_AQL_PACKET_ALIGN 64u

/* The size and the alignment that the caller must allocate for one launch. It covers
 * the kernel arguments and, for a kernel that enables ENABLE_SGPR_DISPATCH_PTR, the
 * AQL dispatch packet behind them. A kernel whose arguments and that packet do not fit
 * in the uint32_t of this interface is refused with BC250HSA_EINVAL and bytes 0: a size
 * that wrapped would defeat the capacity check of bc250hsa_kernarg_pack. */
bc250hsa_status bc250hsa_kernarg_requirements(const bc250hsa_kernel* kernel,
                                              uint32_t* bytes, uint32_t* alignment);

/* Fills the kernel argument buffer from the metadata .args list and from nothing else.
 * args is the array that hipLaunchKernel receives: one pointer per explicit argument,
 * in declaration order. The packer walks the list and the array in step, writes every
 * hidden field it knows from launch, and zeroes every hidden field it does not know.
 * It never computes an offset of its own. kernarg may be a plain host buffer, which is
 * what the host test uses.
 *
 * kernarg_va is the GPU address that this buffer will have in bc250hsa_dispatch. It is
 * written into the AQL dispatch packet of section 7.1 and used for nothing else, so a
 * caller that packs into a plain host buffer passes 0 and the packet's kernarg_address
 * field reads 0. */
bc250hsa_status bc250hsa_kernarg_pack(const bc250hsa_kernel* kernel,
                                      const bc250hsa_launch* launch,
                                      void* const* args, uint32_t arg_count,
                                      void* kernarg, uint32_t kernarg_bytes,
                                      uint64_t kernarg_va,
                                      bc250hsa_pack_result* result);

/* ---------------------------------------------------------------------------------
 * 8. Dispatch
 * ------------------------------------------------------------------------------- */

#define BC250HSA_DISPATCH_GFX_RING   0x1u /* node 0 is the graphics ring: emit
                                           * CONTEXT_CONTROL first. The device path sets
                                           * it by itself; the golden test sets it by
                                           * hand */
#define BC250HSA_DISPATCH_NO_CU_MASK 0x2u /* drop the two SET_SH_REG_INDEX packets */
#define BC250HSA_DISPATCH_START_AT_000 0x4u /* add FORCE_START_AT_000 to the initiator */
#define BC250HSA_DISPATCH_NO_ACQUIRE 0x8u /* leave out ACQUIRE_MEM; diagnosis only */
#define BC250HSA_DISPATCH_NO_FENCE   0x10u/* leave out RELEASE_MEM; the caller fences */
#define BC250HSA_DISPATCH_LIGHT_BARRIER 0x20u /* between two dispatches of one batch, emit
                                               * the level-0 and level-1 invalidate only.
                                               * Section 8.1 */
#define BC250HSA_DISPATCH_FULL_STATE 0x40u /* write the whole compute state for every
                                            * dispatch of a batch, not only the registers
                                            * that changed since the one before it.
                                            * Section 8.8; the switch a lab arm compares
                                            * against */

typedef struct bc250hsa_dispatch {
    uint32_t struct_bytes;
    uint32_t flags;
    const bc250hsa_kernel* kernel;
    uint64_t kernarg_va;            /* a resident buffer of kernarg_bytes, already filled */
    /* The AQL dispatch packet of section 7.1, where bc250hsa_kernarg_pack wrote it:
     * kernarg_va + bc250hsa_pack_result.dispatch_packet_offset. It is needed only by a
     * kernel that enables ENABLE_SGPR_DISPATCH_PTR, and such a kernel with 0 here is
     * refused rather than started with a register that points nowhere. */
    uint64_t dispatch_packet_va;
    bc250hsa_launch launch;
} bc250hsa_dispatch;

/* Builds the indirect buffer into a free slot of this device's command ring, submits it
 * on the node-0 context and returns the fence value that this dispatch writes. It does
 * not wait for the dispatch. It may wait for a free ring slot, under the bound of
 * bc250hsa_wait's defaults, and returns BC250HSA_EBUSY if none frees. It may also wait for
 * a GPU copy of section 5.1 whose own wait failed, and refuses as that section says when
 * the copy still has not retired.
 *
 * With batching on (section 8.1) the dispatch is appended to the indirect buffer this
 * device is building and the submission happens at a flush point. The returned fence
 * value is then the value of the whole batch: it is a value this device has promised
 * and not yet submitted, every wait for it submits the batch first, and a caller
 * cannot tell the difference apart from the timing.
 *
 * It refuses, and submits nothing: a zero grid or block, a block product above
 * max_flat_workgroup_size, local memory above the device limit, uses_dynamic_stack,
 * a kernarg_va that is not aligned to kernarg_align, a kernel that enables
 * ENABLE_SGPR_DISPATCH_PTR with no dispatch_packet_va, and a kernel whose enabled user
 * SGPRs this build does not program (BC250HSA_EUNSUPPORTED in the last three cases). */
bc250hsa_status bc250hsa_dispatch_submit(bc250hsa_device* dev,
                                         const bc250hsa_dispatch* dispatch,
                                         uint64_t* fence_value_out);

/* HIP runtime submissions carry an immutable BC250_HIP_DISPATCH_RECORD from
 * driver/shim/include/bc250_hip_journal.h. The backend copies it before it
 * returns and uploads the batch to the KMD journal before SubmitCommand.
 * An upload refusal prevents that batch from reaching the GPU. The record
 * is required even when diagnostic logging is disabled. The legacy entry
 * above remains available to the standalone layer-1 measurement tools. */
bc250hsa_status bc250hsa_dispatch_submit_recorded(bc250hsa_device* dev,
                                                  const bc250hsa_dispatch* dispatch,
                                                  const void* record,
                                                  uint32_t record_bytes,
                                                  uint64_t* fence_value_out);

/* ---------------------------------------------------------------------------------
 * 8.1 Batching
 *
 * One dispatch per indirect buffer is one D3DKMTSubmitCommand, and therefore one
 * user-to-kernel transition and one kernel driver submission, per kernel. A decode step
 * of a language model issues hundreds of small kernels, so that cost is paid hundreds of
 * times per token. The kernel driver runs one indirect buffer at a time
 * (driver/kmd/wddm.c refuses a blob whose num_ibs is not 1), so the answer is not more
 * indirect buffers per submission: it is more dispatches per indirect buffer.
 *
 * With batching on, consecutive dispatches of this device append into one indirect
 * buffer, separated by the barrier of BC250HSA_DISPATCH_LIGHT_BARRIER or by the full
 * acquire, and one RELEASE_MEM at the end writes one fence value for the whole buffer.
 * Every dispatch of the batch therefore reports that one value, which keeps the rule a
 * caller needs: when the fence reaches the value a dispatch reported, that dispatch has
 * finished.
 *
 * The batch is submitted at the first of these:
 *   - bc250hsa_flush, bc250hsa_close, or a bc250hsa_wait for a value the device has not
 *     submitted yet;
 *   - bc250hsa_free, bc250hsa_map, bc250hsa_unmap, a copy through the host mapping, or a
 *     module load or unload, because each of them changes memory a dispatch of the open
 *     batch may name;
 *   - max_dispatches dispatches, or max_ib_dwords dwords, in the buffer;
 *   - a dispatch that arrives max_hold_us or more after the batch was opened.
 * The library runs no thread of its own, so the time cap is read when a call arrives.
 * Nothing a program can observe stays behind an open batch: every read of the device
 * goes through one of the flush points above.
 *
 * A submission that fails with an open batch loses work this library already promised.
 * It therefore marks the device lost, where the next call reports it, instead of
 * returning an error to a caller that did not make that dispatch.
 * ------------------------------------------------------------------------------- */

#define BC250HSA_BATCH_DISPATCHES_MAX     256u
#define BC250HSA_BATCH_DISPATCHES_DEFAULT  32u
#define BC250HSA_BATCH_HOLD_US_DEFAULT   1000u

typedef struct bc250hsa_batch_policy {
    uint32_t struct_bytes;
    uint32_t enabled;         /* 0: one submission per dispatch, as build 1 did */
    uint32_t max_dispatches;  /* 0 takes BC250HSA_BATCH_DISPATCHES_DEFAULT */
    uint32_t max_ib_dwords;   /* 0 takes what one command ring slot holds */
    uint32_t max_hold_us;     /* 0 takes BC250HSA_BATCH_HOLD_US_DEFAULT */
    uint32_t light_barrier;   /* 1: BC250HSA_DISPATCH_LIGHT_BARRIER between dispatches */
} bc250hsa_batch_policy;

/* Rule 6 of this header keeps policy out of the library: the caller decides. A change
 * submits an open batch first, so the policy of a dispatch is the policy its buffer was
 * opened with. */
bc250hsa_status bc250hsa_batch_policy_set(bc250hsa_device* dev,
                                          const bc250hsa_batch_policy* policy);
bc250hsa_status bc250hsa_batch_policy_get(bc250hsa_device* dev, bc250hsa_batch_policy* out);

/* Submits the open batch, if there is one. fence_value_out, when it is not NULL,
 * receives the value of that submission, or the last submitted value when nothing was
 * open. It is BC250HSA_OK with batching off and with an empty batch. */
bc250hsa_status bc250hsa_flush(bc250hsa_device* dev, uint64_t* fence_value_out);

/* --- the pure builder, for the golden test and for a failure report --------------- */

#define BC250HSA_PM4_MAX_DWORDS 128u

typedef struct bc250hsa_pm4_env {
    uint32_t struct_bytes;
    uint32_t flags;                     /* the same BC250HSA_DISPATCH_* bits */
    uint64_t fence_va;                  /* 8-byte aligned, the monitored fence */
    uint64_t fence_value;
    uint32_t private_segment_rsrc[4];   /* the 128-bit buffer resource for s[0:3] */
    uint32_t ib_pad_dwords;             /* 8 on the graphics ring, 0 takes 8 */
    /* Optional extension: old 48-byte environments remain valid for zero-private
     * kernels. Fixed-private dispatches require the complete structure. The buffer
     * is device-local, 4096-aligned, resident, and owned until this IB retires. */
    uint64_t scratch_va;
    uint64_t scratch_bytes;
} bc250hsa_pm4_env;

/* Writes the dispatch of section 8 of the design as PM4 dwords into the caller's
 * buffer. The 48-byte ABI 1.2 environment remains accepted for zero-private kernels.
 * Fixed-private kernels require scratch_va/scratch_bytes in the full environment.
 * It touches no device and no operating system, so a host test compares the
 * result with a golden stream. */
bc250hsa_status bc250hsa_pm4_build_dispatch(const bc250hsa_dispatch* dispatch,
                                            const bc250hsa_pm4_env* env,
                                            uint32_t* dwords, uint32_t dword_capacity,
                                            uint32_t* dwords_written);

/* The same stream for count dispatches in one indirect buffer: one head (the graphics
 * ring CONTEXT_CONTROL and the full acquire), count dispatch bodies with the barrier of
 * env->flags between them, and one completion write at the end. count 1 writes exactly
 * what bc250hsa_pm4_build_dispatch writes, dword for dword. A batch of n dispatches
 * needs at most n * BC250HSA_PM4_MAX_DWORDS dwords. Fixed-private kernels are
 * accepted only with count 1, so one live BO never changes its ring stride. */
bc250hsa_status bc250hsa_pm4_build_batch(const bc250hsa_dispatch* dispatches, uint32_t count,
                                         const bc250hsa_pm4_env* env,
                                         uint32_t* dwords, uint32_t dword_capacity,
                                         uint32_t* dwords_written);

/* COMPUTE_PGM_RSRC2.LDS_SIZE for this part: align(bytes, 512) / 512. The command
 * processor writes this field from the AQL packet, and a PM4 path must write it by
 * hand, because the kernel descriptor holds 0. */
uint32_t bc250hsa_lds_size_field(uint32_t group_segment_bytes, uint32_t dynamic_group_bytes);

/* The 128-bit buffer resource of a raw buffer on gfx10.1, for the private segment
 * buffer in s[0:3]. */
bc250hsa_status bc250hsa_buffer_resource(uint64_t va, uint64_t bytes, uint32_t out_dwords[4]);

#define BC250HSA_MAX_USER_SGPR 16u

typedef struct bc250hsa_user_sgpr_plan {
    uint32_t struct_bytes;
    uint32_t count;                             /* registers in use, at most 16 */
    uint32_t value[BC250HSA_MAX_USER_SGPR];     /* COMPUTE_USER_DATA_0 upward */
} bc250hsa_user_sgpr_plan;

/* Walks the ENABLE_SGPR_* bits of the kernel in the order of the AMDGPU documentation
 * and places each item at the next free COMPUTE_USER_DATA register. It refuses an
 * enabled item that this build does not program, with BC250HSA_EUNSUPPORTED, and it
 * refuses ENABLE_SGPR_DISPATCH_PTR with dispatch_packet_va 0 for the same reason: a
 * register that points nowhere is worse than a stated refusal. */
bc250hsa_status bc250hsa_plan_user_sgprs(const bc250hsa_kernel* kernel, uint64_t kernarg_va,
                                         uint64_t dispatch_packet_va,
                                         const uint32_t private_segment_rsrc[4],
                                         bc250hsa_user_sgpr_plan* out);

/* ---------------------------------------------------------------------------------
 * 9. Completion
 * ------------------------------------------------------------------------------- */

/* The last value that the GPU wrote, read straight from the fence's host mapping. It
 * never enters the kernel and it never fails. UINT64_MAX means a lost device. */
uint64_t bc250hsa_fence_read(bc250hsa_device* dev);

/* The value the last dispatch of this device writes. 0 before the first one. With
 * batching on it covers a dispatch that is still in the open batch, because that is what
 * a caller means by "everything this device owes"; bc250hsa_wait for it submits the
 * batch. */
uint64_t bc250hsa_fence_last_submitted(bc250hsa_device* dev);

/* Waits until the fence reaches value. A value this device has promised and not yet
 * submitted submits the open batch first (section 8.1). It reads the host mapping
 * first, and most waits end there. It then waits in slices of slice_ms, up to total_ms in all, and between
 * two slices it re-reads the fence and asks the operating system whether the device
 * still runs. slice_ms 0 takes 1000 and total_ms 0 takes 120000, which are the
 * measured defaults of our Vulkan driver: one long wait turns a healthy wait behind
 * another process's reset into a lost device. A test or a short trial passes a smaller
 * total by hand.
 *
 * Returns BC250HSA_OK, BC250HSA_ETIMEOUT (the device still lives) or
 * BC250HSA_EDEVICELOST. */
bc250hsa_status bc250hsa_wait(bc250hsa_device* dev, uint64_t value,
                              uint32_t slice_ms, uint32_t total_ms);

/* The same two numbers for the waits this library performs for itself. A GPU copy of
 * section 5.1 returns after its own fence, so that wait is inside the library and the
 * caller never sees its value. This call tells the library which bound to use, and 0 and
 * 0 restore the defaults above. Nothing else of this interface waits on its own, so a
 * caller that never uses section 5.1 never needs it. */
bc250hsa_status bc250hsa_wait_policy_set(bc250hsa_device* dev, uint32_t slice_ms,
                                         uint32_t total_ms);

/* ---------------------------------------------------------------------------------
 * 10. Diagnosis
 * ------------------------------------------------------------------------------- */

typedef struct bc250hsa_fault {
    uint32_t struct_bytes;
    uint32_t device_lost;       /* 1: this device no longer runs work */
    uint64_t faulted_va;
    uint32_t general_error;
    uint32_t device_error;
    uint32_t fault_flags;
    uint32_t pipeline_stage;
    uint64_t fence_value;       /* the value at the time of the query */
    uint64_t fence_expected;    /* the last submitted value */
} bc250hsa_fault;

/* Asks the operating system for the execution state, and for the page fault block when
 * the state reports a fault. It changes nothing and it is safe at any time. */
bc250hsa_status bc250hsa_query_fault(bc250hsa_device* dev, bc250hsa_fault* out);

/* The dwords of the last indirect buffer that this device submitted, for a failure
 * report. The pointer stays valid until the next submission. */
bc250hsa_status bc250hsa_last_ib(bc250hsa_device* dev, const uint32_t** dwords,
                                 uint32_t* count);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* BC250HSA_H */
