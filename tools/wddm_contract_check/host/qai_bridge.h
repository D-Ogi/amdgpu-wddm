/* The border between the two halves of this harness.
 *
 * qai_bridge.c is compiled against the WDK kernel headers (ntifs.h, dispmprt.h) together with the real
 * driver\kmd\wddm.c, and must not see the C runtime. qai_test.c is an ordinary console program and must
 * not see the kernel headers. Everything that crosses between them is declared here, in types both
 * compilers agree on without either header set.
 *
 * Nothing in this file describes the driver's answers. It describes the shape of the call: which query
 * type, how large a buffer dxgkrnl would pass, which bytes of that buffer the driver is not obliged to
 * write. What the answers should BE is checked by ..\check.py against the source; what this harness
 * checks is that the answer is written inside its buffer, all of it, and the same way every time.
 */
#ifndef BC250_QAI_BRIDGE_H
#define BC250_QAI_BRIDGE_H

/* Statuses the cases use. Spelled out here so that qai_test.c needs no ntstatus.h. */
#define BC250H_STATUS_SUCCESS            0x00000000L
#define BC250H_STATUS_NOT_SUPPORTED      ((long)0xC00000BBL)
#define BC250H_STATUS_INVALID_PARAMETER  ((long)0xC000000DL)
#define BC250H_STATUS_BUFFER_TOO_SMALL   ((long)0xC0000023L)

/* A byte range of an output buffer that the driver is NOT required to write: an in-out member dxgkrnl
 * fills before the call, or structure padding. Bytes outside every such range must come back identical
 * from two calls made into differently pre-filled buffers, or the driver left them uninitialized. */
struct bc250h_region {
    unsigned    off;
    unsigned    len;
    const char* why;
};

/* How a case wants its output buffer prepared before the call. */
#define BC250H_PREP_NONE            0   /* leave the fill pattern alone */
#define BC250H_PREP_SEGMENT_COUNT   1   /* DXGK_QUERYSEGMENTOUT4, pass 1: NbSegment = 0 */
#define BC250H_PREP_SEGMENT_FILL    2   /* pass 2: NbSegment, pSegmentDescriptor, stride set */
#define BC250H_PREP_SEGMENT_NODESC  3   /* pass 2 with NbSegment non-zero but pSegmentDescriptor NULL */

/* One call of DxgkDdiQueryAdapterInfo, as dxgkrnl would make it. */
struct bc250h_case {
    const char*                 name;
    unsigned                    type;           /* DXGK_QUERYADAPTERINFOTYPE */
    unsigned                    out_size;       /* the size dxgkrnl passes for this type */
    unsigned                    in_size;        /* 0 when the type takes no input structure */
    long                        expect;         /* status of the exact-size call */
    long                        expect_short;   /* status when the buffer is one byte short */
    long                        expect_zero;    /* status when OutputDataSize is 0 */
    long                        expect_null;    /* status when pOutputData is NULL at the full size */
    int                         prep;           /* BC250H_PREP_* */
    int                         writes_whole_buffer; /* 1: the driver zeroes all of OutputDataSize */
    unsigned                    n_ignore;
    const struct bc250h_region* ignore;
    /* BC250H_PREP_SEGMENT_FILL only: the SegmentDescriptorStride to hand the driver. 0 means
     * sizeof(DXGK_SEGMENTDESCRIPTOR4) exactly. dxgkrnl is free to choose a stride larger than the
     * structure it compiled against, and the driver must iterate with the stride it is given, so a
     * larger one has to work and a smaller one has to be refused. Cases written before this field
     * existed leave it 0 and keep the exact stride. */
    unsigned                    stride;
};

/* Start and stop the adapter the calls are made against. vram_enabled = 0 models a start with the
 * EnableVram gate closed, where wddm.c has no segment to declare. Returns 0 on success. */
int  bc250h_start(int vram_enabled);
void bc250h_stop(void);

/* The table WddmBuildTable() produced for this run. */
unsigned bc250h_table_bytes(void);
unsigned bc250h_table_pointers_set(void);
int      bc250h_table_reserved_clean(void);         /* 1 when every "must be zero" member is NULL */
unsigned bc250h_drivercaps_size(void);
unsigned bc250h_segment_descriptor_stride(void);

/* The case table. */
unsigned                        bc250h_case_count(void);
const struct bc250h_case*       bc250h_case(unsigned index);

/* Prepare an output buffer for a case that needs in-out members set (BC250H_PREP_*). stride is the
 * case's stride field: 0 means sizeof(DXGK_SEGMENTDESCRIPTOR4). */
void bc250h_prepare(int prep, void* out, unsigned out_size, void* descriptors, unsigned nb_segment,
                    unsigned stride);

/* Call DxgkDdiQueryAdapterInfo through the pointer the table carries.
 * Returns 0 for a normal return, 1 if the call raised a structured exception (an access violation on a
 * NULL buffer is the one this harness expects to be able to provoke), in which case *status is the
 * exception code. */
int bc250h_call(unsigned type, void* in, unsigned in_size, void* out, unsigned out_size, long* status);

/* What the compiled driver actually put into a DXGK_DRIVERCAPS answer. The static checker reads the
 * same values out of the source; printing both is how a parsing mistake in check.py shows up. */
struct bc250h_caps_observed {
    unsigned            wddm_version;
    unsigned            scheduling_caps;
    unsigned            memory_management_caps;
    unsigned            flip_caps;
    unsigned            presentation_caps;
    unsigned            support_non_vga;
    unsigned            support_smooth_rotation;
    unsigned            support_per_engine_tdr;
    unsigned            support_direct_flip;
    unsigned            support_surprise_removal;
    unsigned            nb_asymetric_processing_nodes;
    unsigned            max_queued_flip_on_vsync;
    unsigned            max_allocation_list_slot_id;
    unsigned            number_of_swizzling_ranges;
    unsigned            interrupt_message_number;
    unsigned            graphics_preemption_granularity;
    unsigned            compute_preemption_granularity;
    unsigned long long  highest_acceptable_address;
    unsigned long long  internal_gpu_va_start;
    unsigned long long  internal_gpu_va_end;
};
void bc250h_caps_observed(const void* caps_buffer, struct bc250h_caps_observed* out);

/* The same for the one segment descriptor QUERYSEGMENT4 fills. */
struct bc250h_segment_observed {
    unsigned            nb_segment;
    unsigned            paging_buffer_segment_id;
    unsigned            paging_buffer_size;
    unsigned            paging_buffer_private_data_size;
    unsigned            flags;
    unsigned long long  base_address;
    unsigned long long  cpu_translated_address;
    unsigned long long  size;
};
void bc250h_segment_observed(const void* out_buffer, const void* descriptor,
                             struct bc250h_segment_observed* out);

/* How many segments the driver declares, asked the way dxgkrnl asks: one pass-1 call. Stage B made
 * this 2, so nothing in the harness may assume 1. */
unsigned bc250h_segment_count(void);

/* The flags of segment `id` (1-based) as the driver filled it in the last pass-2 call the harness
 * made, or 0 if that segment was not described. Used to model VIDMM_GLOBAL::VerifySegmentSet: a
 * segment set is legal when it is 0, or when every segment it names carries Flags.Aperture. */
unsigned bc250h_segment_flags(const void* descriptors, unsigned stride, unsigned count, unsigned id);
unsigned bc250h_segment_set_bad_id(const void* descriptors, unsigned stride, unsigned count, unsigned set, int* not_declared);

/* ---- DxgkDdiCreateContext ------------------------------------------------------------------------
 *
 * The second matrix. E16 run 004 got through every QueryAdapterInfo type and died one call after
 * CreateContext, so the answer this DDI gives is worth the same treatment: every input combination
 * dxgkrnl can present, the full DXGK_CONTEXTINFO that comes back, and the consistency rules that
 * answer has to keep with the DDI table.
 */

/* Which handle to pass as hDevice. */
#define BC250H_DEV_VALID    0   /* one made by DxgkDdiCreateDevice */
#define BC250H_DEV_NULL     1
#define BC250H_DEV_WRONG    2   /* a live handle of the wrong object type (a process) */

struct bc250h_ctx_case {
    const char* name;
    unsigned    flags;              /* DXGK_CREATECONTEXTFLAGS.Value as dxgkrnl would set it */
    unsigned    node;               /* NodeOrdinal */
    unsigned    engine_affinity;
    int         device;             /* BC250H_DEV_* */
    long        expect;
    int         answers;            /* 1 when a DXGK_CONTEXTINFO is expected back */
};

/* Every member of the DXGK_CONTEXTINFO the driver returned. */
struct bc250h_ctx_observed {
    unsigned dma_buffer_size;
    unsigned dma_buffer_segment_set;
    unsigned dma_buffer_private_data_size;
    unsigned allocation_list_size;
    unsigned patch_location_list_size;
    unsigned caps;
    unsigned paging_companion_node_id;
    int      context_handle_set;    /* hContext came back non-NULL */
};

unsigned                            bc250h_ctx_case_count(void);
const struct bc250h_ctx_case*       bc250h_ctx_case(unsigned index);

/* Layout of DXGKARG_CREATECONTEXT, so qai_test.c can guard it and find the parts the driver may
 * write without including the kernel headers. */
unsigned bc250h_ctx_arg_size(void);
unsigned bc250h_ctx_info_offset(void);
unsigned bc250h_ctx_info_size(void);
unsigned bc250h_ctx_handle_offset(void);
unsigned bc250h_ctx_handle_size(void);

/* The DDI table's answer to the two consistency rules that need it. */
int bc250h_ddi_patch_present(void);
int bc250h_ddi_render_present(void);

/* Create the device handle the valid cases use (and a process handle for the wrong-type case).
 * Call after bc250h_start(). Returns 0 on success. */
int  bc250h_ctx_begin(void);
void bc250h_ctx_end(void);

/* Run one case. arg is a caller-owned buffer of bc250h_ctx_arg_size() bytes, pre-filled by the
 * caller with whatever pattern it wants; the bridge writes the input members into it, copies the
 * whole thing to snapshot, and then calls the DDI. Comparing arg against snapshot afterwards says
 * exactly which bytes the driver wrote, with no need to enumerate the input members here.
 * Returns 0 for a normal return, 1 if the call raised a structured exception. */
int bc250h_ctx_call(const struct bc250h_ctx_case* test_case, void* arg, void* snapshot,
                    struct bc250h_ctx_observed* out, long* status);

/* Destroy the context the last successful call created, so the matrix does not leak objects. */
void bc250h_ctx_destroy_last(void);

/* ---- stage B and stage C: the shims underneath wddm.c ---------------------------------------------
 *
 * 0.7.13 gave wddm.c two neighbours it calls into: vidmm.c (page tables) and gfx.c (the gfx ring).
 * Neither is compiled into this harness - they would drag in the register map and the shim types, and
 * what is under test is wddm.c's side of the contract, not theirs. They are replaced by shims whose
 * answers the tests choose, which is the only way to reach the paths that matter: a ring that refuses,
 * a ring that is not ready, a fence that never arrives.
 *
 * ADR 0008 point 5: DxgkDdiSubmitCommandVirtual may not fail. Whatever the ring answers, the packet is
 * completed - in hardware if it was taken, in software otherwise - and the DDI returns STATUS_SUCCESS.
 * That is what the submit matrix below exists to prove.
 */

struct bc250h_gfx_control {
    int                 submit_ready;       /* what GfxSubmitReady answers */
    long                submit_ib_status;   /* what GfxSubmitIb returns (0 = STATUS_SUCCESS) */
    int                 fence_arrived;      /* what GfxFenceArrived answers */
    int                 root_physical_ok;   /* what VidMmRootPhysical answers */
    unsigned long long  root_physical;      /* and the address it writes back */
    int                 translate_ok;       /* what VidMmTranslate answers */
    int                 translate_system;   /* and whether it calls the page system memory */
    unsigned long long  translate_physical; /* the base it translates to; the VA is added to it */
};

struct bc250h_shim_counters {
    unsigned            submit_ib;
    unsigned            submit_ready;
    unsigned            fence_arrived;
    unsigned            submit_fail;
    unsigned            vidmm_start;
    unsigned            vidmm_stop;
    unsigned            vidmm_update_page_table;
    unsigned            vidmm_set_root_page_table;
    unsigned            vidmm_root_physical;
    unsigned            vidmm_translate;
    unsigned            vidmm_summary;
    /* the arguments of the last GfxSubmitIb, so a test can check what wddm.c passed down */
    unsigned long       last_vmid;
    unsigned long       last_size;
    unsigned long long  last_root;
    unsigned long long  last_gpu_address;
};

void bc250h_shim_defaults(struct bc250h_gfx_control* out);   /* ring up, ready, fence arrives at once */
void bc250h_shim_set(const struct bc250h_gfx_control* control);
void bc250h_shim_counters(struct bc250h_shim_counters* out);
void bc250h_shim_reset_counters(void);

/* ---- DxgkDdiSubmitCommandVirtual ------------------------------------------------------------------ */

#define BC250H_CTXARG_ROOT      0   /* a real context whose RootPhysical was set */
#define BC250H_CTXARG_NOROOT    1   /* a real context that never saw SetRootPageTable */
#define BC250H_CTXARG_NULL      2   /* hContext NULL, as a paging submission arrives */

struct bc250h_submit_case {
    const char*         name;
    unsigned            dma_size;
    unsigned long long  dma_virtual_address;
    int                 context;            /* BC250H_CTXARG_* */
    unsigned            node;
    struct bc250h_gfx_control control;
    int                 expect_ring;        /* 1 when GfxSubmitIb is expected to be reached */
};

unsigned                            bc250h_submit_case_count(void);
const struct bc250h_submit_case*    bc250h_submit_case(unsigned index);

/* Runs one case against the real DxgkDdiSubmitCommandVirtual. Requires bc250h_ctx_begin(); it creates
 * the context itself and destroys it again. reached_ring reports whether GfxSubmitIb was entered.
 * Returns 0 for a normal return, 1 if the call raised a structured exception. */
int bc250h_submit_call(const struct bc250h_submit_case* test_case, long* status, int* reached_ring);

/* ---- DxgkDdiBuildPagingBuffer and DxgkDdiSetRootPageTable ------------------------------------------ */

/* Calls BuildPagingBuffer with the given DXGK_PAGINGBUFFER operation over a real DMA buffer, and
 * reports whether VidMmUpdatePageTable was called and whether the DMA buffer was written. */
int bc250h_paging_call(unsigned operation, void* dma_buffer, unsigned dma_size, unsigned multipass_offset,
                       long* status, int* vidmm_called);

/* Calls SetRootPageTable for the context bc250h_ctx_begin() made, and reports the RootPhysical the
 * driver recorded on it. Returns 0 for a normal return, 1 on a structured exception. */
int bc250h_set_root_call(unsigned segment_id, unsigned long long segment_offset, unsigned entries,
                         unsigned long long* recorded_root);

/* GetRootPageTableSize: in-out NumberOfPte, and the byte size returned. */
int bc250h_root_size_call(unsigned requested_ptes, unsigned* answered_ptes, unsigned long long* bytes);

/* Log lines wddm.c wrote through GuardLog during the last call, for the report. */
unsigned    bc250h_log_count(void);
const char* bc250h_log_line(unsigned index);
void        bc250h_log_reset(void);

/* Controls of the stub kernel in host_stubs.c. */
struct bc250h_stub_counters {
    unsigned            allocations;
    unsigned            frees;
    unsigned long long  bytes;
    unsigned            timer_set;
    unsigned            timer_cancel;
    unsigned            dpc_queued;
    unsigned            dpc_removed;
    unsigned            dpc_flushed;
    unsigned            delays;         /* KeDelayExecutionThread, the stop-path wait */
    unsigned            io_maps;        /* MmMapIoSpaceEx, the present blit's window on the framebuffer */
    unsigned            io_unmaps;
};
void bc250h_stub_set_log_echo(int on);
void bc250h_stub_set_irql(unsigned char irql);
void bc250h_stub_counters(struct bc250h_stub_counters* out);

#endif /* BC250_QAI_BRIDGE_H */
