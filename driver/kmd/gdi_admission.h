#ifndef BC250_GDI_ADMISSION_H
#define BC250_GDI_ADMISSION_H
#include "gdi_private.h"

// BD-060: the answer of DxgkDdiGetStandardAllocationDriverData, and what dxgkrnl asked for, per adapter start.
// The old evidence counted successes only (Calls[] advances after the last refusal), so four GDI successes did not
// bound refused types (Codex 900/901). Here a request is counted on entry, before any refusal, by standard
// allocation kind and phase, and for GDI surfaces by type and phase; its final status is counted by class. The
// CreateAllocation and OpenAllocation outcomes of LB7A allocations are counted by type beside them, and every
// CreateAllocation call by its final outcome (Codex 904: an allocation admitted and then rolled back by a later
// failure of the same call is counted as rolled back, not lost).
//
// Plain C over plain integers, host-tested (test/gdi_admission_test.c, gate "gdi-admission"). wddm.c defines
// BC250_ADMISSION_COUNT as an interlocked increment before it includes this file; the counters are evidence, never
// control flow, and the summary reads them without a lock.
#ifndef BC250_ADMISSION_COUNT
#define BC250_ADMISSION_COUNT(p) ((void)++*(p))
#endif

// D3DKMDT_STANDARDALLOCATION_TYPE, WDK 10.0.26100 d3dkmdt.h; wddm.c checks the four this driver answers with
// C_ASSERT. Slot 0 holds every other value (VGPU 5 and FENCESTORAGE 6 have slots of their own).
#define BC250_STDALLOC_PRIMARY 1ul
#define BC250_STDALLOC_SHADOW 2ul
#define BC250_STDALLOC_STAGING 3ul
#define BC250_STDALLOC_GDI 4ul
#define BC250_STDALLOC_KINDS 7
// D3DKMDT_GDISURFACETYPE 0..8; slot 9 holds any other value and, for CreateAllocation and OpenAllocation, an
// LB7A blob that did not parse (no type to file it under).
#define BC250_GDI_SLOTS 10

enum bc250_stdalloc_status {
    BC250_STDALLOC_OK = 0,
    BC250_STDALLOC_FLAGS,       // a GDI request with any Flags bit set
    BC250_STDALLOC_TYPE,        // GDI type 0, or a type the allocation policy does not admit
    BC250_STDALLOC_GEOMETRY,    // width/height/format have no layout
    BC250_STDALLOC_BUFFER,      // the fill phase's buffer is smaller than the size this driver answered
    BC250_STDALLOC_INPUT,       // a kind this driver does not answer, or its union member is NULL
    BC250_STDALLOC_STATUSES
};

typedef struct _BC250_STDALLOC_REQUEST {
    unsigned long Kind;             // StandardAllocationType
    int Described;                  // the kind's union member (pCreate*Data) is not NULL
    unsigned long Width, Height, Format;
    unsigned long GdiType, GdiFlags;    // GDISURFACE only
    int Fill;                       // pAllocationPrivateDriverData != NULL; NULL is the size query
    unsigned long Bytes;            // AllocationPrivateDriverDataSize, read in the fill phase only
} BC250_STDALLOC_REQUEST;

typedef struct _BC250_STDALLOC_ANSWER {
    BC250_WDDM_ALLOCATION_PRIVATE Surface;  // LB7A v1; Pitch and Size are valid on success
    unsigned long PrivateBytes;     // 32 (LB7A) or, for a GDI surface, 48 (LB7A + GDI1)
    // Write Surface.Pitch to the union's public Pitch. The size query must not change the union
    // (ref/ddi-display/d3dkmddi.md:32953, DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA remarks), so only a fill that
    // succeeds publishes it. The shared primary's union has no output pitch.
    int PublishPitch;
} BC250_STDALLOC_ANSWER;

// The one decision. Same checks in the same order as the code before BD-060; the only behaviour change is
// PublishPitch.
static __inline int Bc250StdAllocDecide(const BC250_STDALLOC_REQUEST* R, BC250_STDALLOC_ANSWER* A)
{
    BC250_GDI_ALLOCATION_POLICY policy;
    BC250_WDDM_ALLOCATION_PRIVATE* s = &A->Surface;

    s->Magic = BC250_WDDM_ALLOCATION_PRIVATE_MAGIC;
    s->Version = 1;
    s->Width = s->Height = s->Pitch = s->Format = 0;
    s->Size = 0;
    A->PrivateBytes = sizeof(BC250_WDDM_ALLOCATION_PRIVATE);
    A->PublishPitch = 0;
    if (R->Kind < BC250_STDALLOC_PRIMARY || R->Kind > BC250_STDALLOC_GDI || !R->Described) return BC250_STDALLOC_INPUT;
    s->Width = R->Width;
    s->Height = R->Height;
    s->Format = R->Kind == BC250_STDALLOC_STAGING ? AMDGPU_WDDM_D3DDDI_A8R8G8B8 : R->Format;
    if (R->Kind == BC250_STDALLOC_GDI) {
        if (R->GdiFlags) return BC250_STDALLOC_FLAGS;
        if (!R->GdiType || !WddmGdiAllocationPolicy(R->GdiType, 0, 0, &policy)) return BC250_STDALLOC_TYPE;
        A->PrivateBytes = sizeof(BC250_GDI_PRIVATE);
        // Standard GDI textures are opened by the DWM UMD. Its LB7A OpenResource contract needs pitch alignment
        // and a four-row-padded allocation, like the UMD-created shared textures; staging uses its format size
        // and the advertised four-byte row alignment. Logical Width/Height stay unchanged.
        if (!WddmGdiLayout(s->Width, s->Height, R->GdiType, WddmSurfaceFormatBpp(s->Format, BC250_SURFACE_GDI),
                           &s->Pitch, &s->Size))
            return BC250_STDALLOC_GEOMETRY;
    } else {
        if (!s->Width || s->Width > 0xfffffffful / 4ul) return BC250_STDALLOC_GEOMETRY;
        s->Pitch = R->Kind == BC250_STDALLOC_PRIMARY ? DcnPrimaryPitch(s->Width) : s->Width * 4ul;
        if (!DcnSurfaceBytes(s->Width, s->Height, s->Pitch, &s->Size)) return BC250_STDALLOC_GEOMETRY;
    }
    if (R->Fill && R->Bytes < A->PrivateBytes) return BC250_STDALLOC_BUFFER;
    A->PublishPitch = R->Fill && R->Kind != BC250_STDALLOC_PRIMARY;
    return BC250_STDALLOC_OK;
}

// What CreateAllocation and OpenAllocation make of a received blob that is not a BC2A one: UNREAD when it is no
// LB7A blob this driver could have written (no type to file it under), REFUSED when it is but the geometry, format
// or policy does not admit it, ADMITTED with *Type and *Policy set. The checks of 53bdebbe in its order.
// OpenAllocation passes ApertureOffered 1 and no CPU hints, as before: it opens what CreateAllocation admitted.
#define BC250_LB7A_UNREAD 0
#define BC250_LB7A_REFUSED 1
#define BC250_LB7A_ADMITTED 2
static __inline int Bc250Lb7aAdmit(const void* Data, unsigned int Bytes, int SharedCpu, int CachedCpu,
    int ApertureOffered, unsigned long* Type, BC250_GDI_ALLOCATION_POLICY* Policy)
{
    const BC250_WDDM_ALLOCATION_PRIVATE* s = (const BC250_WDDM_ALLOCATION_PRIVATE*)Data;
    *Type = 0;
    if (!s || Bytes < sizeof(*s) || s->Magic != BC250_WDDM_ALLOCATION_PRIVATE_MAGIC || !s->Size ||
        !WddmGdiPrivate(s, Bytes, Type)) return BC250_LB7A_UNREAD;
    // A standard type the policy puts in the aperture (type 2) needs the segment; legacy type 0 keeps its rule.
    if (!WddmSurfaceAdmitted(s, *Type) || !WddmGdiAllocationPolicy(*Type, SharedCpu, CachedCpu, Policy) ||
        (*Type && Policy->Aperture && !ApertureOffered)) return BC250_LB7A_REFUSED;
    return BC250_LB7A_ADMITTED;
}

static const char* const g_Bc250StdAllocStatusNames[BC250_STDALLOC_STATUSES] = {
    "ok", "flags", "type", "geom", "buf", "input" };

// CreateAllocation, per LB7A allocation: created (the whole call succeeded), refused (its own failure: the
// admission above, or its object could not be made) and rolled back (admitted, then freed because a later
// allocation of the same call or the resource object failed). Per call: the final outcome.
#define BC250_CREATE_CREATED 0
#define BC250_CREATE_REFUSED 1
#define BC250_CREATE_ROLLED_BACK 2
enum bc250_create_outcome {
    BC250_CREATE_OK = 0,
    BC250_CREATE_RESOURCE_DATA,     // the resource private data was refused before any allocation
    BC250_CREATE_INVALID,           // an allocation was refused: STATUS_INVALID_PARAMETER
    BC250_CREATE_NO_MEMORY,         // an allocation object or the resource object: STATUS_INSUFFICIENT_RESOURCES
    BC250_CREATE_OUTCOMES
};

typedef struct _BC250_STDALLOC_COUNTERS {
    volatile long Requests[BC250_STDALLOC_KINDS][2];        // kind x phase (0 size, 1 fill), on entry
    volatile long Answers[BC250_STDALLOC_KINDS][BC250_STDALLOC_STATUSES];
    volatile long GdiRequests[BC250_GDI_SLOTS][2];          // GDI type x phase, on entry (union member present)
    volatile long GdiAnswers[BC250_GDI_SLOTS][BC250_STDALLOC_STATUSES];
    volatile long GdiCreated[BC250_GDI_SLOTS][3];           // CreateAllocation per LB7A allocation, BC250_CREATE_*
    volatile long GdiOpened[BC250_GDI_SLOTS][2];            // OpenAllocation per LB7A allocation: opened, NULL handle
    volatile long CreateCalls[BC250_CREATE_OUTCOMES];       // CreateAllocation calls by final outcome
} BC250_STDALLOC_COUNTERS;

static __inline unsigned long Bc250StdAllocKindSlot(unsigned long Kind)
{
    return Kind >= 1 && Kind < BC250_STDALLOC_KINDS ? Kind : 0;
}
static __inline unsigned long Bc250GdiSlot(unsigned long Type)
{
    return Type < BC250_GDI_SLOTS - 1 ? Type : BC250_GDI_SLOTS - 1;
}

// On entry, before anything can refuse.
static __inline void Bc250StdAllocEnter(BC250_STDALLOC_COUNTERS* C, const BC250_STDALLOC_REQUEST* R)
{
    unsigned long phase = R->Fill ? 1 : 0;
    BC250_ADMISSION_COUNT(&C->Requests[Bc250StdAllocKindSlot(R->Kind)][phase]);
    if (R->Kind == BC250_STDALLOC_GDI && R->Described)
        BC250_ADMISSION_COUNT(&C->GdiRequests[Bc250GdiSlot(R->GdiType)][phase]);
}

// The final status of the same request.
static __inline void Bc250StdAllocLeave(BC250_STDALLOC_COUNTERS* C, const BC250_STDALLOC_REQUEST* R, int Status)
{
    if (Status < 0 || Status >= BC250_STDALLOC_STATUSES) Status = BC250_STDALLOC_INPUT;
    BC250_ADMISSION_COUNT(&C->Answers[Bc250StdAllocKindSlot(R->Kind)][Status]);
    if (R->Kind == BC250_STDALLOC_GDI && R->Described)
        BC250_ADMISSION_COUNT(&C->GdiAnswers[Bc250GdiSlot(R->GdiType)][Status]);
}

// The control flow of DxgkDdiCreateAllocation around the per-allocation work, so that its rollback paths run under
// the host test with injected failures (resource-object OOM, a refusal or OOM after earlier admissions). wddm.c
// supplies the operations; Admit is the old loop body. On a failure everything admitted is freed in reverse order,
// as before, and each LB7A allocation among it is counted as rolled back.
#define BC250_CREATE_NOT_LB7A 0xfffffffful      // a slot for a BC2A (UMD) allocation: not counted by GDI type
#define BC250_CREATE_STEP_ADMITTED 0
#define BC250_CREATE_STEP_REFUSED 1
#define BC250_CREATE_STEP_NO_MEMORY 2
typedef struct _BC250_CREATE_OPS {
    // Allocation Index: an object made and stored on ADMITTED; on a failure nothing is left behind. *Slot is the
    // GDI slot of an LB7A blob (refused ones too; BC250_GDI_SLOTS - 1 when it did not parse) or NOT_LB7A.
    int (*Admit)(void* Context, unsigned long Index, unsigned long* Slot);
    unsigned long (*Slot)(void* Context, unsigned long Index);  // of an admitted allocation
    void (*Free)(void* Context, unsigned long Index);
    int (*Resource)(void* Context);     // the resource object when the call asks for one; 0 when it cannot be made
} BC250_CREATE_OPS;

static __inline void Bc250CreateCountSlot(BC250_STDALLOC_COUNTERS* C, unsigned long Slot, int Column)
{
    if (C && Slot != BC250_CREATE_NOT_LB7A) BC250_ADMISSION_COUNT(&C->GdiCreated[Bc250GdiSlot(Slot)][Column]);
}

static __inline int Bc250CreateRun(BC250_STDALLOC_COUNTERS* C, const BC250_CREATE_OPS* Ops, void* Context,
    unsigned long Count)
{
    unsigned long i, admitted = 0;
    int outcome = BC250_CREATE_OK;
    for (i = 0; i < Count; i++) {
        unsigned long slot = BC250_CREATE_NOT_LB7A;
        int step = Ops->Admit(Context, i, &slot);
        if (step != BC250_CREATE_STEP_ADMITTED) {
            Bc250CreateCountSlot(C, slot, BC250_CREATE_REFUSED);
            outcome = step == BC250_CREATE_STEP_NO_MEMORY ? BC250_CREATE_NO_MEMORY : BC250_CREATE_INVALID;
            admitted = i;
            goto rollback;
        }
    }
    if (!Ops->Resource(Context)) {
        outcome = BC250_CREATE_NO_MEMORY;
        admitted = Count;
        goto rollback;
    }
    for (i = 0; i < Count; i++) Bc250CreateCountSlot(C, Ops->Slot(Context, i), BC250_CREATE_CREATED);
    if (C) BC250_ADMISSION_COUNT(&C->CreateCalls[BC250_CREATE_OK]);
    return BC250_CREATE_OK;
rollback:
    while (admitted-- > 0) {
        Bc250CreateCountSlot(C, Ops->Slot(Context, admitted), BC250_CREATE_ROLLED_BACK);
        Ops->Free(Context, admitted);
    }
    if (C) BC250_ADMISSION_COUNT(&C->CreateCalls[outcome]);
    return outcome;
}

// One bit per GDI slot (bit 9 = any other type), for a gate that asserts "no refused standard GDI type" after DWM
// started: requested (either phase), refused by the standard answer, refused at CreateAllocation (its own failure),
// rolled back at CreateAllocation, NULL at open.
typedef struct _BC250_GDI_MASKS {
    unsigned long Requested, Refused, CreateRefused, CreateRolledBack, OpenRefused;
} BC250_GDI_MASKS;
static __inline void Bc250GdiMasks(const BC250_STDALLOC_COUNTERS* C, BC250_GDI_MASKS* M)
{
    unsigned long i;
    int status;
    M->Requested = M->Refused = M->CreateRefused = M->CreateRolledBack = M->OpenRefused = 0;
    for (i = 0; i < BC250_GDI_SLOTS; i++) {
        if (C->GdiRequests[i][0] || C->GdiRequests[i][1]) M->Requested |= 1ul << i;
        for (status = BC250_STDALLOC_OK + 1; status < BC250_STDALLOC_STATUSES; status++)
            if (C->GdiAnswers[i][status]) M->Refused |= 1ul << i;
        if (C->GdiCreated[i][BC250_CREATE_REFUSED]) M->CreateRefused |= 1ul << i;
        if (C->GdiCreated[i][BC250_CREATE_ROLLED_BACK]) M->CreateRolledBack |= 1ul << i;
        if (C->GdiOpened[i][1]) M->OpenRefused |= 1ul << i;
    }
}
#endif
