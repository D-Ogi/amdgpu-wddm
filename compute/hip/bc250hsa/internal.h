/* internal.h - what the translation units of bc250hsa share.
 *
 * Layer 1 of docs/design/m16-hip-route-b.md. Nothing here is public: the public
 * interface is compute/hip/include/bc250hsa.h and it does not change.
 *
 * Three groups:
 *   1. the log hook and the process counters (status.c),
 *   2. the module structure that the loader fills and the dispatch reads
 *      (co_loader.c, co_metadata.c, kernarg.c, pm4_dispatch.c),
 *   3. the device structure that the Windows half owns (kmt_device.c,
 *      kmt_memory.c, submit.c).
 *
 * A host test links group 1 and group 2 and never group 3. That is why the
 * device structure is an incomplete type here and is declared in kmt_device.h.
 */
#ifndef BC250HSA_INTERNAL_H
#define BC250HSA_INTERNAL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bc250hsa.h"

/* --------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------ */

#define BC250HSA_ARRAY_COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

static __inline uint64_t bc250hsa_align_up_u64(uint64_t value, uint64_t alignment)
{
    if (alignment <= 1u) {
        return value;
    }
    return (value + alignment - 1u) / alignment * alignment;
}

static __inline uint32_t bc250hsa_align_up_u32(uint32_t value, uint32_t alignment)
{
    if (alignment <= 1u) {
        return value;
    }
    return (value + alignment - 1u) / alignment * alignment;
}

static __inline int bc250hsa_is_power_of_two_u64(uint64_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

/* Every public call that takes a caller-filled structure starts with this. Rule 4
 * of the header: an unknown size is BC250HSA_EINVAL, never a guess. */
static __inline int bc250hsa_struct_bytes_ok(uint32_t given, size_t known)
{
    return given == (uint32_t)known;
}

/* --------------------------------------------------------------------------
 * Log and counters (status.c)
 * ------------------------------------------------------------------------ */

void bc250hsa_log(uint32_t level, const char* format, ...);
void bc250hsa_log_enabled_set(void);   /* test hook: no-op in the product build */

/* The counter fields, named so that a caller cannot pass the wrong one. */
typedef enum bc250hsa_counter {
    BC250HSA_C_MODULES_LOADED = 0,
    BC250HSA_C_DISPATCHES_BUILT,
    BC250HSA_C_SUBMISSIONS,
    BC250HSA_C_SUBMISSIONS_REFUSED,
    BC250HSA_C_WAITS,
    BC250HSA_C_WAITS_FAST,
    BC250HSA_C_WAITS_TIMED_OUT,
    BC250HSA_C_DEVICE_LOSSES,
    BC250HSA_C_HIDDEN_ARGS_ZEROED,
    BC250HSA_C_UNKNOWN_ARG_KINDS,
    BC250HSA_C_HOSTCALL_BUFFER_REQUESTS,
    BC250HSA_C_DYNAMIC_STACK_REFUSALS,
    BC250HSA_C_BATCHES_SUBMITTED,
    BC250HSA_C_DISPATCHES_BATCHED,
    /* The GPU copy route of scratch/hip-gpu-copy/CONTRACT.md section 9. Every copy and
     * every fill is counted on exactly one of the three lines (GPU, staged GPU, CPU
     * fallback), so the three sums are the whole traffic and a fallback cannot hide. */
    BC250HSA_C_GPU_COPY_D2H,
    BC250HSA_C_GPU_COPY_D2H_BYTES,
    BC250HSA_C_GPU_COPY_D2H_STAGED,
    BC250HSA_C_GPU_COPY_D2H_STAGED_BYTES,
    BC250HSA_C_GPU_COPY_D2D,
    BC250HSA_C_GPU_COPY_D2D_BYTES,
    BC250HSA_C_GPU_FILL,
    BC250HSA_C_GPU_FILL_BYTES,
    BC250HSA_C_CPU_COPY_FALLBACK,
    BC250HSA_C_CPU_COPY_FALLBACK_BYTES,
    BC250HSA_C_GPU_COPY_PACKETS,
    BC250HSA_C_GPU_COPY_FORCED_FLUSHES,
    BC250HSA_C_GPU_COPY_CONTROL_FAILED,
    /* Durations, in ticks of bc250hsa_ticks (bc250hsa.h section 3). A total is added with
     * bc250hsa_count_add; a _MAX field only ever rises, through bc250hsa_count_max. */
    BC250HSA_C_WAITS_FAST_TICKS,
    BC250HSA_C_WAITS_FAST_TICKS_MAX,
    BC250HSA_C_WAITS_OTHER_TICKS,
    BC250HSA_C_WAITS_OTHER_TICKS_MAX,
    BC250HSA_C_SUBMIT_CALLS,
    BC250HSA_C_SUBMIT_TICKS,
    BC250HSA_C_GPU_COPY_D2H_TICKS,
    BC250HSA_C_GPU_COPY_D2H_STAGED_TICKS,
    BC250HSA_C_GPU_COPY_D2D_TICKS,
    BC250HSA_C_GPU_FILL_TICKS,
    BC250HSA_C_CPU_COPY_FALLBACK_TICKS,
    /* The device lock and the GPU copy (CONTRACT.md deviation 13.23), and the journal guard
     * of submit_ib. */
    BC250HSA_C_GPU_COPY_LOCK_HOLDS,
    BC250HSA_C_GPU_COPY_LOCK_TICKS,
    BC250HSA_C_GPU_COPY_LOCK_TICKS_MAX,
    BC250HSA_C_GPU_COPY_LOCK_WAITS,
    BC250HSA_C_GPU_COPY_LOCK_WAIT_TICKS,
    BC250HSA_C_GPU_COPY_LOCK_WAIT_TICKS_MAX,
    BC250HSA_C_JOURNAL_GUARD_REFUSALS,
    BC250HSA_C_GPU_COPY_UNRETIRED,
    BC250HSA_C_GPU_COPY_EXCLUSION_REFUSALS,
    BC250HSA_C_GPU_COPY_LATE_RETIREMENTS,
    BC250HSA_C_CPU_COPY_EXCLUSION_REFUSALS,
    BC250HSA_C_COUNT
} bc250hsa_counter;

void bc250hsa_count_add(bc250hsa_counter which, uint64_t delta);
/* Raises the counter to value when value is larger, with a compare-and-swap loop and no
 * lock, so that two threads that finish a wait at once both get their say. */
void bc250hsa_count_max(bc250hsa_counter which, uint64_t value);

/* The last failed Windows call of this thread, for bc250hsa_last_os_status(). */
void bc250hsa_set_os_status(int32_t status);

/* --------------------------------------------------------------------------
 * The loaded module (co_loader.c, co_metadata.c)
 * ------------------------------------------------------------------------ */

/* The 64-byte kernel descriptor, as section 3.5 of the design measured it. The
 * loader keeps a copy per kernel, so a failure report can print it without
 * reading device memory again. */
typedef struct bc250hsa_kernel_descriptor {
    uint32_t group_segment_fixed_size;
    uint32_t private_segment_fixed_size;
    uint32_t kernarg_size;
    uint32_t reserved0;
    uint64_t kernel_code_entry_byte_offset;
    uint8_t  reserved1[20];
    uint32_t compute_pgm_rsrc3;
    uint32_t compute_pgm_rsrc1;
    uint32_t compute_pgm_rsrc2;
    uint16_t kernel_code_properties;
    uint16_t kernarg_preload;
    uint32_t reserved2;
} bc250hsa_kernel_descriptor;

/* KERNEL_CODE_PROPERTIES bits, AMDGPUUsage.rst "Kernel Descriptor". The order of
 * the first seven is also the order the user SGPRs are placed in. */
#define BC250HSA_KCP_PRIVATE_SEGMENT_BUFFER 0x0001u
#define BC250HSA_KCP_DISPATCH_PTR           0x0002u
#define BC250HSA_KCP_QUEUE_PTR              0x0004u
#define BC250HSA_KCP_KERNARG_SEGMENT_PTR    0x0008u
#define BC250HSA_KCP_DISPATCH_ID            0x0010u
#define BC250HSA_KCP_FLAT_SCRATCH_INIT      0x0020u
#define BC250HSA_KCP_PRIVATE_SEGMENT_SIZE   0x0040u
#define BC250HSA_KCP_WAVEFRONT_SIZE32       0x0400u
#define BC250HSA_KCP_USES_DYNAMIC_STACK     0x0800u
/* Everything this build refuses to see set. */
#define BC250HSA_KCP_KNOWN_MASK                                                  \
    (BC250HSA_KCP_PRIVATE_SEGMENT_BUFFER | BC250HSA_KCP_DISPATCH_PTR |           \
     BC250HSA_KCP_QUEUE_PTR | BC250HSA_KCP_KERNARG_SEGMENT_PTR |                 \
     BC250HSA_KCP_DISPATCH_ID | BC250HSA_KCP_FLAT_SCRATCH_INIT |                 \
     BC250HSA_KCP_PRIVATE_SEGMENT_SIZE | BC250HSA_KCP_WAVEFRONT_SIZE32 |         \
     BC250HSA_KCP_USES_DYNAMIC_STACK)
/* The items this build actually programs into COMPUTE_USER_DATA: the private
 * segment buffer and the kernel argument pointer. Everything else enabled is
 * BC250HSA_EUNSUPPORTED (section 3.5 of the design). */
#define BC250HSA_KCP_PROGRAMMED_MASK                                             \
    (BC250HSA_KCP_PRIVATE_SEGMENT_BUFFER | BC250HSA_KCP_KERNARG_SEGMENT_PTR)

/* COMPUTE_PGM_RSRC2.USER_SGPR, bits 5:1. */
#define BC250HSA_RSRC2_USER_SGPR_SHIFT 1u
#define BC250HSA_RSRC2_USER_SGPR_MASK  0x1Fu
/* COMPUTE_PGM_RSRC2.LDS_SIZE, bits 23:15, in units of 512 bytes on gfx10.1. */
#define BC250HSA_RSRC2_LDS_SIZE_SHIFT  15u
#define BC250HSA_RSRC2_LDS_SIZE_MASK   0x1FFu
#define BC250HSA_LDS_GRANULE_BYTES     512u

typedef struct bc250hsa_kernel_internal {
    bc250hsa_kernel            pub;      /* what the caller sees */
    bc250hsa_kernel_descriptor descriptor;
    char*                      name;     /* owned; pub.name points at it */
    bc250hsa_arg*              args;     /* owned; pub.args points at it */
    char*                      symbol;      /* owned metadata descriptor symbol */
} bc250hsa_kernel_internal;

struct bc250hsa_module {
    bc250hsa_allocator        alloc;     /* the allocator this module was loaded with */
    bc250hsa_mem              image;     /* the one executable range */
    uint64_t                  lowest_vaddr;  /* the ELF p_vaddr the range starts at */
    uint32_t                  kernel_count;
    bc250hsa_kernel_internal* kernels;
    /* .dynsym and .dynstr, copied so that bc250hsa_module_symbol() works after
     * the caller's image buffer is gone. */
    uint8_t*                  dynsym;
    size_t                    dynsym_bytes;
    char*                     dynstr;
    size_t                    dynstr_bytes;
};

/* co_metadata.c: the MessagePack note to bc250hsa_kernel_internal. */
bc250hsa_status bc250hsa_metadata_parse(const uint8_t* note, size_t note_bytes,
                                        struct bc250hsa_module* mod);
/* co_metadata.c: the descriptor bytes to the fields of one kernel. */
bc250hsa_status bc250hsa_descriptor_read(const uint8_t* bytes, size_t byte_count,
                                        bc250hsa_kernel_internal* kernel);

/* --------------------------------------------------------------------------
 * The host allocator the loader tests use, and the kernarg/pm4 seams
 * ------------------------------------------------------------------------ */

/* pm4_dispatch.c, shared with submit.c. */
bc250hsa_status bc250hsa_pm4_check_dispatch(const bc250hsa_dispatch* dispatch,
                                            uint32_t lds_bytes_per_workgroup);

/* Close only releases resources after positive retirement. The callbacks make the
 * exact close decision testable without a KMT device, including failure ordering. */
typedef struct bc250hsa_close_ops {
    void* ctx;
    bc250hsa_status (*flush)(void* ctx);
    bc250hsa_status (*wait)(void* ctx);
    void (*release)(void* ctx);
    void (*quarantine)(void* ctx, bc250hsa_status status);
} bc250hsa_close_ops;
void bc250hsa_finish_close(const bc250hsa_close_ops* ops);

/* Fixed-private scratch for gfx1013. GFX10 WAVES counts the whole GPU, not
 * per-SE slots. 32 resident waves accommodate the admitted 1024-thread maximum.
 * Slots are disjoint between in-flight IBs; reuse requires the slot fence. */
#define BC250HSA_SCRATCH_WAVES 32u
#define BC250HSA_SCRATCH_ALIGNMENT 4096u
typedef struct bc250hsa_scratch_plan {
    uint32_t bytes_per_thread;
    uint32_t bytes_per_wave;
    uint32_t tmpring_size;
    uint64_t bytes;
} bc250hsa_scratch_plan;
bc250hsa_status bc250hsa_plan_scratch(uint32_t private_bytes, uint32_t wave_size,
                                      bc250hsa_scratch_plan* out);
bc250hsa_status bc250hsa_scratch_resource(uint64_t va, uint64_t bytes,
                                          const bc250hsa_scratch_plan* plan,
                                          uint32_t wave_size, uint32_t out[4]);
/* Allocator seam is shared by the real retired-slot path and offline lifetime tests.
 * UINT64_MAX is removal, never a completed fence. Failure leaves the old BO intact. */
bc250hsa_status bc250hsa_prepare_scratch(bc250hsa_mem* slot, uint64_t required_fence,
                                         uint64_t observed_fence,
                                         const bc250hsa_scratch_plan* plan,
                                         const bc250hsa_allocator* allocator);

/* The dword writer of pm4_dispatch.c, and the three steps of one indirect buffer.
 * bc250hsa_pm4_build_batch is these three in a row, and submit.c calls them one at a
 * time: the head when it opens a batch, one append per dispatch, the tail when it
 * submits. A buffer is therefore written once and not rebuilt per dispatch.
 *
 * It holds no state of its own: submit.c makes the writer again from the slot and the
 * dword count it already keeps. overflow says that the buffer was full; the count then
 * stops growing and every later put is dropped, so a caller checks the flag. */
typedef struct bc250hsa_pm4_writer {
    uint32_t* dwords;
    uint32_t  capacity;
    uint32_t  count;
    int       overflow;
} bc250hsa_pm4_writer;

/* The compute state that the previous dispatch of the same indirect buffer left in the
 * hardware. A SET_SH_REG write is persistent register state: DISPATCH_DIRECT does not
 * clear it, so a second dispatch in the same buffer has to write only the registers
 * whose value differs. Everything that never differs (the three start registers, the
 * shader checksum, the six request-control registers, the coherency start delay, the
 * two compute-unit masks, the scratch ring size and the resource limits) is written by
 * the first dispatch of the buffer and by nothing after it.
 *
 * The cache belongs to one indirect buffer and to nothing wider. Another context's
 * buffer runs between two of ours, and this build programs no state at the ring frame,
 * so the first dispatch of every buffer writes the whole sequence. `valid` is what
 * says whether anything may be left out, and submit.c resets it when it opens a buffer.
 *
 * A NULL cache, and BC250HSA_DISPATCH_FULL_STATE, both mean "write everything": the
 * golden single-dispatch stream of test_pm4.c goes through the NULL path, and the flag
 * is the switch a lab arm turns the whole mechanism off with.
 *
 * One invariant the cache does not police, because it holds register values and not
 * policy: which packets count as constant depends on BC250HSA_DISPATCH_NO_CU_MASK of
 * the dispatch that wrote them, and that flag arrives per dispatch through
 * bc250hsa_pm4_env. The pure builder cannot break it (bc250hsa_pm4_build_batch takes
 * one env for the whole buffer) and layer 2 cannot either (the flag is process-wide,
 * runtime/hip_device.cpp). A caller that varies that one flag between two dispatches of
 * one buffer must set BC250HSA_DISPATCH_FULL_STATE on the dispatch that changes it, or
 * the two compute-unit masks of the first dispatch stay as the buffer before ours left
 * them. That is a scheduling cost and never a wrong result, which is why it is an
 * invariant here and not a comparison in the loop. */
typedef struct bc250hsa_pm4_state {
    uint32_t valid;                            /* 0: nothing is known, write it all */
    uint32_t pgm_lo;
    uint32_t pgm_hi;
    uint32_t rsrc1;
    uint32_t rsrc2;
    uint32_t rsrc3;
    uint32_t tmpring_size;
    uint32_t block[3];
    uint32_t user_sgpr_count;
    uint32_t user_sgpr[BC250HSA_MAX_USER_SGPR];
} bc250hsa_pm4_state;

void bc250hsa_pm4_writer_init(bc250hsa_pm4_writer* w, uint32_t* dwords, uint32_t capacity,
                              uint32_t count);
void bc250hsa_pm4_ib_head(bc250hsa_pm4_writer* w, const bc250hsa_pm4_env* env);
/* first 1: no barrier in front, because the head's acquire already covers it, and the
 * whole compute state is written whatever `state` holds.
 * state NULL: write the whole compute state for every dispatch, as build 1 did. */
bc250hsa_status bc250hsa_pm4_ib_append(bc250hsa_pm4_writer* w, const bc250hsa_dispatch* dispatch,
                                       const bc250hsa_pm4_env* env, int first,
                                       bc250hsa_pm4_state* state);
bc250hsa_status bc250hsa_pm4_ib_tail(bc250hsa_pm4_writer* w, const bc250hsa_pm4_env* env);

/* --------------------------------------------------------------------------
 * The GPU copy and fill: PM4 DMA_DATA in an indirect buffer of its own on the copy
 * context (scratch/hip-gpu-copy/CONTRACT.md sections 4 and 7, deviation 13.23)
 * ------------------------------------------------------------------------ */

/* One copy or fill the caller wants done. A list of them goes into one indirect buffer
 * under one fence. The longest list this build produces is a fill's three parts
 * (kmt_memory.c, bc250hsa_fill_device); a row-by-row two-dimensional copy is still one
 * call per row and therefore one submission per row, which CONTRACT.md deviation 13.10
 * states and costs. */
#define BC250HSA_COPY_OP_COPY 0u
#define BC250HSA_COPY_OP_FILL 1u
typedef struct bc250hsa_copy_op {
    uint32_t kind;      /* BC250HSA_COPY_OP_COPY or BC250HSA_COPY_OP_FILL */
    uint32_t pattern;   /* the fill's dword pattern; unused by a copy */
    uint64_t src_va;    /* the copy's source; unused by a fill */
    uint64_t dst_va;
    uint64_t bytes;
} bc250hsa_copy_op;

/* The dwords one operation needs, chunk count included. 0 for 0 bytes, which no caller
 * may append: a DMA_DATA of 0 bytes is not a no-operation this build relies on. */
uint32_t bc250hsa_pm4_copy_dwords(uint64_t bytes);

/* The pure emitters. They touch no device, so tests/host/test_pm4.c compares their
 * dwords with the encoding of pm4_regs.h and with driver/kmd/gfx_copy.c.
 *
 * wait_before sets RAW_WAIT on the first chunk, which orders this operation behind an
 * earlier CP DMA of the same buffer. sync_after sets CP_SYNC on the last chunk, which
 * makes the command processor wait for the copy before it runs the next packet. A caller
 * that appends several operations under one fence sets sync_after on the last one only
 * when nothing after it in the buffer reads the destination; this build sets both on
 * every operation, as it did when a batch could carry a dispatch after a copy. */
bc250hsa_status bc250hsa_pm4_append_copy(bc250hsa_pm4_writer* w, uint64_t dst_va,
                                         uint64_t src_va, uint64_t bytes, int wait_before,
                                         int sync_after);
bc250hsa_status bc250hsa_pm4_append_fill(bc250hsa_pm4_writer* w, uint64_t dst_va,
                                         uint32_t pattern, uint64_t bytes, int wait_before,
                                         int sync_after);
/* One list of operations, in order. It is what submit.c writes into a copy buffer. */
bc250hsa_status bc250hsa_pm4_append_ops(bc250hsa_pm4_writer* w, const bc250hsa_copy_op* ops,
                                        uint32_t count);

/* Do the two ranges overlap? A DMA_DATA over an overlap is undefined, and
 * driver/kmd/gfx_copy.c refuses one, so the caller answers this before it appends. */
int bc250hsa_ranges_overlap(uint64_t a, uint64_t b, uint64_t bytes);

/* The staging ring of CONTRACT.md section 5b, as two pure decisions:
 *
 *   bc250hsa_stage_pick: which slot may be written now. A slot is free when the fence
 *     value of the copy that last wrote it has retired, which is take_slot's own rule
 *     (submit.c:187-216). It answers the slot index, or BC250HSA_EBUSY when every slot
 *     is still in flight and the caller must wait for one.
 *   bc250hsa_stage_round: how many bytes the next round moves.
 *
 * They are separate from the device so that the wrap and the multi-round split are
 * tested with no adapter. */
bc250hsa_status bc250hsa_stage_pick(const uint64_t* slot_fence, uint32_t slot_count,
                                    uint64_t observed_fence, uint32_t next_slot,
                                    uint32_t* slot_out);
uint64_t bc250hsa_stage_round(uint64_t remaining, uint64_t slot_bytes);

/* The three parts of a fill, as a third pure decision. bc250hsa_pm4_append_fill needs a
 * 4-byte aligned address and size (radv_cp_dma.c:334), so a range that starts or ends
 * between two dword boundaries has one to three bytes at that end which the fill packet
 * cannot carry. This splits the range into the head bytes, the 4-byte aligned interior and
 * the tail bytes; kmt_memory.c fills the interior with one packet and copies each edge out
 * of a staging slot. The three parts tile the range in address order, head_bytes and
 * tail_bytes are each 0 to 3, and interior_bytes is a multiple of 4. A part with 0 bytes
 * is simply not appended, and the interior and the tail start on a dword boundary
 * whenever their own byte count is not 0, which is what the fill packet needs. A range
 * that holds no whole dword is the head alone: the other two are then 0 bytes and their
 * addresses mean nothing. */
typedef struct bc250hsa_fill_plan {
    uint64_t head_va;
    uint32_t head_bytes;      /* 0 to 3 */
    uint64_t interior_va;     /* a multiple of 4 when interior_bytes is not 0 */
    uint64_t interior_bytes;  /* a multiple of 4 */
    uint64_t tail_va;         /* a multiple of 4 when tail_bytes is not 0 */
    uint32_t tail_bytes;      /* 0 to 3 */
} bc250hsa_fill_plan;

bc250hsa_status bc250hsa_fill_split(uint64_t va, uint64_t bytes, bc250hsa_fill_plan* out);

#endif /* BC250HSA_INTERNAL_H */
