// Host test of gdi_admission.h (BD-060): the standard-allocation decision of DxgkDdiGetStandardAllocationDriverData,
// its per-start request/answer counters, the LB7A admission of CreateAllocation/OpenAllocation, and the control flow
// of CreateAllocation with injected failures (review 904). Built with no WDK header by run_gdi_admission.ps1, beside
// dcn_translate.c, which holds the layouts it calls.
#include <stdio.h>
#include <string.h>
#include "../gdi_admission.h"

static int g_failures, g_checks;

#define CHECK(cond) \
    do { g_checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

#define FMT_A8R8G8B8 21ul
#define FMT_X8R8G8B8 22ul
#define FMT_A8 28ul

static BC250_STDALLOC_REQUEST Gdi(unsigned long Type, unsigned long W, unsigned long H, unsigned long Format, int Fill)
{
    BC250_STDALLOC_REQUEST r;
    memset(&r, 0, sizeof(r));
    r.Kind = BC250_STDALLOC_GDI; r.Described = 1;
    r.Width = W; r.Height = H; r.Format = Format; r.GdiType = Type;
    r.Fill = Fill; r.Bytes = Fill ? 48 : 0;
    return r;
}

// The decision before BD-060, transcribed from wddm.c at 53bdebbe: 1 success with its pitch/size/bytes, 0 refusal.
static int OldDecision(const BC250_STDALLOC_REQUEST* R, unsigned long* Pitch, unsigned long long* Size, unsigned long* Bytes)
{
    BC250_GDI_ALLOCATION_POLICY p;
    unsigned long format = R->Format, privateBytes = 32;
    if (!R->Described || R->Kind < 1 || R->Kind > 4) return 0;
    if (R->Kind == 3) format = FMT_A8R8G8B8;
    if (R->Kind == 4) {
        if (R->GdiFlags || !R->GdiType || !WddmGdiAllocationPolicy(R->GdiType, 0, 0, &p)) return 0;
        privateBytes = 48;
        if (!WddmGdiLayout(R->Width, R->Height, R->GdiType, WddmSurfaceFormatBpp(format, BC250_SURFACE_GDI), Pitch, Size))
            return 0;
    } else {
        if (!R->Width || R->Width > 0xfffffffful / 4) return 0;
        *Pitch = R->Kind == 1 ? DcnPrimaryPitch(R->Width) : R->Width * 4;
        if (!DcnSurfaceBytes(R->Width, R->Height, *Pitch, Size)) return 0;
    }
    if (R->Fill && R->Bytes < privateBytes) return 0;
    *Bytes = privateBytes;
    return 1;
}

// The answer is the old one for every kind, type, format, phase and a spread of shapes.
static void SameAsBefore(void)
{
    static const unsigned long widths[] = { 0, 1, 3, 33, 64, 65, 1920, 0x40000000ul, 0xfffffffful };
    static const unsigned long heights[] = { 0, 1, 3, 7, 1080, 0xfffffffful };
    static const unsigned long formats[] = { 0, FMT_A8R8G8B8, FMT_X8R8G8B8, FMT_A8, 31, 32, 33, 113 };
    unsigned long kind, type, w, h, f, fill, bytes, flags;
    unsigned long compared = 0, admitted = 0;
    for (kind = 0; kind <= 6; kind++) for (type = 0; type <= 10; type++) for (w = 0; w < 9; w++)
    for (h = 0; h < 6; h++) for (f = 0; f < 8; f++) for (fill = 0; fill < 2; fill++) for (flags = 0; flags < 2; flags++)
    for (bytes = 0; bytes < 3; bytes++) {
        BC250_STDALLOC_REQUEST r;
        BC250_STDALLOC_ANSWER a;
        unsigned long pitch = 0, oldBytes = 0;
        unsigned long long size = 0;
        int old, now;
        if (kind != 4 && (type || flags)) continue;
        if (!fill && bytes) continue;
        memset(&r, 0, sizeof(r));
        r.Kind = kind; r.Described = 1; r.Width = widths[w]; r.Height = heights[h]; r.Format = formats[f];
        r.GdiType = type; r.GdiFlags = flags; r.Fill = (int)fill; r.Bytes = bytes == 0 ? 48 : bytes == 1 ? 32 : 31;
        old = OldDecision(&r, &pitch, &size, &oldBytes);
        now = Bc250StdAllocDecide(&r, &a);
        CHECK(old == (now == BC250_STDALLOC_OK));
        if (old && now == BC250_STDALLOC_OK) {
            CHECK(a.Surface.Pitch == pitch && a.Surface.Size == size && a.PrivateBytes == oldBytes);
            CHECK(a.Surface.Magic == BC250_WDDM_ALLOCATION_PRIVATE_MAGIC && a.Surface.Version == 1);
            admitted++;
        }
        compared++;
    }
    CHECK(compared > 10000 && admitted > 1000);
    printf("same as before: %lu requests compared, %lu admitted\n", compared, admitted);
}

// Review 900/901: a refused type is counted at entry and as a refusal, never as a success.
static void RefusalsCounted(void)
{
    unsigned long type;
    for (type = 5; type <= 9; type++) {
        BC250_STDALLOC_COUNTERS c;
        BC250_STDALLOC_ANSWER a;
        BC250_STDALLOC_REQUEST r = Gdi(type, 64, 4, FMT_A8R8G8B8, 0);
        int status;
        memset(&c, 0, sizeof(c));
        Bc250StdAllocEnter(&c, &r);
        CHECK(c.Requests[4][0] == 1 && c.GdiRequests[type][0] == 1);
        status = Bc250StdAllocDecide(&r, &a);
        CHECK(status == BC250_STDALLOC_TYPE);
        Bc250StdAllocLeave(&c, &r, status);
        CHECK(c.Answers[4][BC250_STDALLOC_TYPE] == 1 && c.Answers[4][BC250_STDALLOC_OK] == 0);
        CHECK(c.GdiAnswers[type][BC250_STDALLOC_TYPE] == 1 && c.GdiAnswers[type][BC250_STDALLOC_OK] == 0);
        CHECK(!a.PublishPitch);
    }
    {
        // Each class lands in its own column; type 2 size and fill are two requests, two successes.
        BC250_STDALLOC_COUNTERS c;
        BC250_STDALLOC_ANSWER a;
        BC250_STDALLOC_REQUEST r;
        int i, status;
        memset(&c, 0, sizeof(c));
        for (i = 0; i < 2; i++) {
            r = Gdi(2, 33, 7, FMT_A8R8G8B8, i);
            Bc250StdAllocEnter(&c, &r); status = Bc250StdAllocDecide(&r, &a); Bc250StdAllocLeave(&c, &r, status);
            CHECK(status == BC250_STDALLOC_OK);
        }
        CHECK(c.GdiRequests[2][0] == 1 && c.GdiRequests[2][1] == 1 && c.GdiAnswers[2][BC250_STDALLOC_OK] == 2);
        r = Gdi(1, 33, 7, FMT_A8R8G8B8, 0); r.GdiFlags = 1;
        Bc250StdAllocEnter(&c, &r); status = Bc250StdAllocDecide(&r, &a); Bc250StdAllocLeave(&c, &r, status);
        CHECK(status == BC250_STDALLOC_FLAGS && c.GdiAnswers[1][BC250_STDALLOC_FLAGS] == 1);
        r = Gdi(1, 33, 7, FMT_A8, 0);   // texture is 4 bytes a pixel only
        Bc250StdAllocEnter(&c, &r); status = Bc250StdAllocDecide(&r, &a); Bc250StdAllocLeave(&c, &r, status);
        CHECK(status == BC250_STDALLOC_GEOMETRY && c.GdiAnswers[1][BC250_STDALLOC_GEOMETRY] == 1);
        r = Gdi(1, 33, 7, FMT_A8R8G8B8, 1); r.Bytes = 47;
        Bc250StdAllocEnter(&c, &r); status = Bc250StdAllocDecide(&r, &a); Bc250StdAllocLeave(&c, &r, status);
        CHECK(status == BC250_STDALLOC_BUFFER && c.GdiAnswers[1][BC250_STDALLOC_BUFFER] == 1 && !a.PublishPitch);
        CHECK(c.GdiRequests[1][0] == 2 && c.GdiRequests[1][1] == 1 && c.GdiAnswers[1][BC250_STDALLOC_OK] == 0);
        r = Gdi(0, 33, 7, FMT_A8R8G8B8, 0);
        Bc250StdAllocEnter(&c, &r); status = Bc250StdAllocDecide(&r, &a); Bc250StdAllocLeave(&c, &r, status);
        CHECK(status == BC250_STDALLOC_TYPE && c.GdiAnswers[0][BC250_STDALLOC_TYPE] == 1);
        r = Gdi(0x80000000ul, 33, 7, FMT_A8R8G8B8, 0);
        Bc250StdAllocEnter(&c, &r); status = Bc250StdAllocDecide(&r, &a); Bc250StdAllocLeave(&c, &r, status);
        CHECK(status == BC250_STDALLOC_TYPE && c.GdiRequests[9][0] == 1 && c.GdiAnswers[9][BC250_STDALLOC_TYPE] == 1);
        // A GDI kind without its union member has no type to file: kind counted, no GDI row.
        r = Gdi(2, 33, 7, FMT_A8R8G8B8, 0); r.Described = 0;
        Bc250StdAllocEnter(&c, &r); status = Bc250StdAllocDecide(&r, &a); Bc250StdAllocLeave(&c, &r, status);
        CHECK(status == BC250_STDALLOC_INPUT && c.GdiRequests[2][0] == 1 && c.Answers[4][BC250_STDALLOC_INPUT] == 1);
        CHECK(c.Requests[4][0] == 6 && c.Requests[4][1] == 2);
        // Kinds this driver does not answer: VGPU 5 has its slot, unknown values share slot 0.
        memset(&r, 0, sizeof(r)); r.Kind = 5; r.Described = 1;
        Bc250StdAllocEnter(&c, &r); status = Bc250StdAllocDecide(&r, &a); Bc250StdAllocLeave(&c, &r, status);
        CHECK(status == BC250_STDALLOC_INPUT && c.Requests[5][0] == 1 && c.Answers[5][BC250_STDALLOC_INPUT] == 1);
        r.Kind = 77;
        Bc250StdAllocEnter(&c, &r); status = Bc250StdAllocDecide(&r, &a); Bc250StdAllocLeave(&c, &r, status);
        CHECK(status == BC250_STDALLOC_INPUT && c.Requests[0][0] == 1 && c.Answers[0][BC250_STDALLOC_INPUT] == 1);
        Bc250StdAllocLeave(&c, &r, 99);     // an out-of-range status is filed as input, never out of bounds
        CHECK(c.Answers[0][BC250_STDALLOC_INPUT] == 2);
    }
    CHECK(Bc250GdiSlot(8) == 8 && Bc250GdiSlot(9) == 9 && Bc250GdiSlot(0xfffffffful) == 9);
    CHECK(Bc250StdAllocKindSlot(0) == 0 && Bc250StdAllocKindSlot(6) == 6 && Bc250StdAllocKindSlot(7) == 0);
}

// CreateAllocation's admission of a received LB7A blob at 53bdebbe, transcribed (OpenAllocation is the same with no
// CPU hints and no aperture check): 1 admitted, 0 refused.
static int OldCreate(const BC250_GDI_PRIVATE* B, unsigned int Bytes, int Shared, int Cached, int Aperture,
    unsigned long* Type, BC250_GDI_ALLOCATION_POLICY* P)
{
    const BC250_WDDM_ALLOCATION_PRIVATE* s = &B->Surface;
    *Type = 0;
    return Bytes >= sizeof(*s) && s->Magic == BC250_WDDM_ALLOCATION_PRIVATE_MAGIC && s->Size != 0 &&
        WddmGdiPrivate(s, Bytes, Type) && WddmSurfaceAdmitted(s, *Type) &&
        WddmGdiAllocationPolicy(*Type, Shared, Cached, P) && !(*Type == 2 && !Aperture);
}

// Create and open admit exactly what 53bdebbe admitted; types 5-8 stay refused.
static void ReceivedBlobs(void)
{
    static const unsigned long widths[] = { 1, 33, 64, 257 }, heights[] = { 1, 5, 8 };
    static const unsigned long formats[] = { FMT_A8R8G8B8, FMT_X8R8G8B8, FMT_A8, 32, 31, 113, 0 };
    unsigned long type, w, h, f, gotType, oldType, compared = 0, admitted = 0;
    int shared, cached, aperture, layout, bytes;
    BC250_GDI_ALLOCATION_POLICY p, oldP;
    BC250_GDI_PRIVATE b;
    for (type = 0; type <= 9; type++) for (w = 0; w < 4; w++) for (h = 0; h < 3; h++) for (f = 0; f < 7; f++)
    for (layout = 0; layout < 3; layout++) for (bytes = 0; bytes < 2; bytes++)
    for (shared = 0; shared < 2; shared++) for (cached = 0; cached < 2; cached++) for (aperture = 0; aperture < 2; aperture++) {
        int old, now;
        memset(&b, 0, sizeof(b));
        b.Surface.Magic = BC250_WDDM_ALLOCATION_PRIVATE_MAGIC; b.Surface.Version = 1;
        b.Surface.Width = widths[w]; b.Surface.Height = heights[h]; b.Surface.Format = formats[f];
        if (layout == 0) DcnSharedTextureLayout(b.Surface.Width, b.Surface.Height, &b.Surface.Pitch, &b.Surface.Size);
        else if (layout == 1) DcnStagingLayout(b.Surface.Width, b.Surface.Height, 4, &b.Surface.Pitch, &b.Surface.Size);
        else DcnStagingLayout(b.Surface.Width, b.Surface.Height, 1, &b.Surface.Pitch, &b.Surface.Size);
        b.Magic = BC250_GDI_PRIVATE_MAGIC; b.Type = type;
        old = OldCreate(&b, bytes ? 48u : 32u, shared, cached, aperture, &oldType, &oldP);
        now = Bc250Lb7aAdmit(&b, bytes ? 48u : 32u, shared, cached, aperture, &gotType, &p);
        compared++;
        CHECK(old == (now == BC250_LB7A_ADMITTED));
        if (old) { CHECK(gotType == oldType && !memcmp(&p, &oldP, sizeof(p))); admitted++; }
        if (gotType >= 5 && gotType <= 8) CHECK(now == BC250_LB7A_REFUSED);
    }
    CHECK(admitted > 0);
    printf("received blobs: %lu combinations compared, %lu admitted\n", compared, admitted);
    memset(&b, 0, sizeof(b));
    CHECK(Bc250Lb7aAdmit(0, 48, 0, 0, 1, &gotType, &p) == BC250_LB7A_UNREAD);
    CHECK(Bc250Lb7aAdmit(&b, 48, 0, 0, 1, &gotType, &p) == BC250_LB7A_UNREAD);     // no magic
    b.Surface.Magic = BC250_WDDM_ALLOCATION_PRIVATE_MAGIC; b.Surface.Version = 1; b.Surface.Width = 64; b.Surface.Height = 4;
    DcnStagingLayout(64, 4, 4, &b.Surface.Pitch, &b.Surface.Size); b.Surface.Format = FMT_A8R8G8B8;
    b.Magic = BC250_GDI_PRIVATE_MAGIC; b.Type = 2;
    CHECK(Bc250Lb7aAdmit(&b, 31, 0, 0, 1, &gotType, &p) == BC250_LB7A_UNREAD);
    CHECK(Bc250Lb7aAdmit(&b, 47, 0, 0, 1, &gotType, &p) == BC250_LB7A_UNREAD);
    CHECK(Bc250Lb7aAdmit(&b, 48, 0, 0, 0, &gotType, &p) == BC250_LB7A_REFUSED);     // aperture not offered
    CHECK(Bc250Lb7aAdmit(&b, 48, 0, 0, 1, &gotType, &p) == BC250_LB7A_ADMITTED && gotType == 2);
}

// ---- CreateAllocation with injected failures (review 904) ----------------------------------------------------------

#define MAX_ALLOCS 8
// One allocation of the model: its slot (GDI type or BC250_CREATE_NOT_LB7A) and what Admit does with it.
typedef struct { unsigned long Slot; int Step; } MODEL_ALLOC;
typedef struct {
    MODEL_ALLOC Allocs[MAX_ALLOCS];
    int Live[MAX_ALLOCS];       // an object exists for this index
    int ResourceFails;          // resource-object OOM
    int ResourceCalls, Frees, Leaked, DoubleFree;
    unsigned long FreeOrder[MAX_ALLOCS], FreeCount;
} MODEL;

static int ModelAdmit(void* Context, unsigned long Index, unsigned long* Slot)
{
    MODEL* m = (MODEL*)Context;
    *Slot = m->Allocs[Index].Slot;
    if (m->Allocs[Index].Step == BC250_CREATE_STEP_ADMITTED) m->Live[Index] = 1;
    return m->Allocs[Index].Step;
}
static unsigned long ModelSlot(void* Context, unsigned long Index)
{
    MODEL* m = (MODEL*)Context;
    CHECK(m->Live[Index]);
    return m->Allocs[Index].Slot;
}
static void ModelFree(void* Context, unsigned long Index)
{
    MODEL* m = (MODEL*)Context;
    if (!m->Live[Index]) m->DoubleFree++;
    m->Live[Index] = 0;
    m->Frees++;
    m->FreeOrder[m->FreeCount++] = Index;
}
static int ModelResource(void* Context)
{
    MODEL* m = (MODEL*)Context;
    m->ResourceCalls++;
    return !m->ResourceFails;
}
static const BC250_CREATE_OPS g_ModelOps = { ModelAdmit, ModelSlot, ModelFree, ModelResource };

static int RunModel(MODEL* M, BC250_STDALLOC_COUNTERS* C, unsigned long Count)
{
    unsigned long i;
    int outcome = Bc250CreateRun(C, &g_ModelOps, M, Count);
    for (i = 0; i < Count; i++) if (M->Live[i] && outcome != BC250_CREATE_OK) M->Leaked++;
    return outcome;
}

static void CreateRollback(void)
{
    BC250_STDALLOC_COUNTERS c;
    BC250_GDI_MASKS masks;
    MODEL m;
    unsigned long i;

    // Success: every LB7A allocation created, the BC2A one not counted by type, one ok call.
    memset(&c, 0, sizeof(c)); memset(&m, 0, sizeof(m));
    m.Allocs[0].Slot = 1; m.Allocs[1].Slot = BC250_CREATE_NOT_LB7A; m.Allocs[2].Slot = 2;
    CHECK(RunModel(&m, &c, 3) == BC250_CREATE_OK && m.ResourceCalls == 1 && !m.Frees);
    CHECK(c.GdiCreated[1][BC250_CREATE_CREATED] == 1 && c.GdiCreated[2][BC250_CREATE_CREATED] == 1);
    CHECK(c.CreateCalls[BC250_CREATE_OK] == 1);

    // Resource-object OOM after two LB7A types were admitted (904's lost case): both rolled back, call no-memory.
    memset(&c, 0, sizeof(c)); memset(&m, 0, sizeof(m));
    m.Allocs[0].Slot = 1; m.Allocs[1].Slot = 6; m.ResourceFails = 1;
    CHECK(RunModel(&m, &c, 2) == BC250_CREATE_NO_MEMORY);
    CHECK(m.Frees == 2 && !m.Leaked && !m.DoubleFree && m.FreeOrder[0] == 1 && m.FreeOrder[1] == 0);
    CHECK(c.GdiCreated[1][BC250_CREATE_ROLLED_BACK] == 1 && c.GdiCreated[6][BC250_CREATE_ROLLED_BACK] == 1);
    CHECK(!c.GdiCreated[1][BC250_CREATE_CREATED] && !c.GdiCreated[1][BC250_CREATE_REFUSED]);
    CHECK(c.CreateCalls[BC250_CREATE_NO_MEMORY] == 1 && !c.CreateCalls[BC250_CREATE_OK]);
    Bc250GdiMasks(&c, &masks);
    CHECK(masks.CreateRolledBack == 0x042 && !masks.CreateRefused);

    // A single valid allocation, then resource OOM: never 0/0 again.
    memset(&c, 0, sizeof(c)); memset(&m, 0, sizeof(m));
    m.Allocs[0].Slot = 6; m.ResourceFails = 1;
    CHECK(RunModel(&m, &c, 1) == BC250_CREATE_NO_MEMORY && c.GdiCreated[6][BC250_CREATE_ROLLED_BACK] == 1);

    // Multi-allocation rollback: LB7A type 1, BC2A, LB7A type 3 admitted, then a BC2A refusal at index 3.
    memset(&c, 0, sizeof(c)); memset(&m, 0, sizeof(m));
    m.Allocs[0].Slot = 1; m.Allocs[1].Slot = BC250_CREATE_NOT_LB7A; m.Allocs[2].Slot = 3;
    m.Allocs[3].Slot = BC250_CREATE_NOT_LB7A; m.Allocs[3].Step = BC250_CREATE_STEP_REFUSED;
    m.Allocs[4].Slot = 2;
    CHECK(RunModel(&m, &c, 5) == BC250_CREATE_INVALID);
    CHECK(m.Frees == 3 && !m.Leaked && !m.DoubleFree && !m.ResourceCalls);
    CHECK(m.FreeOrder[0] == 2 && m.FreeOrder[1] == 1 && m.FreeOrder[2] == 0);
    CHECK(c.GdiCreated[1][BC250_CREATE_ROLLED_BACK] == 1 && c.GdiCreated[3][BC250_CREATE_ROLLED_BACK] == 1);
    CHECK(!c.GdiCreated[2][0] && !c.GdiCreated[2][1] && !c.GdiCreated[2][2]);  // never reached
    CHECK(c.CreateCalls[BC250_CREATE_INVALID] == 1);
    for (i = 0; i < BC250_GDI_SLOTS; i++) CHECK(!c.GdiCreated[i][BC250_CREATE_REFUSED]);   // a BC2A refusal has no type

    // A BC2A object OOM after an LB7A admission: rolled back, call no-memory.
    memset(&c, 0, sizeof(c)); memset(&m, 0, sizeof(m));
    m.Allocs[0].Slot = 2; m.Allocs[1].Slot = BC250_CREATE_NOT_LB7A; m.Allocs[1].Step = BC250_CREATE_STEP_NO_MEMORY;
    CHECK(RunModel(&m, &c, 2) == BC250_CREATE_NO_MEMORY && m.Frees == 1 && !m.Leaked);
    CHECK(c.GdiCreated[2][BC250_CREATE_ROLLED_BACK] == 1 && c.CreateCalls[BC250_CREATE_NO_MEMORY] == 1);

    // An LB7A refusal after an LB7A admission: the refused one is refused (its own type), the earlier rolled back.
    memset(&c, 0, sizeof(c)); memset(&m, 0, sizeof(m));
    m.Allocs[0].Slot = 1; m.Allocs[1].Slot = 7; m.Allocs[1].Step = BC250_CREATE_STEP_REFUSED;
    CHECK(RunModel(&m, &c, 2) == BC250_CREATE_INVALID && m.Frees == 1);
    CHECK(c.GdiCreated[7][BC250_CREATE_REFUSED] == 1 && c.GdiCreated[1][BC250_CREATE_ROLLED_BACK] == 1);
    CHECK(!c.GdiCreated[7][BC250_CREATE_ROLLED_BACK] && !c.GdiCreated[1][BC250_CREATE_REFUSED]);
    Bc250GdiMasks(&c, &masks);
    CHECK(masks.CreateRefused == 0x080 && masks.CreateRolledBack == 0x002);

    // The first allocation's own object OOM: nothing to roll back.
    memset(&c, 0, sizeof(c)); memset(&m, 0, sizeof(m));
    m.Allocs[0].Slot = 9; m.Allocs[0].Step = BC250_CREATE_STEP_NO_MEMORY;
    CHECK(RunModel(&m, &c, 1) == BC250_CREATE_NO_MEMORY && !m.Frees && c.GdiCreated[9][BC250_CREATE_REFUSED] == 1);

    // Zero allocations: the resource object alone; its OOM is a call outcome with no type.
    memset(&c, 0, sizeof(c)); memset(&m, 0, sizeof(m));
    m.ResourceFails = 1;
    CHECK(RunModel(&m, &c, 0) == BC250_CREATE_NO_MEMORY && m.ResourceCalls == 1 && c.CreateCalls[BC250_CREATE_NO_MEMORY] == 1);

    // No counters (an adapter without the table): the same control flow.
    memset(&m, 0, sizeof(m));
    m.Allocs[0].Slot = 1; m.ResourceFails = 1;
    CHECK(Bc250CreateRun(0, &g_ModelOps, &m, 1) == BC250_CREATE_NO_MEMORY && m.Frees == 1 && !m.Live[0]);

    // Exhaustive: every placement of a failure (refusal, object OOM, resource OOM) over 1..6 allocations of mixed
    // kinds. Invariants: no leak, no double free, reverse order, and per LB7A allocation exactly one outcome.
    {
        unsigned long count, fail, kind, done = 0;
        for (count = 0; count <= 6; count++) for (fail = 0; fail <= count; fail++) for (kind = 0; kind < 3; kind++) {
            unsigned long admittedLb7a = 0, lb7a = 0, total = 0, s;
            int outcome;
            if (fail == count && kind != 2) continue;      // past the end only the resource object can fail
            memset(&c, 0, sizeof(c)); memset(&m, 0, sizeof(m));
            for (i = 0; i < count; i++) m.Allocs[i].Slot = (i % 3 == 1) ? BC250_CREATE_NOT_LB7A : i % 9;
            if (fail < count) m.Allocs[fail].Step = kind == 0 ? BC250_CREATE_STEP_REFUSED : BC250_CREATE_STEP_NO_MEMORY;
            else m.ResourceFails = 1;
            for (i = 0; i < count && i < fail; i++) if (m.Allocs[i].Slot != BC250_CREATE_NOT_LB7A) admittedLb7a++;
            for (i = 0; i <= fail && i < count; i++) if (m.Allocs[i].Slot != BC250_CREATE_NOT_LB7A) lb7a++;
            outcome = RunModel(&m, &c, count);
            CHECK(outcome != BC250_CREATE_OK && !m.Leaked && !m.DoubleFree && m.Frees == (int)(fail < count ? fail : count));
            for (i = 1; i < m.FreeCount; i++) CHECK(m.FreeOrder[i] < m.FreeOrder[i - 1]);
            for (s = 0; s < BC250_GDI_SLOTS; s++) total += c.GdiCreated[s][0] + c.GdiCreated[s][1] + c.GdiCreated[s][2];
            CHECK(total == lb7a);
            for (s = 0, i = 0; s < BC250_GDI_SLOTS; s++) i += c.GdiCreated[s][BC250_CREATE_ROLLED_BACK];
            CHECK(i == admittedLb7a);
            CHECK(c.CreateCalls[outcome] == 1);
            done++;
        }
        printf("create rollback: %lu injected failures\n", done);
    }
}

static void Masks(void)
{
    BC250_STDALLOC_COUNTERS c;
    BC250_GDI_MASKS m;
    int i;
    memset(&c, 0, sizeof(c));
    Bc250GdiMasks(&c, &m);
    CHECK(!m.Requested && !m.Refused && !m.CreateRefused && !m.CreateRolledBack && !m.OpenRefused);
    c.GdiRequests[1][0] = 2; c.GdiAnswers[1][BC250_STDALLOC_OK] = 2;
    c.GdiRequests[6][1] = 1; c.GdiAnswers[6][BC250_STDALLOC_TYPE] = 1;
    c.GdiRequests[9][0] = 1; c.GdiAnswers[9][BC250_STDALLOC_INPUT] = 1;
    c.GdiCreated[2][0] = 1; c.GdiCreated[7][1] = 1; c.GdiCreated[3][2] = 1; c.GdiOpened[0][1] = 3;
    Bc250GdiMasks(&c, &m);
    CHECK(m.Requested == 0x242 && m.Refused == 0x240 && m.CreateRefused == 0x080 && m.CreateRolledBack == 0x008 &&
          m.OpenRefused == 0x001);
    for (i = 0; i < BC250_STDALLOC_STATUSES; i++) CHECK(g_Bc250StdAllocStatusNames[i] != 0);
}

// The size query must not change the union (d3dkmddi.md:32953): no pitch is published without a buffer.
static void SizeQueryLeavesUnion(void)
{
    unsigned long kind;
    for (kind = 1; kind <= 4; kind++) {
        BC250_STDALLOC_REQUEST r;
        BC250_STDALLOC_ANSWER a;
        memset(&r, 0, sizeof(r));
        r.Kind = kind; r.Described = 1; r.Width = 33; r.Height = 7; r.Format = FMT_A8R8G8B8; r.GdiType = kind == 4 ? 1 : 0;
        CHECK(Bc250StdAllocDecide(&r, &a) == BC250_STDALLOC_OK && !a.PublishPitch);
        CHECK(a.PrivateBytes == (kind == 4 ? 48ul : 32ul));
        r.Fill = 1; r.Bytes = a.PrivateBytes;
        CHECK(Bc250StdAllocDecide(&r, &a) == BC250_STDALLOC_OK);
        CHECK(a.PublishPitch == (kind != 1));       // the shared primary's union has no output pitch
        r.Bytes = a.PrivateBytes - 1;
        CHECK(Bc250StdAllocDecide(&r, &a) == BC250_STDALLOC_BUFFER && !a.PublishPitch);
    }
}

// Odd widths and heights: each type's own rounding, never the other's.
static void OddShapes(void)
{
    BC250_STDALLOC_REQUEST r;
    BC250_STDALLOC_ANSWER a;
    r = Gdi(1, 33, 7, FMT_A8R8G8B8, 1);     // shared texture: 256-byte pitch, four-row padding
    CHECK(Bc250StdAllocDecide(&r, &a) == BC250_STDALLOC_OK && a.Surface.Pitch == 256 && a.Surface.Size == 256 * 8);
    CHECK(a.Surface.Width == 33 && a.Surface.Height == 7);
    r = Gdi(2, 33, 7, FMT_A8, 1);           // CPU-visible staging at 1 byte a pixel: 4-byte rows, no row padding
    CHECK(Bc250StdAllocDecide(&r, &a) == BC250_STDALLOC_OK && a.Surface.Pitch == 36 && a.Surface.Size == 36 * 7);
    r = Gdi(3, 1, 1, FMT_A8R8G8B8, 1);
    CHECK(Bc250StdAllocDecide(&r, &a) == BC250_STDALLOC_OK && a.Surface.Pitch == 4 && a.Surface.Size == 4);
    r = Gdi(4, 257, 1, FMT_A8, 1);          // lookup table, A8 only
    CHECK(Bc250StdAllocDecide(&r, &a) == BC250_STDALLOC_OK && a.Surface.Pitch == 260 && a.Surface.Size == 260);
    r = Gdi(1, 0, 7, FMT_A8R8G8B8, 1);
    CHECK(Bc250StdAllocDecide(&r, &a) == BC250_STDALLOC_GEOMETRY);
    r = Gdi(1, 33, 0, FMT_A8R8G8B8, 1);
    CHECK(Bc250StdAllocDecide(&r, &a) == BC250_STDALLOC_GEOMETRY);
    memset(&r, 0, sizeof(r));
    r.Kind = BC250_STDALLOC_PRIMARY; r.Described = 1; r.Width = 33; r.Height = 7; r.Format = FMT_X8R8G8B8;
    CHECK(Bc250StdAllocDecide(&r, &a) == BC250_STDALLOC_OK && a.Surface.Pitch == 256 && a.Surface.Size == 256 * 7);
    r.Kind = BC250_STDALLOC_STAGING; r.Format = 0;  // staging is always A8R8G8B8, whatever arrives
    CHECK(Bc250StdAllocDecide(&r, &a) == BC250_STDALLOC_OK && a.Surface.Pitch == 132 && a.Surface.Format == FMT_A8R8G8B8);
}

int main(void)
{
    SameAsBefore();
    RefusalsCounted();
    ReceivedBlobs();
    CreateRollback();
    Masks();
    SizeQueryLeavesUnion();
    OddShapes();
    printf("gdi_admission_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
