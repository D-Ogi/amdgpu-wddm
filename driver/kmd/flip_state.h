/* flip_state.h - BD-170 r7: the decisions of the flip state machine, without kernel calls.
 *
 * The design is docs/design/vidpn-flip.md section 7.2. Three paths decide with these functions, each under the
 * interrupt lock L of this driver's one MSI message (facts M38):
 *   - the flip DDI's transaction (wddm.c, WddmFlipLocked), which runs at DIRQL inside dxgmms2's
 *     DpSynchronizeExecution (facts M904) or inside one DxgkCbSynchronizeExecution section of the low entry;
 *   - a writer's Begin and End (wddm.c, WddmPrimaryExclusiveBegin and WddmPrimaryExclusiveEnd), each one section;
 *   - the fault path E5 (wddm.c, WddmFlipFault) and its DPC (WddmFlipFaultDpc), which runs outside L.
 * wddm.c reads the facts (hardware readbacks inside L, software state that only L-held code changes) and acts on the
 * answer. Nothing here reads a register or keeps state, so test/flip_state_test.c drives every edge, and
 * test/run_flip_state.ps1 breaks each rule in a copy of this file and requires the test to fail.
 *
 * The edges of a flip request, in this order (section 7.2 names them):
 *   F0  FAULTING: E5 was entered. Nothing is programmed; the DDI answers STATUS_DEVICE_HARDWARE_ERROR.
 *   F1  OVERLAP: the generation is odd and no writer is active, or a writer is active with an even generation. L did
 *       not serialize this call, which facts M904 says cannot happen. E5.
 *   A   REFUSED: the admission (scanout_admit.h, the pitch and format rules) refused the request. Nothing is
 *       published. This is a UMD or admission defect, not a pipe state, and not E5.
 *   W   a writer is active (between Begin and End). The historical W-v/W-i/W-u labels classify the request.
 *       With the hardware flip gate open all three return STATUS_DEVICE_BUSY before publishing anything.
 *       Reserved storage protects the old primary, but does not implement the requested flip's timing.
 *       With the gate closed only the existing software-reporting deposit route remains.
 *   E   PROGRAM: E2 (next VSync) or E3 (immediate) program the plane now.
 * After E: a refusal before the write sequence ran is A; a write sequence that ran and failed is E5 (an MMIO failure
 * is unknown, never proof that nothing was written); an immediate request without P' in the call is U.
 *
 * U, which edge E alone produces, is E5 (section 7.2): a BUSY answer stops the source until a restart (facts M902), a
 * longer poll at DIRQL is not an answer, and P' held in the call for every immediate flip measured, the vertical blank
 * included (facts M903), so U is a fault and not a state the OS meets in normal operation.
 */
#ifndef BC250_FLIP_STATE_H
#define BC250_FLIP_STATE_H

/* DXGK_FAULT_ERROR_CODE.DeviceSpecificCode of the DMA_PAGE_FAULTED notification (31 bits). */
#define BC250_FLIP_FAULT_PROGRAM 0x170001u          /* the write sequence ran and failed */
#define BC250_FLIP_FAULT_UNPROVEN 0x170002u         /* U: an immediate request whose completion the call cannot show */
#define BC250_FLIP_FAULT_OWED_STOPPED 0x170003u     /* a writer's End owes a flip and the OTG is not running */
#define BC250_FLIP_FAULT_OVERLAP 0x170004u          /* F1 */
#define BC250_FLIP_FAULT_SECTION 0x170006u          /* protected section could not be acquired */
#define BC250_FLIP_FAULT_BRIDGE 0x170007u           /* reserved bridge programming or retirement proof failed */
/* The code of a W-u record (wddm.c's summary), from the same series so that a log search finds both. Never an E5
 * code: W-u does not enter E5. */
#define BC250_FLIP_RECORD_WRITER_UNPROVEN 0x170005u

typedef enum _BC250_FLIP_EDGE {
    BC250_FLIP_EDGE_FAULTING = 0,       /* F0 */
    BC250_FLIP_EDGE_OVERLAP,            /* F1 */
    BC250_FLIP_EDGE_REFUSED,            /* A */
    BC250_FLIP_EDGE_DEPOSIT_VSYNC,      /* W-v */
    BC250_FLIP_EDGE_DEPOSIT_DRAINED,    /* W-i */
    BC250_FLIP_EDGE_DEPOSIT_UNPROVEN,   /* W-u */
    BC250_FLIP_EDGE_PROGRAM,            /* E2, E3 */
    BC250_FLIP_EDGES
} BC250_FLIP_EDGE;

/* What wddm.c knows when a request arrives, each one read inside L. */
typedef struct _BC250_FLIP_FACTS {
    unsigned Faulting;          /* E5 was entered on this start */
    unsigned GenerationOdd;     /* the primary generation (PrimarySequence) is odd */
    unsigned WriterActive;      /* a writer is between Begin and End */
    unsigned Admitted;          /* the admission passed */
    unsigned Immediate;         /* the request's FlipImmediate */
    unsigned Drained;           /* Bc250FlipDrained below, read for this request */
    unsigned Resumed;           /* the active writer called Resume */
} BC250_FLIP_FACTS;

static __inline unsigned Bc250FlipEdge(const BC250_FLIP_FACTS* Facts)
{
    if (Facts->Faulting) return BC250_FLIP_EDGE_FAULTING;
    if ((Facts->GenerationOdd != 0) != (Facts->WriterActive != 0)) return BC250_FLIP_EDGE_OVERLAP;
    if (!Facts->Admitted) return BC250_FLIP_EDGE_REFUSED;
    if (Facts->WriterActive) {
        if (!Facts->Immediate) return BC250_FLIP_EDGE_DEPOSIT_VSYNC;
        if (Facts->Drained && !Facts->Resumed) return BC250_FLIP_EDGE_DEPOSIT_DRAINED;
        return BC250_FLIP_EDGE_DEPOSIT_UNPROVEN;
    }
    return BC250_FLIP_EDGE_PROGRAM;
}

/* DRAINED: one readback of DCHUBP_CNTL with all three fields set (AMD hubp1_set_blank's end state, the readback of
 * Bc250Dcn201PlaneBlank). A failed read proves nothing. */
static __inline unsigned Bc250FlipDrained(unsigned ReadOk, unsigned BlankEn, unsigned TtuDisable, unsigned NoOutstanding)
{
    return ReadOk && BlankEn && TtuDisable && NoOutstanding;
}

/* The answer to a request that edge E programmed. Wrote: the write sequence ran (a refusal before it is A, and
 * DcnFlipSourceAddress says which). WriteOk: it completed. Proof: P' held in the call (SURFACE_FLIP_PENDING first
 * observed clear and PRIMARY_SURFACE_ADDRESS read back as the request at one sample, the generation unchanged). */
typedef enum _BC250_FLIP_RESULT {
    BC250_FLIP_RESULT_REFUSED = 0,      /* A: nothing written, nothing published */
    BC250_FLIP_RESULT_SUCCESS,
    BC250_FLIP_RESULT_FAULT_PROGRAM,    /* E5, BC250_FLIP_FAULT_PROGRAM */
    BC250_FLIP_RESULT_UNPROVEN,         /* U */
    BC250_FLIP_RESULTS
} BC250_FLIP_RESULT;

static __inline unsigned Bc250FlipResult(unsigned Wrote, unsigned WriteOk, unsigned Immediate, unsigned Proof)
{
    if (!Wrote) return BC250_FLIP_RESULT_REFUSED;
    if (!WriteOk) return BC250_FLIP_RESULT_FAULT_PROGRAM;
    if (Immediate && !Proof) return BC250_FLIP_RESULT_UNPROVEN;
    return BC250_FLIP_RESULT_SUCCESS;
}

/* A writer's Begin, one try. A writer never starts in FAULTING (section 7.2, E5 step 6). Another writer's odd
 * generation is retried by the caller (200 tries 10 us apart, then refused). */
typedef enum _BC250_WRITER_BEGIN {
    BC250_WRITER_BEGIN_TAKE = 0,
    BC250_WRITER_BEGIN_RETRY,
    BC250_WRITER_BEGIN_FAULTING,
    BC250_WRITER_BEGIN_REFUSED,         /* bridge admission refused before a write */
    BC250_WRITER_BEGINS
} BC250_WRITER_BEGIN;

static __inline unsigned Bc250WriterBegin(unsigned Faulting, unsigned GenerationOdd)
{
    if (Faulting) return BC250_WRITER_BEGIN_FAULTING;
    if (GenerationOdd) return BC250_WRITER_BEGIN_RETRY;
    return BC250_WRITER_BEGIN_TAKE;
}

/* A writer's End. Deposit: a request deposited during this writer that no unblank programmed. Owed: a next-VSync
 * request answered success that no CRTC_VSYNC report carried yet. Either one makes End program Desired with the
 * next-VSync type, so that the next real report carries it. Only a running OTG makes that report: with the gate open
 * no timer makes a CRTC_VSYNC (wddm.c's WddmVSyncArm), so End with the OTG stopped, or its read failed, is E5.
 * OtgRunning: 1 running, 0 stopped, anything else the read failed. A Desired that no longer fits the source is the
 * programming's refusal (Bc250FlipResult REFUSED): dropped and counted. */
typedef enum _BC250_WRITER_END {
    BC250_WRITER_END_NOTHING = 0,
    BC250_WRITER_END_PROGRAM,
    BC250_WRITER_END_FAULT,             /* E5, BC250_FLIP_FAULT_OWED_STOPPED */
    BC250_WRITER_ENDS
} BC250_WRITER_END;

static __inline unsigned Bc250WriterEnd(unsigned Deposit, unsigned Owed, int OtgRunning)
{
    if (!Deposit && !Owed) return BC250_WRITER_END_NOTHING;
    if (OtgRunning != 1) return BC250_WRITER_END_FAULT;
    return BC250_WRITER_END_PROGRAM;
}

/* The writer that holds the primary transaction (WddmPrimaryExclusiveBegin's Kind), for the W-u record. */
typedef enum _BC250_WRITER_KIND {
    BC250_WRITER_NONE = 0,
    BC250_WRITER_MODESET_COMMIT,        /* modeset.c ModesetCommit */
    BC250_WRITER_MODESET_D3,            /* modeset.c ModesetPowerDown */
    BC250_WRITER_MODESET_D0,            /* modeset.c ModesetPowerUp */
    BC250_WRITER_LINK_COMMIT,           /* connector_link.c ConnectorLinkCommit */
    BC250_WRITER_LINK_DISABLE,          /* connector_link.c ConnectorLinkDisable */
    BC250_WRITER_LINK_SUSPEND,          /* connector_link.c ConnectorLinkSuspend */
    BC250_WRITER_LINK_RESUME,           /* connector_link.c ConnectorLinkResume */
    BC250_WRITER_HDMI_START,            /* hdmi_link.c HdmiLinkStart */
    BC250_WRITER_HDMI_STOP,             /* hdmi_link.c HdmiLinkStop */
    BC250_WRITER_DESTROY,               /* wddm.c WddmDestroyRestore */
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    BC250_WRITER_LATCH_PROBE,           /* lab-only bounded DCN experiment */
#endif
    BC250_WRITER_KINDS
} BC250_WRITER_KIND;

/* Legacy deposit record classification, retained for diagnostic format compatibility. Hardware writer overlaps
 * are now refused. Unblank means an earlier revision programmed the deposit before End. Current: no newer deposit
 * came after it. Answer: End's
 * Bc250WriterEnd. Result: Bc250FlipResult of End's programming, read only for BC250_WRITER_END_PROGRAM. */
typedef enum _BC250_WU_END {
    BC250_WU_OPEN = 0,                  /* the writer has not ended */
    BC250_WU_UNBLANK,                   /* the writer's unblank programmed it */
    BC250_WU_END_PROGRAM,               /* End programmed it */
    BC250_WU_END_REFUSED,               /* End's programming refused it: dropped, counted (OwedInvalidated) */
    BC250_WU_END_FAULT,                 /* End's programming failed, or the OTG was not running: E5 with End's code */
    BC250_WU_SUPERSEDED,                /* a newer deposit replaced it before End */
    BC250_WU_DROPPED,                   /* nothing owed any more: the destroy restore took Desired away, or FAULTING */
    BC250_WU_ENDS
} BC250_WU_END;

static __inline unsigned Bc250WuEnd(unsigned Unblank, unsigned Current, unsigned Answer, unsigned Result)
{
    if (Unblank) return BC250_WU_UNBLANK;
    if (!Current) return BC250_WU_SUPERSEDED;
    if (Answer == BC250_WRITER_END_FAULT) return BC250_WU_END_FAULT;
    if (Answer != BC250_WRITER_END_PROGRAM) return BC250_WU_DROPPED;
    if (Result == BC250_FLIP_RESULT_SUCCESS) return BC250_WU_END_PROGRAM;
    if (Result == BC250_FLIP_RESULT_REFUSED) return BC250_WU_END_REFUSED;
    return BC250_WU_END_FAULT;
}

/* E5 step 5, the drain record of the DPC that E5 queued: at most BC250_FLIP_DRAIN_READS reads of DCHUBP_CNTL, 1 us
 * apart. DRAINED ends it; a failed read or the last read without DRAINED leaves fetch ownership unknown. */
#define BC250_FLIP_DRAIN_READS 50u
typedef enum _BC250_FLIP_DRAIN {
    BC250_FLIP_DRAIN_NONE = 0,          /* no fault, or the DPC has not run */
    BC250_FLIP_DRAIN_DRAINED,
    BC250_FLIP_DRAIN_UNKNOWN,
    BC250_FLIP_DRAIN_AGAIN,             /* not a record: read again */
    BC250_FLIP_DRAINS
} BC250_FLIP_DRAIN;

static __inline unsigned Bc250FlipDrainStep(unsigned Read, unsigned Drained, unsigned Reads)
{
    if (!Read) return BC250_FLIP_DRAIN_UNKNOWN;
    if (Drained) return BC250_FLIP_DRAIN_DRAINED;
    if (Reads >= BC250_FLIP_DRAIN_READS) return BC250_FLIP_DRAIN_UNKNOWN;
    return BC250_FLIP_DRAIN_AGAIN;
}

#endif
