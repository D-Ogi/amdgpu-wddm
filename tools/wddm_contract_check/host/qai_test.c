/* Host buffer tests for DxgkDdiQueryAdapterInfo of driver\kmd\wddm.c.
 *
 * Runs the real handler (see qai_bridge.c for how it is reached) against every query type the driver
 * handles and every type it refuses, and for each of them against a buffer that is NULL, zero-sized,
 * one byte short, exactly right, and larger than needed. Every buffer is surrounded by guard bytes and
 * pre-filled; every case is run twice with two different fill patterns, and the two answers are
 * compared byte for byte. A byte that differs between the runs is a byte the driver did not write.
 *
 * What a failure here means, in order of severity:
 *   - a fault: the handler dereferenced something it did not check.
 *   - a touched guard byte: the handler wrote outside the buffer dxgkrnl gave it.
 *   - a byte that differs between the two runs: the handler handed dxgkrnl a byte of whatever was on
 *     the caller's stack. dxgkrnl copies the whole answer; a stale byte becomes a cap.
 *   - a wrong status.
 *
 * Nothing here needs hardware, a driver load or the lab machine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qai_bridge.h"

#define GUARD_BYTES     64u
#define GUARD_LEAD      0xA5u
#define GUARD_TRAIL     0x5Au
#define FILL_A          0xCCu
#define FILL_B          0x55u
#define EXTRA_BYTES     256u            /* "larger than needed" */
#define MAX_PAYLOAD     4096u

static int g_Failures;
static int g_Checks;
static int g_Verbose;

static void report(int ok, const char* what, const char* detail)
{
    g_Checks++;
    if (!ok) g_Failures++;
    if (!ok || g_Verbose)
        printf("  %-4s %-54s %s\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
}

/* A buffer with guard bytes on both sides. payload() is what the driver is given. */
struct guarded {
    unsigned char bytes[GUARD_BYTES + MAX_PAYLOAD + GUARD_BYTES];
    unsigned      size;
};

static unsigned char* payload(struct guarded* g) { return g->bytes + GUARD_BYTES; }

static void guarded_fill(struct guarded* g, unsigned size, unsigned char pattern)
{
    memset(g->bytes, GUARD_LEAD, GUARD_BYTES);
    memset(g->bytes + GUARD_BYTES, pattern, MAX_PAYLOAD);
    memset(g->bytes + GUARD_BYTES + MAX_PAYLOAD, GUARD_TRAIL, GUARD_BYTES);
    g->size = size;
}

/* The guards proper, plus everything past the declared buffer up to the trailing guard: the driver was
 * told OutputDataSize and may write inside it, never one byte further. */
static int guards_intact(const struct guarded* g, unsigned size, unsigned char pattern, char* why, size_t why_size)
{
    unsigned i;

    for (i = 0; i < GUARD_BYTES; i++)
        if (g->bytes[i] != GUARD_LEAD)
        {
            snprintf(why, why_size, "lead guard byte %u overwritten (0x%02X)", i, g->bytes[i]);
            return 0;
        }
    for (i = 0; i < GUARD_BYTES; i++)
        if (g->bytes[GUARD_BYTES + MAX_PAYLOAD + i] != GUARD_TRAIL)
        {
            snprintf(why, why_size, "trailing guard byte %u overwritten (0x%02X)", i, g->bytes[GUARD_BYTES + MAX_PAYLOAD + i]);
            return 0;
        }
    for (i = size; i < MAX_PAYLOAD; i++)
        if (g->bytes[GUARD_BYTES + i] != pattern)
        {
            snprintf(why, why_size, "wrote %u bytes past OutputDataSize = %u", i - size + 1, size);
            return 0;
        }
    return 1;
}

static int region_ignored(const struct bc250h_case* c, unsigned offset)
{
    unsigned i;

    for (i = 0; i < c->n_ignore; i++)
        if (offset >= c->ignore[i].off && offset < c->ignore[i].off + c->ignore[i].len) return 1;
    return 0;
}

/* Nothing in the first `size` bytes moved since the snapshot taken right after the in-out members were
 * prepared. Against the snapshot rather than against the fill pattern, because a case with a prep has
 * members the harness itself wrote: comparing those to the pattern would report the harness's own
 * writes as the driver's. */
static int unchanged_since(const struct guarded* g, const unsigned char* snapshot, unsigned size,
                           char* why, size_t why_size)
{
    unsigned i;

    for (i = 0; i < size; i++)
        if (g->bytes[GUARD_BYTES + i] != snapshot[i])
        {
            snprintf(why, why_size, "byte %u of the buffer was written (0x%02X, was 0x%02X) although the call failed",
                     i, g->bytes[GUARD_BYTES + i], snapshot[i]);
            return 0;
        }
    return 1;
}

static const char* status_name(long status)
{
    switch ((unsigned long)status)
    {
    case 0x00000000ul: return "STATUS_SUCCESS";
    case 0xC00000BBul: return "STATUS_NOT_SUPPORTED";
    case 0xC000000Dul: return "STATUS_INVALID_PARAMETER";
    case 0xC0000023ul: return "STATUS_BUFFER_TOO_SMALL";
    case 0xC0000005ul: return "EXCEPTION_ACCESS_VIOLATION";
    default: return "";
    }
}

static void status_text(long status, char* text, size_t size)
{
    const char* name = status_name(status);

    if (name[0] != '\0') snprintf(text, size, "%s", name);
    else snprintf(text, size, "0x%08lX", (unsigned long)status);
}

/* How many segments the driver declares. Asked once per configuration, the way dxgkrnl asks: pass 1.
 * Stage B made this 2 (a local segment and an aperture segment), so nothing here may assume 1, and the
 * descriptor array has to be sized from the answer rather than from a constant. */
static unsigned g_SegmentCount;

/* The descriptors as the last dump_segment() call read them back, so that the CreateContext matrix can
 * test a segment set against the flags the driver really reported. */
static unsigned char g_Descriptors[16 * 256];

static unsigned segment_count(void)
{
    return g_SegmentCount;
}

/* The bytes of the descriptor array a pass-2 case hands over: the count at the stride the case chose.
 * A case with a stride smaller than the structure is refused before anything is written, but the
 * array is still allocated at the real stride so that a driver which ignored the refusal would be
 * caught overrunning it rather than corrupting the harness. */
static unsigned descriptor_bytes(const struct bc250h_case* c)
{
    unsigned stride = bc250h_segment_descriptor_stride();
    unsigned count = segment_count();

    if (c->stride > stride) stride = c->stride;
    if (count == 0) count = 1;
    return stride * count;
}

/* snapshot receives the output buffer exactly as the driver is about to receive it, after the in-out
 * members have been prepared. It is what "the driver wrote nothing" is measured against. */
static int call_case(const struct bc250h_case* c, struct guarded* out, struct guarded* desc,
                     unsigned out_size, unsigned char pattern, unsigned level_index,
                     unsigned char* snapshot, long* status)
{
    unsigned char in[16];

    guarded_fill(out, out_size, pattern);
    guarded_fill(desc, descriptor_bytes(c), pattern);
    memset(in, 0, sizeof(in));
    if (c->in_size != 0)
    {
        /* DXGK_QUERYPAGETABLELEVELDESCIN: WORD LevelIndex, WORD PhysicalAdapterIndex. */
        in[0] = (unsigned char)(level_index & 0xFF);
        in[1] = (unsigned char)((level_index >> 8) & 0xFF);
    }
    bc250h_prepare(c->prep, payload(out), out_size, payload(desc), segment_count(), c->stride);
    if (snapshot != NULL) memcpy(snapshot, payload(out), MAX_PAYLOAD);
    bc250h_log_reset();
    return bc250h_call(c->type, c->in_size ? in : NULL, c->in_size, payload(out), out_size, status);
}

static void check_status(const char* what, long got, long want, int faulted)
{
    char detail[160];
    char g[64], w[64];

    status_text(got, g, sizeof(g));
    status_text(want, w, sizeof(w));
    if (faulted)
    {
        snprintf(detail, sizeof(detail), "the handler faulted (%s); expected %s", g, w);
        report(0, what, detail);
        return;
    }
    snprintf(detail, sizeof(detail), "got %s, expected %s", g, w);
    report(got == want, what, detail);
}

static void run_case(const struct bc250h_case* c, unsigned level_index)
{
    static struct guarded out_a, out_b, desc_a, desc_b;
    static unsigned char snap_a[MAX_PAYLOAD], snap_b[MAX_PAYLOAD];
    char what[160];
    char detail[192];
    long status = 0, status_b = 0;
    int faulted;
    unsigned i, differing = 0, first_diff = 0;

    (void)snap_b;   /* the second run's snapshot is kept for symmetry with the first */

    printf("%s\n", c->name);

    /* 1 + 2. Exact size, twice, with two fill patterns. */
    faulted = call_case(c, &out_a, &desc_a, c->out_size, (unsigned char)FILL_A, level_index, snap_a, &status);
    snprintf(what, sizeof(what), "exact buffer (%u bytes), status", c->out_size);
    check_status(what, status, c->expect, faulted);
    if (!guards_intact(&out_a, c->out_size, (unsigned char)FILL_A, detail, sizeof(detail)))
        report(0, "exact buffer, guard bytes", detail);
    else report(1, "exact buffer, guard bytes", "untouched");

    faulted = call_case(c, &out_b, &desc_b, c->out_size, (unsigned char)FILL_B, level_index, snap_b, &status_b);
    check_status("second run with a different fill, status", status_b, c->expect, faulted);

    if (c->expect == BC250H_STATUS_SUCCESS)
    {
        for (i = 0; i < c->out_size; i++)
        {
            if (region_ignored(c, i)) continue;
            if (payload(&out_a)[i] != payload(&out_b)[i])
            {
                if (differing == 0) first_diff = i;
                differing++;
            }
        }
        if (differing != 0)
            snprintf(detail, sizeof(detail),
                     "%u byte(s) differ between the two runs, first at offset %u (0x%02X vs 0x%02X): "
                     "not written by the driver", differing, first_diff,
                     payload(&out_a)[first_diff], payload(&out_b)[first_diff]);
        else
            snprintf(detail, sizeof(detail), "all %u bytes deterministic%s", c->out_size,
                     c->n_ignore ? " outside the declared in/out members" : "");
        report(differing == 0, "every returned byte is initialized", detail);

        if (c->prep == BC250H_PREP_SEGMENT_FILL)
        {
            /* The stride dxgkrnl chose, not sizeof: the driver walks the array with the stride it was
             * given, so a check that walks it with sizeof reads the gap between descriptors and calls
             * it uninitialized. */
            unsigned stride = c->stride ? c->stride : bc250h_segment_descriptor_stride();
            unsigned described;
            struct bc250h_segment_observed seg;

            /* With no segment declared the descriptor array is not the driver's to touch, so there is
             * nothing to be deterministic about: NbSegment = 0 is the whole answer. */
            bc250h_segment_observed(payload(&out_a), payload(&desc_a), &seg);
            if (seg.nb_segment == 0)
            {
                report(1, "segment descriptor left alone", "NbSegment = 0, no descriptor to fill");
                goto no_descriptor;
            }

            /* Every declared segment, not just the first: stage B added an aperture segment behind the
             * local one, and a second descriptor left half-written would be invisible here otherwise.
             * Step by the stride but compare only the structure: when dxgkrnl picks a stride larger
             * than DXGK_SEGMENTDESCRIPTOR4 the bytes past the structure are its own padding, and a
             * driver that wrote into them would be the one at fault. */
            described = bc250h_segment_descriptor_stride();
            if (described > stride) described = stride;
            differing = 0;
            for (i = 0; i < seg.nb_segment * stride; i++)
            {
                if (i % stride >= described) continue;
                if (payload(&desc_a)[i] != payload(&desc_b)[i]) { if (!differing) first_diff = i; differing++; }
            }
            if (differing)
                snprintf(detail, sizeof(detail),
                         "%u segment(s) x %u of %u bytes, %u byte(s) nondeterministic, first at offset %u "
                         "(segment %u offset %u: 0x%02X vs 0x%02X)", seg.nb_segment, described, stride,
                         differing, first_diff, first_diff / stride + 1, first_diff % stride,
                         payload(&desc_a)[first_diff], payload(&desc_b)[first_diff]);
            else
                snprintf(detail, sizeof(detail), "%u segment(s) x %u of %u bytes, all deterministic",
                         seg.nb_segment, described, stride);
            report(differing == 0, "every segment descriptor fully initialized", detail);

            for (i = 1; i <= seg.nb_segment; i++)
            {
                unsigned flags = bc250h_segment_flags(payload(&desc_a), stride, seg.nb_segment, i);

                snprintf(detail, sizeof(detail), "segment %u flags 0x%08X%s", i, flags,
                         (flags & 1u) ? " (Aperture)" : "");
                report(1, "segment described", detail);
            }

            if (!guards_intact(&desc_a, descriptor_bytes(c), (unsigned char)FILL_A, detail, sizeof(detail)))
                report(0, "segment descriptors, guard bytes", detail);
            else report(1, "segment descriptors, guard bytes", "untouched");
        }
no_descriptor:
        ;
    }
    else
    {
        if (!unchanged_since(&out_a, snap_a, c->out_size, detail, sizeof(detail)))
            report(0, "a refused call writes nothing", detail);
        else report(1, "a refused call writes nothing", "buffer untouched");
    }

    /* 3. One byte short. */
    if (c->out_size > 0)
    {
        faulted = call_case(c, &out_a, &desc_a, c->out_size - 1, (unsigned char)FILL_A, level_index, snap_a, &status);
        snprintf(what, sizeof(what), "one byte short (%u bytes), status", c->out_size - 1);
        check_status(what, status, c->expect_short, faulted);
        if (!unchanged_since(&out_a, snap_a, c->out_size - 1, detail, sizeof(detail)))
            report(0, "one byte short writes nothing", detail);
        else report(1, "one byte short writes nothing", "buffer untouched");
    }

    /* 4. Zero size. */
    faulted = call_case(c, &out_a, &desc_a, 0, (unsigned char)FILL_A, level_index, snap_a, &status);
    check_status("zero-size buffer, status", status, c->expect_zero, faulted);
    if (!unchanged_since(&out_a, snap_a, MAX_PAYLOAD, detail, sizeof(detail)))
        report(0, "zero-size buffer writes nothing", detail);
    else report(1, "zero-size buffer writes nothing", "buffer untouched");

    /* 5. NULL pointer at the full size. Any failure status is acceptable; a fault is not. The input
     *    structure is passed as NULL too, which is the same question for the one type that reads one. */
    {
        char msg[192];

        bc250h_log_reset();
        faulted = bc250h_call(c->type, NULL, 0, NULL, c->out_size, &status);
        status_text(status, detail, sizeof(detail));
        snprintf(what, sizeof(what), "NULL pOutputData with OutputDataSize = %u", c->out_size);
        if (faulted)
        {
            snprintf(msg, sizeof(msg), "the handler dereferenced the NULL buffer (%s)", detail);
            report(0, what, msg);
        }
        else
        {
            snprintf(msg, sizeof(msg), "refused with %s", detail);
            report(status != BC250H_STATUS_SUCCESS, what, msg);
        }
    }

    /* 6. Larger than needed. dxgkrnl may declare a bigger DXGK_DRIVERCAPS than the driver compiled
     *    against; the driver must fill what it knows and never write past OutputDataSize. */
    faulted = call_case(c, &out_a, &desc_a, c->out_size + EXTRA_BYTES, (unsigned char)FILL_A, level_index, NULL, &status);
    snprintf(what, sizeof(what), "larger buffer (%u bytes), status", c->out_size + EXTRA_BYTES);
    check_status(what, status, c->expect, faulted);
    if (!guards_intact(&out_a, c->out_size + EXTRA_BYTES, (unsigned char)FILL_A, detail, sizeof(detail)))
        report(0, "larger buffer, guard bytes", detail);
    else report(1, "larger buffer, guard bytes", "untouched");
    if (c->expect == BC250H_STATUS_SUCCESS)
    {
        unsigned zeroed = 0;

        for (i = c->out_size; i < c->out_size + EXTRA_BYTES; i++)
            if (payload(&out_a)[i] == 0) zeroed++;
        snprintf(detail, sizeof(detail), "%u of %u tail bytes zeroed by the driver (%s)",
                 zeroed, EXTRA_BYTES, c->writes_whole_buffer ? "expected: it zeroes OutputDataSize"
                                                             : "expected: it fills only the members it knows");
        report(1, "larger buffer, the unknown tail", detail);
    }
    printf("\n");
}

static void dump_caps(void)
{
    static struct guarded out, desc;
    struct bc250h_caps_observed caps;
    const struct bc250h_case* c = bc250h_case(0);
    long status = 0;

    if (c == NULL) return;
    (void)call_case(c, &out, &desc, c->out_size, (unsigned char)FILL_A, 0, NULL, &status);
    if (status != BC250H_STATUS_SUCCESS) return;
    bc250h_caps_observed(payload(&out), &caps);
    printf("DXGK_DRIVERCAPS as the compiled driver fills it (%u bytes):\n", bc250h_drivercaps_size());
    printf("  WDDMVersion                       0x%04X\n", caps.wddm_version);
    printf("  SchedulingCaps.Value              0x%08X\n", caps.scheduling_caps);
    printf("  MemoryManagementCaps.Value        0x%08X\n", caps.memory_management_caps);
    printf("  FlipCaps.Value                    0x%08X\n", caps.flip_caps);
    printf("  PresentationCaps.Value            0x%08X\n", caps.presentation_caps);
    printf("  SupportNonVGA                     %u\n", caps.support_non_vga);
    printf("  SupportSmoothRotation             %u\n", caps.support_smooth_rotation);
    printf("  SupportPerEngineTDR               %u\n", caps.support_per_engine_tdr);
    printf("  SupportDirectFlip                 %u\n", caps.support_direct_flip);
    printf("  SupportSurpriseRemoval            %u\n", caps.support_surprise_removal);
    printf("  NbAsymetricProcessingNodes        %u\n", caps.nb_asymetric_processing_nodes);
    printf("  MaxQueuedFlipOnVSync              %u\n", caps.max_queued_flip_on_vsync);
    printf("  MaxAllocationListSlotId           %u\n", caps.max_allocation_list_slot_id);
    printf("  NumberOfSwizzlingRanges           %u\n", caps.number_of_swizzling_ranges);
    printf("  InterruptMessageNumber            %u\n", caps.interrupt_message_number);
    printf("  GraphicsPreemptionGranularity     %u\n", caps.graphics_preemption_granularity);
    printf("  ComputePreemptionGranularity      %u\n", caps.compute_preemption_granularity);
    printf("  HighestAcceptableAddress          0x%016llX\n", caps.highest_acceptable_address);
    printf("  InternalGpuVirtualAddressRange    0x%llX .. 0x%llX\n",
           caps.internal_gpu_va_start, caps.internal_gpu_va_end);
    printf("\n");
}

static void dump_segment(const char* title)
{
    static struct guarded out, desc;
    struct bc250h_segment_observed seg;
    const struct bc250h_case* c = NULL;
    unsigned i;
    long status = 0;

    for (i = 0; i < bc250h_case_count(); i++)
    {
        c = bc250h_case(i);
        if (c->prep == BC250H_PREP_SEGMENT_FILL) break;
        c = NULL;
    }
    if (c == NULL) return;
    (void)call_case(c, &out, &desc, c->out_size, (unsigned char)FILL_A, 0, NULL, &status);
    if (status != BC250H_STATUS_SUCCESS) { printf("%s: QUERYSEGMENT4 returned 0x%08lX\n\n", title, (unsigned long)status); return; }
    bc250h_segment_observed(payload(&out), payload(&desc), &seg);
    /* Kept for the CreateContext matrix, which has to test a segment set against these flags. */
    memcpy(g_Descriptors, payload(&desc),
           (sizeof(g_Descriptors) < descriptor_bytes(c)) ? sizeof(g_Descriptors) : descriptor_bytes(c));
    printf("%s:\n", title);
    printf("  NbSegment                         %u\n", seg.nb_segment);
    printf("  PagingBufferSegmentId             %u\n", seg.paging_buffer_segment_id);
    printf("  PagingBufferSize                  %u\n", seg.paging_buffer_size);
    printf("  PagingBufferPrivateDataSize       %u\n", seg.paging_buffer_private_data_size);
    for (i = 1; i <= seg.nb_segment; i++)
    {
        unsigned flags = bc250h_segment_flags(payload(&desc), bc250h_segment_descriptor_stride(), seg.nb_segment, i);

        printf("  segment %u Flags.Value             0x%08X%s\n", i, flags, (flags & 1u) ? "  (Aperture)" : "");
    }
    /* Budget groups (DXGK_SEGMENTFLAGS: LocalBudgetGroup 0x00080000, NonLocalBudgetGroup 0x00100000). Every
     * declared segment is in exactly one group: the aperture in the non-local one, the memory segments in the
     * local one. With the non-local group empty, dxgkrnl reported a UMA-style local budget that included shared
     * system memory (trial 211, revision 183). */
    for (i = 1; i <= seg.nb_segment; i++)
    {
        unsigned flags = bc250h_segment_flags(payload(&desc), bc250h_segment_descriptor_stride(), seg.nb_segment, i);
        unsigned local = (flags & 0x00080000u) != 0, nonlocal = (flags & 0x00100000u) != 0, aperture = (flags & 1u) != 0;
        char what[96], detail[160];

        snprintf(what, sizeof(what), "segment %u is in the %s budget group only", i, aperture ? "non-local" : "local");
        snprintf(detail, sizeof(detail), "Flags.Value 0x%08X: Aperture %u, LocalBudgetGroup %u, NonLocalBudgetGroup %u",
                 flags, aperture, local, nonlocal);
        report(aperture ? (nonlocal && !local) : (local && !nonlocal), what, detail);
    }
    if (seg.nb_segment != 0)
    {
        printf("  segment 1 BaseAddress             0x%016llX\n", seg.base_address);
        printf("  segment 1 CpuTranslatedAddress    0x%016llX\n", seg.cpu_translated_address);
        printf("  segment 1 Size                    0x%llX\n", seg.size);
    }
    printf("\n");
}

/* ---- DxgkDdiCreateContext ------------------------------------------------------------------------
 *
 * The same treatment for the DDI run 004 died one call after. The input matrix is the flag
 * combinations, the node ordinal in and out of range, and a device handle that is valid, NULL or of
 * the wrong type; for each of them the whole DXGK_CONTEXTINFO is recorded and checked against the
 * consistency rules its documentation states.
 *
 * Three rules are checked here rather than in check.py, because they are about a particular answer to
 * a particular question and the static checker only sees the source of the answer:
 *   - GdiContext = 1 requires AllocationListSize = DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT (256).
 *   - Caps.NoPatchingRequired = 0 means dxgkrnl will patch, which needs DxgkDdiPatch in the table and
 *     non-zero list sizes to patch into.
 *   - DmaBufferSegmentSet must be 0 or name aperture segments only; we declare no aperture segment, so
 *     for this driver the only legal answer is 0 (dxgmms2 VIDMM_DMA_POOL::Init -> VerifySegmentSet,
 *     facts M66).
 */

#define GDI_ALLOCATION_LIST_SIZE    256u
#define CTX_FLAG_GDI                0x2u
#define CTX_CAPS_NOPATCHING         0x1u

static void ctx_dump(const struct bc250h_ctx_observed* o)
{
    printf("      dma %u bytes, segment set 0x%X, private %u, lists %u/%u, caps 0x%08X, companion node %u\n",
           o->dma_buffer_size, o->dma_buffer_segment_set, o->dma_buffer_private_data_size,
           o->allocation_list_size, o->patch_location_list_size, o->caps, o->paging_companion_node_id);
}

static void run_ctx_case(const struct bc250h_ctx_case* c)
{
    static struct guarded arg_a, arg_b;
    static unsigned char snap_a[MAX_PAYLOAD], snap_b[MAX_PAYLOAD];
    struct bc250h_ctx_observed obs_a, obs_b;
    char detail[240];
    unsigned size = bc250h_ctx_arg_size();
    unsigned info_off = bc250h_ctx_info_offset(), info_len = bc250h_ctx_info_size();
    unsigned handle_off = bc250h_ctx_handle_offset(), handle_len = bc250h_ctx_handle_size();
    long status = 0, status_b = 0;
    int faulted, faulted_b;
    unsigned i, differing = 0, first_diff = 0, outside = 0, first_outside = 0;

    printf("CreateContext: %s\n", c->name);
    if (size > MAX_PAYLOAD) { report(0, "argument fits the harness buffer", "DXGKARG_CREATECONTEXT too large"); return; }

    guarded_fill(&arg_a, size, (unsigned char)FILL_A);
    faulted = bc250h_ctx_call(c, payload(&arg_a), snap_a, &obs_a, &status);
    check_status("status", status, c->expect, faulted);

    if (!guards_intact(&arg_a, size, (unsigned char)FILL_A, detail, sizeof(detail)))
        report(0, "guard bytes", detail);
    else report(1, "guard bytes", "untouched");

    /* Exactly which bytes moved. Anything outside ContextInfo and hContext is a member the driver was
     * given as input and had no business writing. */
    for (i = 0; i < size; i++)
    {
        if (payload(&arg_a)[i] == snap_a[i]) continue;
        if (i >= info_off && i < info_off + info_len) continue;
        if (i >= handle_off && i < handle_off + handle_len) continue;
        if (outside == 0) first_outside = i;
        outside++;
    }
    snprintf(detail, sizeof(detail), outside ? "%u input byte(s) overwritten, first at offset %u"
                                             : "only hContext and ContextInfo were written",
             outside, first_outside);
    report(outside == 0, "the driver wrote only its out members", detail);

    if (!c->answers)
    {
        /* A refused call must leave the answer alone: dxgkrnl reads ContextInfo only on success, but a
         * half-written one is how a later success inherits a stale member. */
        for (i = info_off; i < info_off + info_len; i++)
            if (payload(&arg_a)[i] != snap_a[i]) { if (!differing) first_diff = i; differing++; }
        snprintf(detail, sizeof(detail), differing ? "%u byte(s) of ContextInfo written on a refusal, first at %u"
                                                   : "ContextInfo untouched",
                 differing, first_diff);
        report(differing == 0, "a refused call writes no ContextInfo", detail);
        report(!obs_a.context_handle_set, "no context handle returned",
               obs_a.context_handle_set ? "hContext was set although the call failed" : "hContext left NULL");
        printf("\n");
        return;
    }

    report(obs_a.context_handle_set, "a context handle came back",
           obs_a.context_handle_set ? "hContext non-NULL" : "hContext still NULL after a success");
    if (g_Verbose) ctx_dump(&obs_a);

    /* Second run, other fill, to catch a member left at whatever was in the structure. */
    guarded_fill(&arg_b, size, (unsigned char)FILL_B);
    faulted_b = bc250h_ctx_call(c, payload(&arg_b), snap_b, &obs_b, &status_b);
    check_status("second run with a different fill, status", status_b, c->expect, faulted_b);
    for (i = info_off; i < info_off + info_len; i++)
        if (payload(&arg_a)[i] != payload(&arg_b)[i]) { if (!differing) first_diff = i; differing++; }
    snprintf(detail, sizeof(detail), differing
             ? "%u of %u ContextInfo bytes differ between the runs, first at offset %u: not written by the driver"
             : "all %u ContextInfo bytes deterministic", differing ? differing : info_len,
             differing ? info_len : first_diff, first_diff);
    report(differing == 0, "every ContextInfo byte is initialized", detail);

    /* The consistency rules. */
    if (c->flags & CTX_FLAG_GDI)
    {
        snprintf(detail, sizeof(detail),
                 "GdiContext asked, AllocationListSize = %u, required %u (DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT)",
                 obs_a.allocation_list_size, GDI_ALLOCATION_LIST_SIZE);
        report(obs_a.allocation_list_size == GDI_ALLOCATION_LIST_SIZE,
               "GdiContext -> AllocationListSize = 256", detail);
    }

    if ((obs_a.caps & CTX_CAPS_NOPATCHING) == 0)
    {
        snprintf(detail, sizeof(detail),
                 "NoPatchingRequired = 0, so dxgkrnl will patch: DxgkDdiPatch %s, lists %u/%u",
                 bc250h_ddi_patch_present() ? "present" : "NULL",
                 obs_a.allocation_list_size, obs_a.patch_location_list_size);
        report(bc250h_ddi_patch_present() &&
               obs_a.allocation_list_size != 0 && obs_a.patch_location_list_size != 0,
               "NoPatchingRequired = 0 -> a patch path exists", detail);
    }
    else report(1, "NoPatchingRequired = 1 -> no patch path needed",
                "the context patches nothing, so DxgkDdiPatch and the lists may stay empty");

    /* VerifySegmentSet: 0, or aperture segments only. Stage B declares an aperture segment, so naming
     * it is now legal where in 0.7.5 only 0 was. The segments are read back from the driver's own
     * QUERYSEGMENT4 answer rather than assumed. */
    {
        unsigned count = segment_count();
        int not_declared = 0;
        unsigned bad = bc250h_segment_set_bad_id(g_Descriptors, bc250h_segment_descriptor_stride(), count, obs_a.dma_buffer_segment_set, &not_declared);

        if (bad == 0)
            snprintf(detail, sizeof(detail), "DmaBufferSegmentSet = 0x%X: %s",
                     obs_a.dma_buffer_segment_set,
                     obs_a.dma_buffer_segment_set ? "aperture segments only" : "system memory");
        else
            snprintf(detail, sizeof(detail), "DmaBufferSegmentSet = 0x%X names segment %u, which %s",
                     obs_a.dma_buffer_segment_set, bad,
                     not_declared ? "is not declared" : "has Flags.Aperture clear");
        report(bad == 0, "DmaBufferSegmentSet passes VerifySegmentSet", detail);
    }

    snprintf(detail, sizeof(detail), "PagingCompanionNodeId = %u, declared nodes = 1",
             obs_a.paging_companion_node_id);
    report(obs_a.paging_companion_node_id < 1u, "PagingCompanionNodeId names a declared node", detail);

    printf("\n");
}

static void run_ctx_matrix(void)
{
    unsigned i;
    int rc;

    printf("DxgkDdiCreateContext matrix\n");
    printf("---------------------------\n");
    printf("DXGKARG_CREATECONTEXT %u bytes, ContextInfo at +%u (%u bytes), hContext at +%u\n",
           bc250h_ctx_arg_size(), bc250h_ctx_info_offset(), bc250h_ctx_info_size(),
           bc250h_ctx_handle_offset());
    printf("table: DxgkDdiPatch %s, DxgkDdiRender %s\n\n",
           bc250h_ddi_patch_present() ? "present" : "NULL",
           bc250h_ddi_render_present() ? "present" : "NULL");

    rc = bc250h_ctx_begin();
    if (rc != 0) { report(0, "CreateDevice/CreateProcess for the matrix", "could not build the handles"); return; }

    for (i = 0; i < bc250h_ctx_case_count(); i++)
        run_ctx_case(bc250h_ctx_case(i));

    bc250h_ctx_end();
}

/* ---- stage C: DxgkDdiSubmitCommandVirtual ---------------------------------------------------------
 *
 * ADR 0008 point 5: this DDI may not fail. A failure return is bugcheck 0x119 with parameter 1 = 0x2,
 * so however the ring answers, the packet is completed - on the ring if it was taken, in software
 * otherwise - and the DDI returns STATUS_SUCCESS. The gfx.c shim is what makes the refusing ring, the
 * unready ring and the fence that never arrives reachable on a machine with no GPU.
 */
static void run_submit_matrix(void)
{
    struct bc250h_shim_counters before, after;
    char detail[240];
    unsigned i;

    printf("DxgkDdiSubmitCommandVirtual matrix (ADR 0008 point 5: may not fail)\n");
    printf("------------------------------------------------------------------\n\n");

    for (i = 0; i < bc250h_submit_case_count(); i++)
    {
        const struct bc250h_submit_case* c = bc250h_submit_case(i);
        long status = 0;
        int reached = 0, faulted;

        printf("SubmitCommandVirtual: %s\n", c->name);
        bc250h_shim_counters(&before);
        faulted = bc250h_submit_call(c, &status, &reached);
        bc250h_shim_counters(&after);

        check_status("returns STATUS_SUCCESS whatever the ring says", status, BC250H_STATUS_SUCCESS, faulted);

        snprintf(detail, sizeof(detail), "GfxSubmitIb %s, expected %s",
                 reached ? "entered" : "not entered", c->expect_ring ? "entered" : "not entered");
        report(reached == c->expect_ring, "the ring is used exactly when it should be", detail);

        if (reached)
        {
            snprintf(detail, sizeof(detail), "vmid %lu, root 0x%llX, va 0x%llX, %lu bytes",
                     after.last_vmid, after.last_root, after.last_gpu_address, after.last_size);
            report(after.last_size == c->dma_size && after.last_gpu_address == c->dma_virtual_address,
                   "the packet reached the ring unchanged", detail);
            snprintf(detail, sizeof(detail), "root 0x%llX", after.last_root);
            report(after.last_root != 0, "the context's page directory root came with it", detail);
        }
        else
        {
            /* Not reaching the ring is only correct if nothing was left half-done there. */
            snprintf(detail, sizeof(detail), "%u GfxSubmitIb call(s) during this case",
                     after.submit_ib - before.submit_ib);
            report(after.submit_ib == before.submit_ib, "nothing was written to the ring", detail);
        }
        printf("\n");
    }
}

/* ---- stage B: BuildPagingBuffer and the root page table ------------------------------------------- */

#define DXGK_OPERATION_UPDATE_PAGE_TABLE_VALUE  11u
#define DXGK_OPERATION_MAP_APERTURE_SEGMENT_VALUE 5u
#define DXGK_OPERATION_FILL_VALUE               1u
#define DXGK_OPERATION_TRANSFER_VALUE           2u

static void run_paging_matrix(void)
{
    static struct guarded dma;
    char detail[240];
    unsigned i;
    static const struct { unsigned op; const char* name; int expect_vidmm; } ops[] = {
        { DXGK_OPERATION_UPDATE_PAGE_TABLE_VALUE,    "UPDATE_PAGE_TABLE (11)",    1 },
        { DXGK_OPERATION_MAP_APERTURE_SEGMENT_VALUE, "MAP_APERTURE_SEGMENT (5)",  0 },
        { DXGK_OPERATION_FILL_VALUE,                 "FILL (1)",                  0 },
        { DXGK_OPERATION_TRANSFER_VALUE,             "TRANSFER (2)",              0 },
        { 31u,                                       "an operation we do not know (31)", 0 },
    };

    printf("DxgkDdiBuildPagingBuffer matrix (may not fail: 0x119 parameter 1 = 0x5)\n");
    printf("----------------------------------------------------------------------\n\n");

    for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
    {
        long status = 0;
        int vidmm = 0, faulted;

        printf("BuildPagingBuffer: %s\n", ops[i].name);
        guarded_fill(&dma, 4096, (unsigned char)FILL_A);
        faulted = bc250h_paging_call(ops[i].op, payload(&dma), 4096, 0, &status, &vidmm);

        check_status("returns STATUS_SUCCESS", status, BC250H_STATUS_SUCCESS, faulted);
        snprintf(detail, sizeof(detail), "VidMmUpdatePageTable %s, expected %s",
                 vidmm ? "called" : "not called", ops[i].expect_vidmm ? "called" : "not called");
        report(vidmm == ops[i].expect_vidmm, "the page tables are updated only for operation 11", detail);

        /* Stage B writes page tables with the CPU and leaves the paging buffer empty; "no byte was
         * written" is the legal answer to every operation, and it has to be literally true. */
        if (!guards_intact(&dma, 4096, (unsigned char)FILL_A, detail, sizeof(detail)))
            report(0, "the DMA buffer and its guards are untouched", detail);
        else
        {
            unsigned j, written = 0;

            for (j = 0; j < 4096; j++) if (payload(&dma)[j] != (unsigned char)FILL_A) written++;
            snprintf(detail, sizeof(detail), "%u of 4096 bytes written", written);
            report(written == 0, "the DMA buffer and its guards are untouched", detail);
        }
        printf("\n");
    }

    /* A zero-size buffer and a NULL one: VidMm passes both when it is only measuring. */
    {
        long status = 0;
        int vidmm = 0, faulted;

        printf("BuildPagingBuffer: UPDATE_PAGE_TABLE with a zero-size DMA buffer\n");
        faulted = bc250h_paging_call(DXGK_OPERATION_UPDATE_PAGE_TABLE_VALUE, NULL, 0, 0, &status, &vidmm);
        check_status("returns STATUS_SUCCESS", status, BC250H_STATUS_SUCCESS, faulted);
        report(vidmm == 1, "the page tables are still updated", "the update is done by the CPU, not by the buffer");
        printf("\n");
    }
}

static void run_root_matrix(void)
{
    struct bc250h_gfx_control control;
    unsigned long long root = 0;
    unsigned answered = 0;
    unsigned long long bytes = 0;
    char detail[240];
    int rc;

    printf("DxgkDdiGetRootPageTableSize / DxgkDdiSetRootPageTable\n");
    printf("-----------------------------------------------------\n\n");

    rc = bc250h_root_size_call(1, &answered, &bytes);
    snprintf(detail, sizeof(detail), "asked 1 entry, answered %u entries, %llu bytes", answered, bytes);
    report(rc == 0 && answered != 0 && bytes != 0, "GetRootPageTableSize answers a whole level", detail);
    report(bytes >= (unsigned long long)answered * 8ull,
           "the byte size covers the entries it claims", detail);

    rc = bc250h_root_size_call(0x10000, &answered, &bytes);
    snprintf(detail, sizeof(detail), "asked 65536 entries, answered %u entries, %llu bytes", answered, bytes);
    report(rc == 0 && answered != 0, "the answer does not follow an absurd request", detail);

    /* The root the driver records is read where it is used: as the root handed to the ring. */
    bc250h_shim_defaults(&control);
    control.root_physical = 0xABCDE000ull;
    bc250h_shim_set(&control);
    rc = bc250h_set_root_call(1, 0x3000, 512, &root);
    snprintf(detail, sizeof(detail), "VidMmRootPhysical answered 0x%llX, the ring was given 0x%llX",
             control.root_physical, root);
    report(rc == 0 && root == control.root_physical,
           "SetRootPageTable records the root the next submit uses", detail);

    /* VidMm cannot translate the address: the context must be left without a root rather than with a
     * wrong one, and the next submit must then stay off the ring. */
    bc250h_shim_defaults(&control);
    control.root_physical_ok = 0;
    control.root_physical = 0xDEADBEEFull;
    bc250h_shim_set(&control);
    rc = bc250h_set_root_call(1, 0x3000, 512, &root);
    snprintf(detail, sizeof(detail), "VidMmRootPhysical refused; the ring was given 0x%llX", root);
    report(rc == 0 && root == 0, "a root VidMm cannot translate is not used", detail);

    bc250h_shim_defaults(&control);
    bc250h_shim_set(&control);
    printf("\n");
}

int main(int argc, char** argv)
{
    unsigned i;
    int rc;
    unsigned fixture_gib = 8;
    struct bc250h_geometry geometry;
    struct bc250h_stub_counters counters;

    for (i = 1; i < (unsigned)argc; i++) {
        if (strcmp(argv[i], "-v") == 0) g_Verbose = 1;
        else if (strcmp(argv[i], "--vram-gib") == 0 && i + 1 < (unsigned)argc) {
            const char* size = argv[++i];
            if (strcmp(size, "8") == 0) fixture_gib = 8;
            else if (strcmp(size, "12") == 0) fixture_gib = 12;
            else if (strcmp(size, "16") == 0) fixture_gib = 16;
            else { fprintf(stderr, "--vram-gib requires 8, 12 or 16 (synthetic fixture)\n"); return 2; }
        } else { fprintf(stderr, "usage: qai_test [-v] [--vram-gib 8|12|16]\n"); return 2; }
    }
    if (!bc250h_fixture_geometry(fixture_gib, &geometry)) return 2;
    printf("Synthetic geometry: %u GiB, CPU base 0x%llX, MC base 0x%llX; no hardware discovery\n",
           fixture_gib, geometry.vram_physical, geometry.vram_mc_base);
    bc250h_stub_set_log_echo(g_Verbose);

    printf("wddm.c QueryAdapterInfo host buffer tests\n");
    printf("=========================================\n\n");

    rc = bc250h_start_geometry(1, &geometry);
    if (rc != 0) { printf("FATAL: bc250h_start failed (%d)\n", rc); return 3; }

    printf("DRIVER_INITIALIZATION_DATA: %u bytes, %u DDI pointers set, reserved members %s\n",
           bc250h_table_bytes(), bc250h_table_pointers_set(),
           bc250h_table_reserved_clean() ? "all NULL (correct)" : "NOT all NULL (defect)");
    report(bc250h_table_reserved_clean(), "reserved table members are NULL",
           bc250h_table_reserved_clean() ? "" : "a member Learn marks reserved carries a pointer");
    g_SegmentCount = bc250h_segment_count();
    printf("DXGK_SEGMENTDESCRIPTOR4 stride: %u bytes, %u segment(s) declared\n\n",
           bc250h_segment_descriptor_stride(), g_SegmentCount);

    dump_caps();
    dump_segment("QUERYSEGMENT4 with the VRAM carve-out identified");

    for (i = 0; i < bc250h_case_count(); i++)
    {
        const struct bc250h_case* c = bc250h_case(i);
        unsigned level = 0;

        if (c->type == 14)
        {
            if (strstr(c->name, "level 3") != NULL) level = 3;
            else if (strstr(c->name, "level 4") != NULL) level = 4;
        }
        run_case(c, level);
    }

    run_ctx_matrix();

    /* Stage B and stage C need the device handle the context matrix builds. */
    if (bc250h_ctx_begin() == 0)
    {
        run_paging_matrix();
        run_root_matrix();
        run_submit_matrix();
        bc250h_ctx_end();
    }
    else report(0, "CreateDevice for the stage B/C matrices", "could not build the handles");

    bc250h_stub_counters(&counters);
    printf("stub kernel: %u allocations (%llu bytes), %u frees, %u timer arms, %u DPCs queued\n\n",
           counters.allocations, counters.bytes, counters.frees, counters.timer_set, counters.dpc_queued);

    /* Stage B allocates page tables and stage C arms a submit timer, so what WddmStop leaves behind is
     * now worth asserting: a pool block outlives the driver, and a timer still armed when the device is
     * gone fires its DPC into freed memory. Counted after the stop, because that is the call that is
     * supposed to clean up. */
    bc250h_stop();
    {
        struct bc250h_stub_counters stopped;
        char detail[200];

        bc250h_stub_counters(&stopped);
        snprintf(detail, sizeof(detail), "%u allocation(s), %u free(s) after WddmStop",
                 stopped.allocations, stopped.frees);
        report(stopped.allocations == stopped.frees, "the pool is balanced once the device is stopped",
               detail);
        snprintf(detail, sizeof(detail), "%u arm(s), %u cancel(s) after WddmStop",
                 stopped.timer_set, stopped.timer_cancel);
        report(stopped.timer_cancel >= stopped.timer_set, "no timer is left armed behind WddmStop",
               detail);
    }

    /* The second configuration: the EnableVram gate closed, so wddm.c has no segment to declare. */
    rc = bc250h_start_geometry(0, &geometry);
    if (rc != 0) { printf("FATAL: bc250h_start(0) failed (%d)\n", rc); return 3; }
    g_SegmentCount = bc250h_segment_count();
    dump_segment("QUERYSEGMENT4 with EnableVram closed (no carve-out known)");
    for (i = 0; i < bc250h_case_count(); i++)
    {
        const struct bc250h_case* c = bc250h_case(i);

        /* Only the two plain segment passes. The stride and NULL-descriptor cases expect statuses
         * the driver reaches only when it has a segment to describe; with none declared it returns
         * before the stride is ever looked at, which is correct and not what those cases are for. */
        if (c->stride == 0 &&
            (c->prep == BC250H_PREP_SEGMENT_COUNT || c->prep == BC250H_PREP_SEGMENT_FILL))
            run_case(c, 0);
    }
    bc250h_stop();

    printf("=========================================\n");
    printf("%d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures != 0 ? 1 : 0;
}
