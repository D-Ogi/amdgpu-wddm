/* ring_gap.h - the node's own idle-gap accounting, as pure decisions over a QPC timeline.
 *
 * Why it exists (C45b, C47, C48 on RotTR sessions 418-420): the 3D ring of this part stands idle for a median
 * 8 ms, 418 to 605 times in 105 s, while a dispatchable DMA packet is already queued, and the gap ends within
 * 300 us of a display VSync. That is 0.45 to 0.67 ms of a 70 Hz frame given away, and 59 to 64 % of all ring
 * idle in the window. Reading it took a 3 GB xperf text dump per session and three days of offline analysis.
 * This header is the same number, computed by the driver that owns the ring, for every workload, with two QPC
 * reads per packet boundary and two lines in the log summary. The owner's goal is that the GPU must not wait;
 * a driver that cannot say how long its ring waited cannot work towards it.
 *
 * WHAT THIS OBJECT IS NOT. The driver sees its own ring: from the submit that installs the hardware-pending state
 * to the fence read that retires the last job. It cannot see a packet that dxgkrnl holds queued and refuses to
 * dispatch, which is exactly what defined the trace's class (95.4-96.9 % of each stall was dispatchable idle).
 * So this histogram is a SUPERSET of that class: it mixes it with ordinary application-starved idle, and only the
 * VSync-ended filter separates the two, against the 1.8 % a uniform distribution would give. Its edges also sit
 * one DPC later than the trace's (the gap opens where the DPC observes the fence, not at the fence interrupt), so
 * every length here is a lower bound by that latency. Read the two numbers side by side, never as the same number.
 *
 * The definition is otherwise deliberately the same object the ETW analysis used:
 *   - the node is BUSY while at least one of our DMA packets is in flight on it (dxgkrnl's DmaPacket Start..Stop,
 *     which for this driver is the interval from the submit that installs the hardware-pending state to the
 *     fence read that retires the last job of the queue);
 *   - a GAP is one maximal idle interval between two busy intervals;
 *   - a gap is VSYNC-ENDED when a VSync report of ours falls inside it and no more than
 *     BC250_RING_GAP_VSYNC_US before its end. The window is 300 us, which at 59.95 Hz is 1.8 % of the refresh
 *     period: every measured share is read against that 1.8 %, never against a remembered per cent.
 *
 * Everything here is integer arithmetic on microseconds, with no kernel call, no lock and no floating point, so
 * the caller can use it inside its own lock and the host test can drive it with a made-up clock.
 */
#ifndef BC250_RING_GAP_H
#define BC250_RING_GAP_H

/* The bucket edges in microseconds. 16 and 32 bracket one packet boundary of a healthy ring (the measured FAST
 * class is a median 21-22 us); 4k is the threshold of the long-stall class every C4x round is about; 8k is half
 * a refresh period at 60 Hz, where the VSync-ended class piles up. Nine buckets, named, so a reader of the log
 * needs neither this file nor the edges. */
#define BC250_RING_GAP_BUCKETS 9u
#define BC250_RING_GAP_LONG_US 4000u    /* "long" is the >= 4 ms class of C45b/C47/C48 */
#define BC250_RING_GAP_VSYNC_US 300u    /* the phase window; 1.8 % of a 59.95 Hz period */

/* A gap of exactly an edge belongs to the bucket above it, so the names read as "up to". */
static const unsigned long g_Bc250RingGapEdgesUs[BC250_RING_GAP_BUCKETS - 1u] = {
    16u, 32u, 64u, 128u, 512u, 2000u, 4000u, 8000u
};

static __inline unsigned long Bc250RingGapBucket(unsigned long long Microseconds)
{
    unsigned long i;

    for (i = 0; i < BC250_RING_GAP_BUCKETS - 1u; i++)
        if (Microseconds < (unsigned long long)g_Bc250RingGapEdgesUs[i]) return i;
    return BC250_RING_GAP_BUCKETS - 1u;
}

/* One node's accumulated state. All of it is evidence, none of it is control flow, so a reader that races a
 * writer reads a number one boundary old and nothing worse. Counters are 64-bit where a 105 s window can
 * overflow 32 bits of microseconds (7.5 s of idle is 7.5e6 us, so the sum needs the width). */
typedef struct _BC250_RING_GAP {
    unsigned long long IdleSinceQpc;    /* QPC at which the node went idle; 0 = the node is busy */
    unsigned long long LongestQpc;      /* QPC of the end of the longest gap, for a log line that can be placed */
    unsigned long long TotalUs;         /* sum of every closed gap */
    unsigned long long LongUs;          /* of it, the >= 4 ms class */
    unsigned long long VsyncEndedUs;    /* of the >= 4 ms class, the VSync-ended part */
    unsigned long long MaxUs;
    unsigned long Gaps;
    unsigned long LongGaps;
    unsigned long VsyncEndedGaps;
    unsigned long Histogram[BC250_RING_GAP_BUCKETS];
    unsigned long Lost;                 /* closes with no open gap, or a clock that went backwards: must stay 0 */
} BC250_RING_GAP;

/* Microseconds between two QPC values, with the frequency the caller read once. A frequency of 0 (nothing read
 * it yet) and a counter that went backwards both give 0, never a huge number: a wrong bucket is a lie that
 * survives into the summary, and 0 is visibly nothing. The multiply comes first for the precision, but only while
 * it fits: at a 10 MHz counter 1.8e13 ticks is 21 days, and an interval that long would otherwise wrap and land
 * as fiction in MaxUs and the top bucket, so above that the divide goes first and loses sub-second precision
 * nobody reading a 21-day gap cares about. */
static __inline unsigned long long Bc250RingGapUs(unsigned long long From, unsigned long long To,
                                                  unsigned long long Frequency)
{
    unsigned long long ticks;

    if (Frequency == 0ull || To <= From) return 0ull;
    ticks = To - From;
    if (ticks > (~0ull / 1000000ull)) return (ticks / Frequency) * 1000000ull;
    return (ticks * 1000000ull) / Frequency;
}

static __inline void Bc250RingGapReset(BC250_RING_GAP* Gap)
{
    unsigned long i;

    Gap->IdleSinceQpc = 0ull;
    Gap->LongestQpc = 0ull;
    Gap->TotalUs = Gap->LongUs = Gap->VsyncEndedUs = Gap->MaxUs = 0ull;
    Gap->Gaps = Gap->LongGaps = Gap->VsyncEndedGaps = Gap->Lost = 0u;
    for (i = 0; i < BC250_RING_GAP_BUCKETS; i++) Gap->Histogram[i] = 0u;
}

/* The node went idle: the last packet in flight retired at Qpc. Called on every retirement, so the second call
 * of an already idle node must NOT move the start - that would silently shorten every gap that contains a
 * retirement of another node or a repeated poll. A QPC of 0 is refused, because 0 is the "busy" marker. */
static __inline void Bc250RingGapOpen(BC250_RING_GAP* Gap, unsigned long long Qpc)
{
    if (Qpc == 0ull) { Gap->Lost++; return; }
    if (Gap->IdleSinceQpc == 0ull) Gap->IdleSinceQpc = Qpc;
}

/* The node went busy: a submit installed a packet at Qpc. LastVsyncQpc is the QPC of the newest VSync report of
 * the adapter (0 when none was seen yet). Returns the length of the gap just closed in microseconds, 0 when
 * there was no open gap - which is the ordinary case for a submit onto an already busy ring and is not counted
 * as a loss. */
static __inline unsigned long long Bc250RingGapClose(BC250_RING_GAP* Gap, unsigned long long Qpc,
                                                     unsigned long long Frequency, unsigned long long LastVsyncQpc)
{
    unsigned long long began = Gap->IdleSinceQpc;
    unsigned long long us;

    if (began == 0ull) return 0ull;             /* already busy: nothing to close */
    Gap->IdleSinceQpc = 0ull;
    if (Qpc < began) { Gap->Lost++; return 0ull; }   /* the clock went backwards: refuse rather than invent */
    us = Bc250RingGapUs(began, Qpc, Frequency);
    Gap->Gaps++;
    Gap->TotalUs += us;
    Gap->Histogram[Bc250RingGapBucket(us)]++;
    if (us > Gap->MaxUs) { Gap->MaxUs = us; Gap->LongestQpc = Qpc; }
    if (us >= (unsigned long long)BC250_RING_GAP_LONG_US) {
        Gap->LongGaps++;
        Gap->LongUs += us;
        /* The VSync has to be inside this gap and close to its end. A report older than the gap says only that
         * the display is running; one more than the window before the end means the gap outlived it. */
        if (LastVsyncQpc >= began && LastVsyncQpc <= Qpc &&
            Bc250RingGapUs(LastVsyncQpc, Qpc, Frequency) <= (unsigned long long)BC250_RING_GAP_VSYNC_US) {
            Gap->VsyncEndedGaps++;
            Gap->VsyncEndedUs += us;
        }
    }
    return us;
}

/* The two numbers the owner's goal is stated in, per displayed frame, in nanoseconds so that integer arithmetic
 * is enough (a lab frame is 14 ms, and 0.45 ms a frame is 450000 ns). Frames is the VSync report count of the
 * same window; 0 frames gives 0, never a division fault. */
static __inline unsigned long long Bc250RingGapPerFrameNs(unsigned long long Microseconds, unsigned long Frames)
{
    if (Frames == 0u) return 0ull;
    return (Microseconds * 1000ull) / (unsigned long long)Frames;
}

/* The bucket names are not a table here on purpose. The summary writes them into its own GuardLog format
 * ("<16 16 32 64 128" and "512 2k 4k 8k+"), because a %s costs 32 characters of the 159 a log line holds and
 * nine of them would not fit; g_Bc250RingGapEdgesUs above is the one definition of where the edges are, and the
 * host test checks that the two agree in count. A table nobody reads would also be a dead symbol in the driver.
 */

#endif /* BC250_RING_GAP_H */
