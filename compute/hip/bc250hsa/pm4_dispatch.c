/* pm4_dispatch.c - the PM4 stream of one gfx1013 compute dispatch, the user SGPR plan
 * and the buffer resource.
 *
 * Layer 1 of docs/design/m16-hip-route-b.md, section 3.7. The whole file touches no
 * device and no operating system, so tests/host/test_pm4.c compares the result with a
 * golden dword stream.
 *
 * The packet order is the libdrm gfx10 dispatch (tests/amdgpu/shader_test_util.c of
 * libdrm-2.4.114, commit b9ca37b3) as driver/shim/bc250_dispatch.c already emits it,
 * plus the graphics-ring CONTEXT_CONTROL in front and the RELEASE_MEM completion write
 * at the end. The 72 dwords of fact M49 are the measured control.
 */
#include "internal.h"
#include "pm4_regs.h"

typedef bc250hsa_pm4_writer writer;

void bc250hsa_pm4_writer_init(writer* w, uint32_t* dwords, uint32_t capacity, uint32_t count)
{
    w->dwords = dwords;
    w->capacity = capacity;
    w->count = count;
    w->overflow = (count > capacity) ? 1 : 0;
}

static void put(writer* w, uint32_t value)
{
    if (w->count >= w->capacity) {
        w->overflow = 1;
        return;
    }
    w->dwords[w->count++] = value;
}

/* One SET_SH_REG naming `count` consecutive registers from `first`. Every compute
 * register write of the sequence has this shape. */
static void set_sh(writer* w, uint32_t first, const uint32_t* values, uint32_t count)
{
    uint32_t i;
    put(w, BC250HSA_PACKET3_COMPUTE(BC250HSA_PKT3_SET_SH_REG, count));
    put(w, first);
    for (i = 0; i < count; i++) {
        put(w, values[i]);
    }
}

static void set_sh1(writer* w, uint32_t first, uint32_t value)
{
    set_sh(w, first, &value, 1u);
}

bc250hsa_status bc250hsa_buffer_resource(uint64_t va, uint64_t bytes, uint32_t out_dwords[4])
{
    if (out_dwords == NULL) {
        return BC250HSA_EINVAL;
    }
    if ((va >> 48) != 0u || bytes > 0xFFFFFFFFu) {
        return BC250HSA_EINVAL;
    }
    /* A raw buffer: stride 0, so NUM_RECORDS counts bytes. Word 3 is the
     * libdrm-proven gfx10.1 value (FORMAT 32_32_32_32 uint, RESOURCE_LEVEL 1,
     * OOB_SELECT 1, TYPE 0). */
    out_dwords[0] = (uint32_t)va;
    out_dwords[1] = (uint32_t)(va >> 32) & 0xFFFFu;
    out_dwords[2] = (uint32_t)bytes;
    out_dwords[3] = BC250HSA_BUFFER_RSRC_W3;
    return BC250HSA_OK;
}

/* LLVM AMDGPUUsage: private segment size is DWORD-rounded per work item.
 * Mesa ac_shader_util.c: GFX10 WAVESIZE is in 1024-byte units, WAVES is global.
 * Keep this pure so range and retirement controls need no adapter. */
bc250hsa_status bc250hsa_plan_scratch(uint32_t private_bytes, uint32_t wave_size,
                                      bc250hsa_scratch_plan* out)
{
    uint64_t thread_bytes, wave_bytes;
    if (out == NULL || (wave_size != 32u && wave_size != 64u)) {
        return BC250HSA_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    if (private_bytes == 0u) { return BC250HSA_OK; }
    thread_bytes = ((uint64_t)private_bytes + 3u) & ~(uint64_t)3u;
    wave_bytes = (thread_bytes * wave_size + 1023u) & ~(uint64_t)1023u;
    if (thread_bytes > UINT32_MAX ||
        wave_bytes / BC250HSA_SCRATCH_WAVESIZE_GRANULE > BC250HSA_SCRATCH_WAVESIZE_MASK) {
        return BC250HSA_EUNSUPPORTED;
    }
    out->bytes_per_thread = (uint32_t)thread_bytes;
    out->bytes_per_wave = (uint32_t)wave_bytes;
    out->bytes = wave_bytes * BC250HSA_SCRATCH_WAVES;
    out->tmpring_size = BC250HSA_SCRATCH_WAVES |
        ((uint32_t)(wave_bytes / BC250HSA_SCRATCH_WAVESIZE_GRANULE)
         << BC250HSA_SCRATCH_WAVESIZE_SHIFT);
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_scratch_resource(uint64_t va, uint64_t bytes,
                                          const bc250hsa_scratch_plan* plan,
                                          uint32_t wave_size, uint32_t out[4])
{
    if (plan == NULL || out == NULL || (wave_size != 32u && wave_size != 64u) ||
        plan->bytes == 0u || bytes < plan->bytes || va == 0u ||
        (va & (BC250HSA_SCRATCH_ALIGNMENT - 1u)) != 0u || va >= (1ull << 48) ||
        bytes > (1ull << 48) - va) {
        return BC250HSA_EINVAL;
    }
    /* HSA preloads these four SGPRs; the prologue adds the SPI per-wave offset.
     * NUM_RECORDS is the compiler's unbounded scratch descriptor, not BO bytes.
     * TMPRING_SIZE bounds the ring allocation independently. */
    out[0] = (uint32_t)va;
    out[1] = (uint32_t)(va >> 32) | BC250HSA_SCRATCH_SWIZZLE_ENABLE;
    out[2] = UINT32_MAX;
    out[3] = (BC250HSA_SCRATCH_FORMAT_32_FLOAT << BC250HSA_SCRATCH_FORMAT_SHIFT) |
        ((wave_size == 32u ? 2u : 3u) << BC250HSA_SCRATCH_INDEX_STRIDE_SHIFT) |
        BC250HSA_SCRATCH_ADD_TID_ENABLE | BC250HSA_SCRATCH_RESOURCE_LEVEL |
        BC250HSA_SCRATCH_OOB_SELECT;
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_prepare_scratch(bc250hsa_mem* slot, uint64_t required_fence,
                                         uint64_t observed_fence,
                                         const bc250hsa_scratch_plan* plan,
                                         const bc250hsa_allocator* allocator)
{
    bc250hsa_mem replacement;
    bc250hsa_status status;
    if (slot == NULL || plan == NULL || allocator == NULL ||
        allocator->alloc == NULL || allocator->free == NULL || plan->bytes == 0u) {
        return BC250HSA_EINVAL;
    }
    if (observed_fence == UINT64_MAX) { return BC250HSA_EDEVICELOST; }
    if (observed_fence < required_fence) { return BC250HSA_EBUSY; }
    if (slot->opaque != NULL && slot->bytes >= plan->bytes) { return BC250HSA_OK; }
    memset(&replacement, 0, sizeof(replacement));
    status = allocator->alloc(allocator->ctx, plan->bytes, BC250HSA_SCRATCH_ALIGNMENT,
                                BC250HSA_MEM_DEVICE, &replacement);
    if (status != BC250HSA_OK) { return status; }
    if (replacement.bytes < plan->bytes || replacement.va == 0u ||
        (replacement.va & (BC250HSA_SCRATCH_ALIGNMENT - 1u)) != 0u ||
        replacement.va >= (1ull << 48) || replacement.bytes > (1ull << 48) - replacement.va) {
        allocator->free(allocator->ctx, &replacement);
        return BC250HSA_EINVAL;
    }
    if (slot->opaque != NULL) { allocator->free(allocator->ctx, slot); }
    *slot = replacement;
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_plan_user_sgprs(const bc250hsa_kernel* kernel, uint64_t kernarg_va,
                                         uint64_t dispatch_packet_va,
                                         const uint32_t private_segment_rsrc[4],
                                         bc250hsa_user_sgpr_plan* out)
{
    uint16_t properties;

    if (kernel == NULL || out == NULL || private_segment_rsrc == NULL) {
        return BC250HSA_EINVAL;
    }
    if (!bc250hsa_struct_bytes_ok(out->struct_bytes, sizeof(*out))) {
        return BC250HSA_EINVAL;
    }
    properties = kernel->kernel_code_properties;
    /* Queue pointers and dispatch IDs need an HSA queue contract this PM4 path
     * does not expose. Fixed scratch values are supplied below in ABI order. */
    if ((properties & (uint16_t)(BC250HSA_KCP_QUEUE_PTR | BC250HSA_KCP_DISPATCH_ID)) != 0u) {
        bc250hsa_log(BC250HSA_LOG_ERROR,
                     "kernel %s enables a user SGPR item this build does not program (0x%04x)",
                     kernel->name, (unsigned)properties);
        return BC250HSA_EUNSUPPORTED;
    }
    /* The dispatch pointer is programmed from the AQL packet of section 7.1 of the
     * header. Without a packet the register would point nowhere, and a kernel that
     * reads its own blockDim through it would read rubbish, so that is refused. */
    if ((properties & BC250HSA_KCP_DISPATCH_PTR) != 0u && dispatch_packet_va == 0u) {
        bc250hsa_log(BC250HSA_LOG_ERROR,
                     "kernel %s reads the AQL dispatch packet and this dispatch carries none",
                     kernel->name);
        return BC250HSA_EUNSUPPORTED;
    }

    out->count = 0;
    /* The documented order: private segment buffer (4), dispatch pointer (2), queue
     * pointer (2), kernel argument pointer (2), dispatch id (2), flat scratch init
     * (2), private segment size (1). Each enabled item takes the next free
     * COMPUTE_USER_DATA register, so nothing is hard-coded to s[0:3] and s[4:5]. */
    if ((properties & BC250HSA_KCP_PRIVATE_SEGMENT_BUFFER) != 0u) {
        uint32_t i;
        if (out->count + 4u > BC250HSA_MAX_USER_SGPR) {
            return BC250HSA_EUNSUPPORTED;
        }
        for (i = 0; i < 4u; i++) {
            out->value[out->count++] = private_segment_rsrc[i];
        }
    }
    if ((properties & BC250HSA_KCP_DISPATCH_PTR) != 0u) {
        if (out->count + 2u > BC250HSA_MAX_USER_SGPR) {
            return BC250HSA_EUNSUPPORTED;
        }
        out->value[out->count++] = (uint32_t)dispatch_packet_va;
        out->value[out->count++] = (uint32_t)(dispatch_packet_va >> 32);
    }
    if ((properties & BC250HSA_KCP_KERNARG_SEGMENT_PTR) != 0u) {
        if (out->count + 2u > BC250HSA_MAX_USER_SGPR) {
            return BC250HSA_EUNSUPPORTED;
        }
        out->value[out->count++] = (uint32_t)kernarg_va;
        out->value[out->count++] = (uint32_t)(kernarg_va >> 32);
    }
    if ((properties & BC250HSA_KCP_FLAT_SCRATCH_INIT) != 0u) {
        if (out->count + 2u > BC250HSA_MAX_USER_SGPR) { return BC250HSA_EUNSUPPORTED; }
        out->value[out->count++] = private_segment_rsrc[0];
        out->value[out->count++] = private_segment_rsrc[1] & 0xFFFFu;
    }
    if ((properties & BC250HSA_KCP_PRIVATE_SEGMENT_SIZE) != 0u) {
        const uint64_t bytes = ((uint64_t)kernel->private_segment_bytes + 3u) & ~(uint64_t)3u;
        if (out->count == BC250HSA_MAX_USER_SGPR || bytes > UINT32_MAX) {
            return BC250HSA_EUNSUPPORTED;
        }
        out->value[out->count++] = (uint32_t)bytes;
    }
    /* COMPUTE_PGM_RSRC2.USER_SGPR tells the hardware how many registers the
     * prologue reads. A plan that does not fill exactly that many would leave the
     * kernel reading an undefined register. */
    if (out->count != (uint32_t)kernel->user_sgpr_count) {
        bc250hsa_log(BC250HSA_LOG_ERROR,
                     "kernel %s wants %u user SGPRs, the plan fills %u", kernel->name,
                     (unsigned)kernel->user_sgpr_count, out->count);
        return BC250HSA_EUNSUPPORTED;
    }
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_pm4_check_dispatch(const bc250hsa_dispatch* dispatch,
                                            uint32_t lds_bytes_per_workgroup)
{
    const bc250hsa_kernel* k;
    uint64_t               block_product;
    uint64_t               group_bytes;
    uint32_t               i;

    if (dispatch == NULL || dispatch->kernel == NULL) {
        return BC250HSA_EINVAL;
    }
    if (!bc250hsa_struct_bytes_ok(dispatch->struct_bytes, sizeof(*dispatch)) ||
        !bc250hsa_struct_bytes_ok(dispatch->launch.struct_bytes, sizeof(dispatch->launch))) {
        return BC250HSA_EINVAL;
    }
    k = dispatch->kernel;
    for (i = 0; i < 3u; i++) {
        if (dispatch->launch.grid[i] == 0u || dispatch->launch.block[i] == 0u || dispatch->launch.block[i] > 1024u) {
            return BC250HSA_EINVAL;
        }
    }
    block_product = (uint64_t)dispatch->launch.block[0] * dispatch->launch.block[1] *
                    dispatch->launch.block[2];
    if (block_product > 1024u) { return BC250HSA_EINVAL; }
    if (k->max_flat_workgroup_size != 0u && block_product > (uint64_t)k->max_flat_workgroup_size) {
        return BC250HSA_EINVAL;
    }
    group_bytes = (uint64_t)k->group_segment_bytes + dispatch->launch.dynamic_group_bytes;
    if (lds_bytes_per_workgroup != 0u && group_bytes > (uint64_t)lds_bytes_per_workgroup) {
        return BC250HSA_EINVAL;
    }
    if (group_bytes / BC250HSA_LDS_GRANULE_BYTES > BC250HSA_RSRC2_LDS_SIZE_MASK) {
        return BC250HSA_EINVAL;
    }
    /* The wave size of a GFX10 compute dispatch is selected in
     * COMPUTE_DISPATCH_INITIATOR.CS_W32_EN and nowhere else. A wave64 kernel
     * therefore needs a dispatch this build does not write, and a kernel whose two
     * statements of its own wave size disagree is not understood at all: both are
     * refused by name instead of launched in the wrong wave size, where EXEC is half
     * the width the code manages and every lane mask is wrong. */
    if ((k->kernel_code_properties & BC250HSA_KCP_WAVEFRONT_SIZE32) == 0u) {
        bc250hsa_log(BC250HSA_LOG_ERROR,
                     "kernel %s is wave64, which this build does not dispatch", k->name);
        return BC250HSA_EUNSUPPORTED;
    }
    if (k->wave_size != 32u) {
        bc250hsa_log(BC250HSA_LOG_ERROR,
                     "kernel %s says wavefront size %u in its metadata and wave32 in its"
                     " descriptor", k->name, (unsigned)k->wave_size);
        return BC250HSA_EUNSUPPORTED;
    }
    /* A fixed frame has a bounded ring. A dynamic stack needs a separate policy
     * and remains refused instead of pretending the fixed metadata is a bound. */
    if (k->uses_dynamic_stack != 0u) {
        bc250hsa_count_add(BC250HSA_C_DYNAMIC_STACK_REFUSALS, 1u);
        return BC250HSA_EUNSUPPORTED;
    }
    if (k->private_segment_bytes != 0u) {
        bc250hsa_scratch_plan scratch;
        bc250hsa_status status = bc250hsa_plan_scratch(k->private_segment_bytes, k->wave_size, &scratch);
        if (status != BC250HSA_OK) { return status; }
        if ((k->compute_pgm_rsrc2 & BC250HSA_SCRATCH_EN) == 0u) {
            return BC250HSA_EUNSUPPORTED;
        }
    }
    if (dispatch->kernarg_va != 0u && k->kernarg_align != 0u &&
        (dispatch->kernarg_va % (uint64_t)k->kernarg_align) != 0u) {
        return BC250HSA_EINVAL;
    }
    if (k->kernarg_bytes != 0u && dispatch->kernarg_va == 0u) {
        return BC250HSA_EINVAL;
    }
    /* Section 7.1 of the header: a kernel that reads the AQL dispatch packet needs the
     * packet, and the refusal names the kernel here as well, so that the reason is in
     * the log before the plan runs. */
    if ((k->kernel_code_properties & BC250HSA_KCP_DISPATCH_PTR) != 0u &&
        dispatch->dispatch_packet_va == 0u) {
        bc250hsa_log(BC250HSA_LOG_ERROR,
                     "kernel %s reads the AQL dispatch packet and this dispatch carries none",
                     k->name);
        return BC250HSA_EUNSUPPORTED;
    }
    if (dispatch->dispatch_packet_va != 0u &&
        (dispatch->dispatch_packet_va % (uint64_t)BC250HSA_AQL_PACKET_ALIGN) != 0u) {
        return BC250HSA_EINVAL;
    }
    return BC250HSA_OK;
}

/* Packet 2 of the stream, with the acquire the caller asks for: the full one at the head
 * of an indirect buffer, and the light one of BC250HSA_ACQUIRE_GCR_CNTL_LIGHT between two
 * dispatches of one batch when BC250HSA_DISPATCH_LIGHT_BARRIER is set (section 8.1). */
static void acquire_mem(writer* w, uint32_t gcr_cntl)
{
    put(w, BC250HSA_PACKET3(BC250HSA_PKT3_ACQUIRE_MEM, 6u));
    put(w, 0u);                 /* CP_COHER_CNTL */
    put(w, 0xFFFFFFFFu);        /* CP_COHER_SIZE */
    put(w, 0x00FFFFFFu);        /* CP_COHER_SIZE_HI */
    put(w, 0u);                 /* CP_COHER_BASE */
    put(w, 0u);                 /* CP_COHER_BASE_HI */
    put(w, BC250HSA_ACQUIRE_POLL_INTERVAL);
    put(w, gcr_cntl);
}

/* The head of an indirect buffer: packets 1 and 2. */
void bc250hsa_pm4_ib_head(writer* w, const bc250hsa_pm4_env* env)
{
    /* 1. The graphics ring needs CONTEXT_CONTROL in front of the first state write
     *    of an indirect buffer; it is a no-operation on a compute ring. */
    if ((env->flags & BC250HSA_DISPATCH_GFX_RING) != 0u) {
        put(w, BC250HSA_PACKET3(BC250HSA_PKT3_CONTEXT_CONTROL, 1u));
        put(w, BC250HSA_CONTEXT_CONTROL_DW);
        put(w, BC250HSA_CONTEXT_CONTROL_DW);
    }

    /* 2. The cache acquire the kernel driver's ring frame does not emit. Without it
     *    the shader can be fetched through a stale instruction cache line. */
    if ((env->flags & BC250HSA_DISPATCH_NO_ACQUIRE) == 0u) {
        acquire_mem(w, BC250HSA_ACQUIRE_GCR_CNTL);
    }
}

/* The barrier between dispatch i and dispatch i+1 of one indirect buffer. The wait for
 * the waves is already behind dispatch i: it is packet 17 of its body. What is left is
 * the acquire that makes dispatch i's writes visible to dispatch i+1. */
static void build_inter_dispatch_barrier(writer* w, const bc250hsa_pm4_env* env)
{
    if ((env->flags & BC250HSA_DISPATCH_NO_ACQUIRE) != 0u) {
        return;
    }
    acquire_mem(w, ((env->flags & BC250HSA_DISPATCH_LIGHT_BARRIER) != 0u)
                       ? BC250HSA_ACQUIRE_GCR_CNTL_LIGHT
                       : BC250HSA_ACQUIRE_GCR_CNTL);
}

/* The completion write, packet 18, and the padding of packet 19. */
bc250hsa_status bc250hsa_pm4_ib_tail(writer* w, const bc250hsa_pm4_env* env)
{
    uint32_t pad_to;
    uint32_t at;

    if ((env->flags & BC250HSA_DISPATCH_NO_FENCE) == 0u) {
        put(w, BC250HSA_PACKET3(BC250HSA_PKT3_RELEASE_MEM, 6u));
        put(w, BC250HSA_RELEASE_MEM_DW1);
        put(w, BC250HSA_RELEASE_MEM_DW2);
        put(w, (uint32_t)env->fence_va);
        put(w, (uint32_t)(env->fence_va >> 32));
        put(w, (uint32_t)env->fence_value);
        put(w, (uint32_t)(env->fence_value >> 32));
        put(w, 0u);   /* INT_CTXID */
    }

    /* 19. The graphics ring pads an indirect buffer to its own granularity. */
    pad_to = (env->ib_pad_dwords == 0u) ? BC250HSA_IB_PAD_DWORDS : env->ib_pad_dwords;
    if (!bc250hsa_is_power_of_two_u64(pad_to)) {
        return BC250HSA_EINVAL;
    }
    at = bc250hsa_align_up_u32(w->count, pad_to);
    /* put() does not advance the count once the buffer is full, so this loop must stop
     * on the overflow flag and not on the count alone. */
    while (w->count < at && !w->overflow) {
        put(w, BC250HSA_CP_NOP);
    }
    return BC250HSA_OK;
}

/* Does this dispatch need the run of user data registers written again? A different
 * count, or any different value, and the whole run goes out: one SET_SH_REG names
 * consecutive registers, so there is nothing to gain from writing part of it. */
static int user_sgprs_differ(const bc250hsa_pm4_state* state,
                             const bc250hsa_user_sgpr_plan* plan)
{
    uint32_t i;

    if (state->user_sgpr_count != plan->count) {
        return 1;
    }
    for (i = 0; i < plan->count; i++) {
        if (state->user_sgpr[i] != plan->value[i]) {
            return 1;
        }
    }
    return 0;
}

/* Packets 3 to 17 of the stream: everything that belongs to one dispatch, the head of
 * the indirect buffer and the completion write apart, with the barrier of section 8.1 in
 * front of every dispatch but the first.
 *
 * With a `state` cache (section 8.8 of the design) a dispatch that follows another one
 * in the same buffer writes only the registers whose value changed. The first dispatch
 * of a buffer, a NULL cache and BC250HSA_DISPATCH_FULL_STATE all write the whole
 * sequence, so the stream of one dispatch by itself is unchanged, dword for dword. */
bc250hsa_status bc250hsa_pm4_ib_append(writer* w, const bc250hsa_dispatch* dispatch,
                                       const bc250hsa_pm4_env* env, int first,
                                       bc250hsa_pm4_state* state)
{
    const bc250hsa_kernel*  k;
    bc250hsa_user_sgpr_plan plan;
    bc250hsa_status         status;
    const uint32_t          zero[6] = { 0, 0, 0, 0, 0, 0 };
    uint32_t                values[4];
    uint32_t                rsrc2;
    uint32_t                scratch_rsrc[4];
    uint32_t                tmpring_size = 0u;
    uint32_t                initiator;
    uint32_t                pgm_lo;
    uint32_t                pgm_hi;
    int                     full;
    uint32_t                i;

    if (w == NULL || dispatch == NULL || env == NULL) {
        return BC250HSA_EINVAL;
    }
    status = bc250hsa_pm4_check_dispatch(dispatch, 0u);
    if (status != BC250HSA_OK) {
        return status;
    }
    k = dispatch->kernel;

    memcpy(scratch_rsrc, env->private_segment_rsrc, sizeof(scratch_rsrc));
    if (k->private_segment_bytes != 0u) {
        bc250hsa_scratch_plan scratch;
        if (env->struct_bytes != sizeof(*env)) { return BC250HSA_EINVAL; }
        status = bc250hsa_plan_scratch(k->private_segment_bytes, k->wave_size, &scratch);
        if (status != BC250HSA_OK) { return status; }
        status = bc250hsa_scratch_resource(env->scratch_va, env->scratch_bytes,
                                             &scratch, k->wave_size, scratch_rsrc);
        if (status != BC250HSA_OK) { return status; }
        tmpring_size = scratch.tmpring_size;
        /* Different fixed frames may not reinterpret one live scratch BO's stride.
         * Production submits scratch alone; the public batch builder rejects it. */
        if (!first) { return BC250HSA_EUNSUPPORTED; }
    }
    plan.struct_bytes = (uint32_t)sizeof(plan);
    status = bc250hsa_plan_user_sgprs(k, dispatch->kernarg_va, dispatch->dispatch_packet_va,
                                      scratch_rsrc, &plan);
    if (status != BC250HSA_OK) {
        return status;
    }

    /* The whole sequence goes out when the hardware state behind this dispatch is not
     * known: the first dispatch of the buffer, a caller that keeps no cache, and the
     * switch that turns the mechanism off. */
    full = first || state == NULL || state->valid == 0u ||
           (env->flags & BC250HSA_DISPATCH_FULL_STATE) != 0u;

    if (!first) {
        build_inter_dispatch_barrier(w, env);
    }

    if (full) {
        /* 3 to 5. COMPUTE_START_X/Y/Z, the shader checksum and COMPUTE_REQ_CTRL with
         *    the five registers behind it, all zero, as the reference writes them.
         *    Constant for the life of the buffer, so only the first dispatch of it
         *    writes them. */
        set_sh(w, BC250HSA_REG_COMPUTE_START_X, zero, 3u);
        set_sh1(w, BC250HSA_REG_COMPUTE_SHADER_CHKSUM, 0u);
        set_sh(w, BC250HSA_REG_COMPUTE_REQ_CTRL, zero, 6u);

        /* 6. The one uconfig write of the sequence, and the one packet with no
         *    shader-type bit. A performance knob of ACQUIRE_MEM. */
        put(w, BC250HSA_PACKET3(BC250HSA_PKT3_SET_UCONFIG_REG, 1u));
        put(w, BC250HSA_REG_CP_COHER_START_DELAY);
        put(w, BC250HSA_COHER_START_DELAY);

        /* 7 and 8. Every compute unit of all four shader engines, through the command
         *    processor's own mask path. The first two packets to drop when the command
         *    processor refuses the opcode (open question 2 of the design). */
        if ((env->flags & BC250HSA_DISPATCH_NO_CU_MASK) == 0u) {
            put(w, BC250HSA_PACKET3_COMPUTE(BC250HSA_PKT3_SET_SH_REG_INDEX, 2u));
            put(w,
                (BC250HSA_SH_REG_INDEX_CU << 28) | BC250HSA_REG_COMPUTE_STATIC_THREAD_MGMT_SE0);
            put(w, 0xFFFFFFFFu);
            put(w, 0xFFFFFFFFu);
            put(w, BC250HSA_PACKET3_COMPUTE(BC250HSA_PKT3_SET_SH_REG_INDEX, 2u));
            put(w,
                (BC250HSA_SH_REG_INDEX_CU << 28) | BC250HSA_REG_COMPUTE_STATIC_THREAD_MGMT_SE2);
            put(w, 0xFFFFFFFFu);
            put(w, 0xFFFFFFFFu);
        }
    }

    /* 9. COMPUTE_PGM_LO and _HI: the entry address shifted right by 8, and bits
     *    47:40 of the byte address. The high half is not address32_hi; a build that
     *    kept a preamble value of 0x80 fetched a shader at the wrong address. */
    pgm_lo = (uint32_t)(k->entry_va >> 8);
    pgm_hi = (uint32_t)(k->entry_va >> 40) & 0xFFu;
    if (full || state->pgm_lo != pgm_lo || state->pgm_hi != pgm_hi) {
        values[0] = pgm_lo;
        values[1] = pgm_hi;
        set_sh(w, BC250HSA_REG_COMPUTE_PGM_LO, values, 2u);
    }

    /* 10. RSRC1 unchanged, RSRC2 with the computed LDS_SIZE written into bits 23:15.
     *     Decision 3 of section 2: the command processor normally writes that field
     *     from the AQL packet, there is no AQL packet here, and the descriptor holds
     *     0 (measured on reduce256, which has 1024 bytes of local memory). */
    rsrc2 = k->compute_pgm_rsrc2 &
            ~(uint32_t)(BC250HSA_RSRC2_LDS_SIZE_MASK << BC250HSA_RSRC2_LDS_SIZE_SHIFT);
    rsrc2 |= (bc250hsa_lds_size_field(k->group_segment_bytes,
                                      dispatch->launch.dynamic_group_bytes) &
              BC250HSA_RSRC2_LDS_SIZE_MASK)
             << BC250HSA_RSRC2_LDS_SIZE_SHIFT;
    if (full || state->rsrc1 != k->compute_pgm_rsrc1 || state->rsrc2 != rsrc2) {
        values[0] = k->compute_pgm_rsrc1;
        values[1] = rsrc2;
        set_sh(w, BC250HSA_REG_COMPUTE_PGM_RSRC1, values, 2u);
    }

    /* 11. RSRC3, gfx10 and later, copied unchanged. */
    if (full || state->rsrc3 != k->compute_pgm_rsrc3) {
        set_sh1(w, BC250HSA_REG_COMPUTE_PGM_RSRC3, k->compute_pgm_rsrc3);
    }

    /* Each IB writes its ring size, and the cache tracks transitions explicitly. */
    if (full || state->tmpring_size != tmpring_size) {
        set_sh1(w, BC250HSA_REG_COMPUTE_TMPRING_SIZE, tmpring_size);
    }

    /* 13. The workgroup size in work items. */
    if (full || state->block[0] != dispatch->launch.block[0] ||
        state->block[1] != dispatch->launch.block[1] ||
        state->block[2] != dispatch->launch.block[2]) {
        set_sh(w, BC250HSA_REG_COMPUTE_NUM_THREAD_X, dispatch->launch.block, 3u);
    }

    /* 14. The user data plan, one packet for the whole run of registers. The kernel
     *     argument pointer is in it, and layer 2 hands out another kernel argument
     *     buffer per launch, so in practice this is the one packet that does go out
     *     for every dispatch. */
    if (plan.count != 0u && (full || user_sgprs_differ(state, &plan))) {
        set_sh(w, BC250HSA_REG_COMPUTE_USER_DATA_0, plan.value, plan.count);
    }

    /* 15. COMPUTE_RESOURCE_LIMITS, 0 as the reference writes it. Constant. */
    if (full) {
        set_sh1(w, BC250HSA_REG_COMPUTE_RESOURCE_LIMITS, 0u);
    }

    /* 16. The dispatch itself, in workgroups. CS_W32_EN comes from the kernel and not
     *     from the measured control dispatch, whose shader is wave64: this is the one
     *     register field that selects the wave size of the waves the command processor
     *     starts. bc250hsa_pm4_check_dispatch has already refused everything else. */
    initiator = BC250HSA_DISPATCH_INITIATOR_SHADER_EN;
    if ((k->kernel_code_properties & BC250HSA_KCP_WAVEFRONT_SIZE32) != 0u) {
        initiator |= BC250HSA_DISPATCH_INITIATOR_CS_W32_EN;
    }
    if ((env->flags & BC250HSA_DISPATCH_START_AT_000) != 0u) {
        initiator |= BC250HSA_DISPATCH_INITIATOR_FORCE_START_0;
    }
    put(w, BC250HSA_PACKET3_COMPUTE(BC250HSA_PKT3_DISPATCH_DIRECT, 3u));
    put(w, dispatch->launch.grid[0]);
    put(w, dispatch->launch.grid[1]);
    put(w, dispatch->launch.grid[2]);
    put(w, initiator);

    /* 17. The command processor waits for the waves before the completion write is
     *     allowed to signal, and before the next dispatch of a batch reads what this
     *     one wrote. */
    put(w, BC250HSA_PACKET3(BC250HSA_PKT3_EVENT_WRITE, 0u));
    put(w, BC250HSA_EVENT_TYPE(BC250HSA_EVENT_CS_PARTIAL_FLUSH) |
               BC250HSA_EVENT_INDEX(BC250HSA_EVENT_INDEX_CS_PARTIAL_FLUSH));
    /* A dispatch the buffer had no room for was not built: put() dropped its dwords and
     * submit.c is about to open another buffer and append it again. Counting it here
     * would count it twice, and dispatches_built is the denominator of the ratio this
     * route is judged by (submissions per dispatch), so the count follows the dwords. */
    if (w->overflow) {
        return BC250HSA_OK;
    }
    /* The dwords are in the buffer, so the hardware state they leave behind is now
     * known. A dispatch that overflowed wrote nothing, and submit.c appends it again
     * into the next buffer with `first` set, so the cache must not be touched above. */
    if (state != NULL) {
        state->pgm_lo = pgm_lo;
        state->pgm_hi = pgm_hi;
        state->rsrc1 = k->compute_pgm_rsrc1;
        state->rsrc2 = rsrc2;
        state->rsrc3 = k->compute_pgm_rsrc3;
        state->tmpring_size = tmpring_size;
        state->block[0] = dispatch->launch.block[0];
        state->block[1] = dispatch->launch.block[1];
        state->block[2] = dispatch->launch.block[2];
        state->user_sgpr_count = plan.count;
        for (i = 0; i < plan.count; i++) {
            state->user_sgpr[i] = plan.value[i];
        }
        state->valid = 1u;
    }
    bc250hsa_count_add(BC250HSA_C_DISPATCHES_BUILT, 1u);
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_pm4_build_batch(const bc250hsa_dispatch* dispatches, uint32_t count,
                                         const bc250hsa_pm4_env* env, uint32_t* dwords,
                                         uint32_t dword_capacity, uint32_t* dwords_written)
{
    writer             w;
    bc250hsa_pm4_state state;
    bc250hsa_status    status;
    uint32_t           i;

    if (dispatches == NULL || env == NULL || dwords == NULL || dwords_written == NULL ||
        count == 0u) {
        return BC250HSA_EINVAL;
    }
    if (count > BC250HSA_BATCH_DISPATCHES_MAX) {
        return BC250HSA_EINVAL;
    }
    if (env->struct_bytes != sizeof(*env) && env->struct_bytes != offsetof(bc250hsa_pm4_env, scratch_va)) {
        return BC250HSA_EINVAL;
    }
    if ((env->flags & BC250HSA_DISPATCH_NO_FENCE) == 0u &&
        (env->fence_va == 0u || (env->fence_va & 7u) != 0u)) {
        return BC250HSA_EINVAL;
    }

    if (count > 1u) {
        for (i = 0; i < count; i++) {
            if (dispatches[i].kernel != NULL && dispatches[i].kernel->private_segment_bytes != 0u) {
                return BC250HSA_EUNSUPPORTED;
            }
        }
    }
    bc250hsa_pm4_writer_init(&w, dwords, dword_capacity, 0u);
    memset(&state, 0, sizeof(state));

    bc250hsa_pm4_ib_head(&w, env);
    for (i = 0; i < count; i++) {
        status = bc250hsa_pm4_ib_append(&w, &dispatches[i], env, i == 0u, &state);
        if (status != BC250HSA_OK) {
            return status;
        }
    }
    status = bc250hsa_pm4_ib_tail(&w, env);
    if (status != BC250HSA_OK) {
        return status;
    }

    if (w.overflow) {
        return BC250HSA_ENOMEM;
    }
    *dwords_written = w.count;
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_pm4_build_dispatch(const bc250hsa_dispatch* dispatch,
                                            const bc250hsa_pm4_env* env, uint32_t* dwords,
                                            uint32_t dword_capacity, uint32_t* dwords_written)
{
    if (dispatch == NULL) {
        return BC250HSA_EINVAL;
    }
    /* One dispatch is a batch of one: the head, one body, the completion write and the
     * padding. The golden stream of tests/host/test_pm4.c is the control that this
     * refactoring changed no dword of it. */
    return bc250hsa_pm4_build_batch(dispatch, 1u, env, dwords, dword_capacity, dwords_written);
}
