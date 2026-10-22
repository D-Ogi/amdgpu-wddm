/* Host test for driver\kmd\flip_state.h: every edge of the BD-170 flip state machine, every writer decision and
 * the drain record (docs/design/vidpn-flip.md section 7.2). The header is included as it ships; run_flip_state.ps1
 * also compiles this test against copies of it with one rule broken, and requires a FAIL line from each. */
#include <stdio.h>
#include "flip_state.h"

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static BC250_FLIP_FACTS Facts(unsigned faulting, unsigned odd, unsigned writer, unsigned admitted, unsigned immediate,
                              unsigned drained, unsigned resumed)
{
    BC250_FLIP_FACTS f;
    f.Faulting = faulting; f.GenerationOdd = odd; f.WriterActive = writer; f.Admitted = admitted;
    f.Immediate = immediate; f.Drained = drained; f.Resumed = resumed;
    return f;
}

/* Every combination of the seven facts against the order of section 7.2, written out once more here. */
static unsigned Expected(const BC250_FLIP_FACTS* f)
{
    if (f->Faulting) return BC250_FLIP_EDGE_FAULTING;
    if (f->GenerationOdd && !f->WriterActive) return BC250_FLIP_EDGE_OVERLAP;
    if (!f->GenerationOdd && f->WriterActive) return BC250_FLIP_EDGE_OVERLAP;
    if (!f->Admitted) return BC250_FLIP_EDGE_REFUSED;
    if (f->WriterActive && !f->Immediate) return BC250_FLIP_EDGE_DEPOSIT_VSYNC;
    if (f->WriterActive && f->Drained && !f->Resumed) return BC250_FLIP_EDGE_DEPOSIT_DRAINED;
    if (f->WriterActive) return BC250_FLIP_EDGE_DEPOSIT_UNPROVEN;
    return BC250_FLIP_EDGE_PROGRAM;
}

static void Edges(void)
{
    unsigned bits, seen[BC250_FLIP_EDGES] = {0}, e;
    BC250_FLIP_FACTS f;
    for (bits = 0; bits < 128; bits++) {
        f = Facts(bits & 1, (bits >> 1) & 1, (bits >> 2) & 1, (bits >> 3) & 1, (bits >> 4) & 1, (bits >> 5) & 1,
                  (bits >> 6) & 1);
        e = Bc250FlipEdge(&f);
        CHECK(e < BC250_FLIP_EDGES && e == Expected(&f));
        if (e < BC250_FLIP_EDGES) seen[e]++;
    }
    for (e = 0; e < BC250_FLIP_EDGES; e++) CHECK(seen[e] > 0);
    /* The named cases, one line each. F0 wins over everything, a writer's deposit included. */
    f = Facts(1, 1, 1, 1, 0, 1, 0); CHECK(Bc250FlipEdge(&f) == BC250_FLIP_EDGE_FAULTING);
    f = Facts(1, 0, 0, 0, 1, 0, 0); CHECK(Bc250FlipEdge(&f) == BC250_FLIP_EDGE_FAULTING);
    /* F1 both ways, ahead of the admission: a refused request in an overlap is still the overlap. */
    f = Facts(0, 1, 0, 1, 1, 0, 0); CHECK(Bc250FlipEdge(&f) == BC250_FLIP_EDGE_OVERLAP);
    f = Facts(0, 0, 1, 1, 0, 0, 0); CHECK(Bc250FlipEdge(&f) == BC250_FLIP_EDGE_OVERLAP);
    f = Facts(0, 1, 0, 0, 0, 0, 0); CHECK(Bc250FlipEdge(&f) == BC250_FLIP_EDGE_OVERLAP);
    /* A ahead of W: a refused request during a writer deposits nothing. */
    f = Facts(0, 1, 1, 0, 0, 1, 0); CHECK(Bc250FlipEdge(&f) == BC250_FLIP_EDGE_REFUSED);
    f = Facts(0, 0, 0, 0, 1, 0, 0); CHECK(Bc250FlipEdge(&f) == BC250_FLIP_EDGE_REFUSED);
    /* W-v whatever the plane: a next-VSync request waits for a report that End's programming makes. */
    f = Facts(0, 1, 1, 1, 0, 0, 1); CHECK(Bc250FlipEdge(&f) == BC250_FLIP_EDGE_DEPOSIT_VSYNC);
    /* W-i only with DRAINED and before Resume; after Resume, or with the plane not proven drained, W-u. */
    f = Facts(0, 1, 1, 1, 1, 1, 0); CHECK(Bc250FlipEdge(&f) == BC250_FLIP_EDGE_DEPOSIT_DRAINED);
    f = Facts(0, 1, 1, 1, 1, 1, 1); CHECK(Bc250FlipEdge(&f) == BC250_FLIP_EDGE_DEPOSIT_UNPROVEN);
    f = Facts(0, 1, 1, 1, 1, 0, 0); CHECK(Bc250FlipEdge(&f) == BC250_FLIP_EDGE_DEPOSIT_UNPROVEN);
    /* E: no writer, an even generation, admitted. The plane's state does not matter here. */
    f = Facts(0, 0, 0, 1, 1, 0, 0); CHECK(Bc250FlipEdge(&f) == BC250_FLIP_EDGE_PROGRAM);
    f = Facts(0, 0, 0, 1, 0, 1, 1); CHECK(Bc250FlipEdge(&f) == BC250_FLIP_EDGE_PROGRAM);
}

static void Drained(void)
{
    unsigned bits;
    for (bits = 0; bits < 16; bits++)
        CHECK(Bc250FlipDrained(bits & 1, (bits >> 1) & 1, (bits >> 2) & 1, (bits >> 3) & 1) == (unsigned)(bits == 15));
}

static void Results(void)
{
    unsigned bits;
    for (bits = 0; bits < 16; bits++) {
        const unsigned wrote = bits & 1, ok = (bits >> 1) & 1, immediate = (bits >> 2) & 1, proof = (bits >> 3) & 1;
        const unsigned r = Bc250FlipResult(wrote, ok, immediate, proof);
        if (!wrote) CHECK(r == BC250_FLIP_RESULT_REFUSED);
        else if (!ok) CHECK(r == BC250_FLIP_RESULT_FAULT_PROGRAM);
        else if (immediate && !proof) CHECK(r == BC250_FLIP_RESULT_UNPROVEN);
        else CHECK(r == BC250_FLIP_RESULT_SUCCESS);
    }
    /* A next-VSync request needs no proof in the call: its report completes it. */
    CHECK(Bc250FlipResult(1, 1, 0, 0) == BC250_FLIP_RESULT_SUCCESS);
    /* A failed write sequence is a fault even when P' would have held. */
    CHECK(Bc250FlipResult(1, 0, 1, 1) == BC250_FLIP_RESULT_FAULT_PROGRAM);
}

static void Writers(void)
{
    int otg;
    CHECK(Bc250WriterBegin(0, 0) == BC250_WRITER_BEGIN_TAKE);
    CHECK(Bc250WriterBegin(0, 1) == BC250_WRITER_BEGIN_RETRY);
    CHECK(Bc250WriterBegin(1, 0) == BC250_WRITER_BEGIN_FAULTING);
    CHECK(Bc250WriterBegin(1, 1) == BC250_WRITER_BEGIN_FAULTING);
    for (otg = -2; otg <= 2; otg++) {
        CHECK(Bc250WriterEnd(0, 0, otg) == BC250_WRITER_END_NOTHING);
        CHECK(Bc250WriterEnd(1, 0, otg) == (unsigned)(otg == 1 ? BC250_WRITER_END_PROGRAM : BC250_WRITER_END_FAULT));
        CHECK(Bc250WriterEnd(0, 1, otg) == (unsigned)(otg == 1 ? BC250_WRITER_END_PROGRAM : BC250_WRITER_END_FAULT));
        CHECK(Bc250WriterEnd(1, 1, otg) == (unsigned)(otg == 1 ? BC250_WRITER_END_PROGRAM : BC250_WRITER_END_FAULT));
    }
}

/* How a W-u deposit left its writer: every combination against the order of Bc250WuEnd, written out again. */
static unsigned ExpectedWu(unsigned unblank, unsigned current, unsigned answer, unsigned result)
{
    if (unblank) return BC250_WU_UNBLANK;
    if (!current) return BC250_WU_SUPERSEDED;
    if (answer == BC250_WRITER_END_FAULT) return BC250_WU_END_FAULT;
    if (answer == BC250_WRITER_END_NOTHING) return BC250_WU_DROPPED;
    if (result == BC250_FLIP_RESULT_SUCCESS) return BC250_WU_END_PROGRAM;
    if (result == BC250_FLIP_RESULT_REFUSED) return BC250_WU_END_REFUSED;
    return BC250_WU_END_FAULT;
}

static void WuEnds(void)
{
    unsigned unblank, current, answer, result, e, seen[BC250_WU_ENDS] = {0};
    for (unblank = 0; unblank < 2; unblank++)
        for (current = 0; current < 2; current++)
            for (answer = 0; answer < BC250_WRITER_ENDS; answer++)
                for (result = 0; result < BC250_FLIP_RESULTS; result++) {
                    e = Bc250WuEnd(unblank, current, answer, result);
                    CHECK(e < BC250_WU_ENDS && e == ExpectedWu(unblank, current, answer, result));
                    if (e < BC250_WU_ENDS) seen[e]++;
                }
    /* Every end but OPEN, which only an unended writer leaves, is reachable. */
    CHECK(seen[BC250_WU_OPEN] == 0);
    for (e = 1; e < BC250_WU_ENDS; e++) CHECK(seen[e] > 0);
    /* The named cases. The unblank showed it, whatever came after. */
    CHECK(Bc250WuEnd(1, 0, BC250_WRITER_END_PROGRAM, BC250_FLIP_RESULT_SUCCESS) == BC250_WU_UNBLANK);
    /* A newer deposit is what End programs, not this one. */
    CHECK(Bc250WuEnd(0, 0, BC250_WRITER_END_PROGRAM, BC250_FLIP_RESULT_SUCCESS) == BC250_WU_SUPERSEDED);
    CHECK(Bc250WuEnd(0, 1, BC250_WRITER_END_PROGRAM, BC250_FLIP_RESULT_SUCCESS) == BC250_WU_END_PROGRAM);
    CHECK(Bc250WuEnd(0, 1, BC250_WRITER_END_PROGRAM, BC250_FLIP_RESULT_REFUSED) == BC250_WU_END_REFUSED);
    CHECK(Bc250WuEnd(0, 1, BC250_WRITER_END_PROGRAM, BC250_FLIP_RESULT_FAULT_PROGRAM) == BC250_WU_END_FAULT);
    /* The OTG stopped at End: End's own E5 (BC250_FLIP_FAULT_OWED_STOPPED), never the W-u record's code. */
    CHECK(Bc250WuEnd(0, 1, BC250_WRITER_END_FAULT, BC250_FLIP_RESULT_REFUSED) == BC250_WU_END_FAULT);
    /* Nothing owed at End: the destroy restore took Desired away, or FAULTING. */
    CHECK(Bc250WuEnd(0, 1, BC250_WRITER_END_NOTHING, BC250_FLIP_RESULT_SUCCESS) == BC250_WU_DROPPED);
}

/* Ten writers and no writer; the W-u record's code stays 0x170005, the code an earlier r6 gave W-u as a fault. */
typedef char ProductWriterIdsStable[(BC250_WRITER_NONE == 0 && BC250_WRITER_DESTROY == 10) ? 1 : -1];
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
typedef char LabWriterCount[(BC250_WRITER_LATCH_PROBE == 11 && BC250_WRITER_KINDS == 12) ? 1 : -1];
#else
typedef char ReleaseWriterCount[(BC250_WRITER_KINDS == 11) ? 1 : -1];
#endif
typedef char WuCodeIs170005[(BC250_FLIP_RECORD_WRITER_UNPROVEN == 0x170005u) ? 1 : -1];

/* The bound of section 7.2, E5 step 5. */
typedef char DrainReadsAre50[(BC250_FLIP_DRAIN_READS == 50u) ? 1 : -1];

static void Drain(void)
{
    unsigned reads;
    CHECK(Bc250FlipDrainStep(0, 1, 1) == BC250_FLIP_DRAIN_UNKNOWN);
    CHECK(Bc250FlipDrainStep(1, 1, BC250_FLIP_DRAIN_READS) == BC250_FLIP_DRAIN_DRAINED);
    for (reads = 1; reads < BC250_FLIP_DRAIN_READS; reads++)
        CHECK(Bc250FlipDrainStep(1, 0, reads) == BC250_FLIP_DRAIN_AGAIN);
    CHECK(Bc250FlipDrainStep(1, 0, BC250_FLIP_DRAIN_READS) == BC250_FLIP_DRAIN_UNKNOWN);
}

static void Codes(void)
{
    /* Six E5 codes and the W-u record's code, distinct, each inside DXGK_FAULT_ERROR_CODE.DeviceSpecificCode's 31
     * bits. */
    static const unsigned codes[] = { BC250_FLIP_FAULT_PROGRAM, BC250_FLIP_FAULT_UNPROVEN, BC250_FLIP_FAULT_OWED_STOPPED,
                                      BC250_FLIP_FAULT_OVERLAP, BC250_FLIP_FAULT_SECTION, BC250_FLIP_FAULT_BRIDGE,
                                      BC250_FLIP_RECORD_WRITER_UNPROVEN };
    unsigned i, j;
    for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        CHECK(codes[i] != 0 && codes[i] < 0x80000000u);
        for (j = i + 1; j < sizeof(codes) / sizeof(codes[0]); j++) CHECK(codes[i] != codes[j]);
    }
}

int main(void)
{
    Edges();
    Drained();
    Results();
    Writers();
    WuEnds();
    Drain();
    Codes();
    printf("flip state: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
