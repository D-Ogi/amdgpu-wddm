/* C48/C49 host control: the ring-gap accounting (ring_gap.h) and the completion-report pairing
 * (notify_pairing.h). Both are pure decisions, so the whole mechanism this candidate instruments and fixes is
 * exercised here on a made-up clock, with no device, no lock and no lab.
 *
 * The numbers the cases are built from come from the offline analysis of RotTR sessions 418-420
 * (scratch/m15/etw/c48/c48-result.json): the healthy boundary is a median 21-22 us, the long class is >= 4 ms,
 * the stall median is 7969-8173 us, the VSync-ended share of the long class is 61-65 %, and the uniform chance
 * of the 300 us window at 59.95 Hz is 1.8 %.
 */
#include <stdio.h>
#include <string.h>
#include "ring_gap.h"
#include "notify_pairing.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

/* A 10 MHz QPC, which is what this platform reports, so one tick is 100 ns and a microsecond is 10 ticks. */
#define HZ 10000000ull
#define US(n) ((unsigned long long)(n) * 10ull)

int main(void)
{
    BC250_RING_GAP g;
    BC250_NOTIFY_PAIRING p;
    unsigned long long now, us;
    unsigned i;
    /* Kept for the PASS line: the cases after them reset g and p on purpose. */
    unsigned long window_gaps = 0, window_long = 0, window_vsync = 0, hop_deferred = 0, hop_same = 0;
    unsigned long long window_max = 0ull, window_vsync_ns = 0ull;

    /* ---- the bucket edges, including both ends of every interval ------------------------------------------ */
    CHECK(Bc250RingGapBucket(0) == 0 && Bc250RingGapBucket(15) == 0);
    CHECK(Bc250RingGapBucket(16) == 1 && Bc250RingGapBucket(21) == 1 && Bc250RingGapBucket(31) == 1);
    CHECK(Bc250RingGapBucket(32) == 2 && Bc250RingGapBucket(63) == 2);
    CHECK(Bc250RingGapBucket(64) == 3 && Bc250RingGapBucket(127) == 3);
    CHECK(Bc250RingGapBucket(128) == 4 && Bc250RingGapBucket(511) == 4);
    CHECK(Bc250RingGapBucket(512) == 5 && Bc250RingGapBucket(1999) == 5);
    CHECK(Bc250RingGapBucket(2000) == 6 && Bc250RingGapBucket(3999) == 6);
    CHECK(Bc250RingGapBucket(4000) == 7 && Bc250RingGapBucket(7999) == 7);
    CHECK(Bc250RingGapBucket(8000) == 8 && Bc250RingGapBucket(1000000) == 8);
    /* The long threshold is the edge of bucket 7, so the histogram and the long counter can never disagree. */
    CHECK(Bc250RingGapBucket(BC250_RING_GAP_LONG_US) == 7 && Bc250RingGapBucket(BC250_RING_GAP_LONG_US - 1) == 6);
    /* The edges are the one definition of the buckets, and the summary writes their names into its own format:
     * eight edges for nine buckets, strictly increasing, with 4000 among them. A ninth edge would silently drop
     * a bucket out of the two log lines. */
    CHECK(sizeof(g_Bc250RingGapEdgesUs) / sizeof(g_Bc250RingGapEdgesUs[0]) == BC250_RING_GAP_BUCKETS - 1u);
    for (i = 1; i < BC250_RING_GAP_BUCKETS - 1u; i++) CHECK(g_Bc250RingGapEdgesUs[i] > g_Bc250RingGapEdgesUs[i - 1]);
    CHECK(g_Bc250RingGapEdgesUs[6] == BC250_RING_GAP_LONG_US);

    /* ---- microseconds, and the two ways a clock can lie -------------------------------------------------- */
    CHECK(Bc250RingGapUs(0, US(1000), HZ) == 1000ull);
    CHECK(Bc250RingGapUs(US(5), US(3), HZ) == 0ull);     /* backwards: 0, never a huge unsigned difference */
    CHECK(Bc250RingGapUs(0, US(1000), 0ull) == 0ull);    /* no frequency read yet: 0, not a division fault */
    /* An interval of 19e12 ticks is 22 days at 10 MHz: times a million it would wrap 64 bits and land as a lie in
     * MaxUs and the top bucket. Above the threshold the divide goes first, and at these magnitudes it is exact. */
    CHECK(Bc250RingGapUs(0ull, 19000000000000ull, HZ) == 1900000000000ull);
    CHECK(Bc250RingGapUs(0ull, 18000000000000ull, HZ) == 1800000000000ull);   /* just under, multiply-first */

    /* ---- one healthy boundary: the node goes idle and a submit takes it back, 21 us later ----------------- */
    Bc250RingGapReset(&g);
    now = US(1000000);
    Bc250RingGapOpen(&g, now);
    us = Bc250RingGapClose(&g, now + US(21), HZ, 0ull);
    CHECK(us == 21ull && g.Gaps == 1 && g.TotalUs == 21ull && g.MaxUs == 21ull);
    CHECK(g.Histogram[1] == 1 && g.LongGaps == 0 && g.VsyncEndedGaps == 0 && g.Lost == 0);
    CHECK(g.IdleSinceQpc == 0ull);

    /* A submit onto a ring that is already busy closes nothing and counts nothing - it is the ordinary case of
     * a second packet queued behind the first, not a lost gap. */
    CHECK(Bc250RingGapClose(&g, now + US(30), HZ, 0ull) == 0ull);
    CHECK(g.Gaps == 1 && g.Lost == 0);

    /* ---- the retirement that is not the first one: the gap must keep its own start --------------------- */
    /* WddmGfxHeadLocked runs on every retirement, and a poll that finds the queue still empty must not restart
     * the clock: that would turn one 8 ms stall into a handful of short ones and hide the whole class. */
    Bc250RingGapReset(&g);
    Bc250RingGapOpen(&g, US(2000000));
    for (i = 1; i <= 8; i++) Bc250RingGapOpen(&g, US(2000000) + US(i * 500));
    us = Bc250RingGapClose(&g, US(2000000) + US(8000), HZ, 0ull);
    CHECK(us == 8000ull && g.Gaps == 1 && g.LongGaps == 1 && g.Histogram[8] == 1);

    /* ---- the VSync window, with both of its boundaries and both of its ways to miss ------------------- */
    /* The stall of sessions 418-420: 8.0 ms long, ending 97 us after a VSync report. */
    Bc250RingGapReset(&g);
    now = US(3000000);
    Bc250RingGapOpen(&g, now);
    us = Bc250RingGapClose(&g, now + US(8000), HZ, now + US(8000 - 97));
    CHECK(us == 8000ull && g.LongGaps == 1 && g.VsyncEndedGaps == 1 && g.VsyncEndedUs == 8000ull);
    /* Exactly on the edge of the window counts; one microsecond past it does not. */
    Bc250RingGapReset(&g);
    Bc250RingGapOpen(&g, now);
    (void)Bc250RingGapClose(&g, now + US(8000), HZ, now + US(8000) - US(BC250_RING_GAP_VSYNC_US));
    CHECK(g.VsyncEndedGaps == 1);
    Bc250RingGapReset(&g);
    Bc250RingGapOpen(&g, now);
    (void)Bc250RingGapClose(&g, now + US(8000), HZ, now + US(8000) - US(BC250_RING_GAP_VSYNC_US + 1));
    CHECK(g.LongGaps == 1 && g.VsyncEndedGaps == 0);
    /* A VSync older than the gap says only that the display is running. */
    Bc250RingGapReset(&g);
    Bc250RingGapOpen(&g, now);
    (void)Bc250RingGapClose(&g, now + US(8000), HZ, now - US(1));
    CHECK(g.LongGaps == 1 && g.VsyncEndedGaps == 0);
    /* A VSync after the gap's end cannot have ended it (the report arrives while the ring already runs). */
    Bc250RingGapReset(&g);
    Bc250RingGapOpen(&g, now);
    (void)Bc250RingGapClose(&g, now + US(8000), HZ, now + US(8001));
    CHECK(g.LongGaps == 1 && g.VsyncEndedGaps == 0);
    /* A short gap that happens to end after a VSync is NOT counted in the VSync-ended class: the class is the
     * >= 4 ms one of C45b/C47/C48, and mixing the 21 us boundaries into it would drown the signal in 1.8 %. */
    Bc250RingGapReset(&g);
    Bc250RingGapOpen(&g, now);
    (void)Bc250RingGapClose(&g, now + US(21), HZ, now + US(10));
    CHECK(g.Gaps == 1 && g.LongGaps == 0 && g.VsyncEndedGaps == 0);

    /* ---- a window the size of the lab's: the shape of 418 must come back out -------------------------- */
    /* 19694 boundaries in 105 s: 16593 at 21 us, 2082 at 137 us, 337 at 2569 us, 418 VSync-ended at 8112 us and
     * 264 long ones that no VSync ended. The gap set is driven in that proportion and the counters are read
     * against the numbers c48gaps.py printed from the trace. */
    Bc250RingGapReset(&g);
    now = US(10000000);
    for (i = 0; i < 16593; i++) { Bc250RingGapOpen(&g, now); now += US(21); (void)Bc250RingGapClose(&g, now, HZ, 0ull); now += US(500); }
    for (i = 0; i < 2082; i++) { Bc250RingGapOpen(&g, now); now += US(137); (void)Bc250RingGapClose(&g, now, HZ, 0ull); now += US(500); }
    for (i = 0; i < 337; i++) { Bc250RingGapOpen(&g, now); now += US(2569); (void)Bc250RingGapClose(&g, now, HZ, 0ull); now += US(500); }
    for (i = 0; i < 418; i++) {
        unsigned long long began = now;
        Bc250RingGapOpen(&g, began);
        now += US(8112);
        (void)Bc250RingGapClose(&g, now, HZ, now - US(97));
        now += US(500);
    }
    for (i = 0; i < 264; i++) { Bc250RingGapOpen(&g, now); now += US(7695); (void)Bc250RingGapClose(&g, now, HZ, 0ull); now += US(500); }
    CHECK(g.Gaps == 19694u && g.Lost == 0u);
    CHECK(g.Histogram[1] == 16593u && g.Histogram[4] == 2082u && g.Histogram[6] == 337u);
    /* 8112 us is in the 8k+ bucket and 7695 us is not, which is exactly why the edge sits at half a refresh
     * period: the VSync-ended class and the "nothing to run" class of the trace separate by length alone. */
    CHECK(g.Histogram[8] == 418u && g.Histogram[7] == 264u);
    CHECK(g.LongGaps == 682u && g.VsyncEndedGaps == 418u);
    CHECK(g.MaxUs == 8112ull);
    /* The idle this candidate is for: 418 x 8112 us over the window's 7350 frames (105 s at 70 Hz) is
     * 0.4613 ms a frame, which is the 0.454 the trace gave, to the frame count's own accuracy. */
    CHECK(g.VsyncEndedUs == 418ull * 8112ull);
    CHECK(Bc250RingGapPerFrameNs(g.VsyncEndedUs, 7350u) == 461335ull);
    CHECK(Bc250RingGapPerFrameNs(g.VsyncEndedUs, 0u) == 0ull);
    /* Not a single microsecond may be lost between the histogram and the sum. */
    {
        unsigned long counted = 0;
        for (i = 0; i < BC250_RING_GAP_BUCKETS; i++) counted += g.Histogram[i];
        CHECK(counted == g.Gaps);
    }

    window_gaps = g.Gaps; window_long = g.LongGaps; window_vsync = g.VsyncEndedGaps; window_max = g.MaxUs;
    window_vsync_ns = Bc250RingGapPerFrameNs(g.VsyncEndedUs, 7350u);

    /* ---- the losses that must stay impossible ------------------------------------------------------------ */
    Bc250RingGapReset(&g);
    Bc250RingGapOpen(&g, 0ull);                     /* 0 is the busy marker, not a time */
    CHECK(g.Lost == 1u && g.IdleSinceQpc == 0ull);
    Bc250RingGapOpen(&g, US(100));
    CHECK(Bc250RingGapClose(&g, US(99), HZ, 0ull) == 0ull);    /* the clock went backwards */
    CHECK(g.Lost == 2u && g.Gaps == 0u && g.IdleSinceQpc == 0ull);

    /* ---- the report pairing: the defect, one completion at a time -------------------------------------- */
    Bc250NotifyPairingReset(&p);
    Bc250NotifyPairingCompletion(&p, 0);
    CHECK(p.Reports == 1u && p.SamePass == 0u && p.Deferred == 1u && p.Unpaired == 0u);
    /* The idle notification is the one the device pass makes before anything has been reported. */
    CHECK(p.IdleNotifies == 1u);

    /* 187 completions a second, which is what session 418 actually retired (19694 boundaries in 105 s, one packet
     * per report in 100 % of them). 1268 a second is the SUBMIT rate of the same run and was the wrong number to
     * price this with. Every completion pays the hop, none is paired in its own pass, and the scheduler is handed
     * an unpaired report 187 times a second. */
    Bc250NotifyPairingReset(&p);
    for (i = 0; i < 187; i++) Bc250NotifyPairingCompletion(&p, 0);
    CHECK(p.Reports == 187u && p.Deferred == 187u && p.SamePass == 0u && p.MaxUnpaired == 1u);
    /* Two dxgkrnl device passes per completion with the gate closed: the one that read the fence and the one the
     * report's own DxgkCbQueueDpc brought back. That second round trip is what the gate removes. */
    CHECK(p.Pairings + p.IdleNotifies == 2u * 187u);

    hop_deferred = p.Deferred;

    /* ---- the fix: the pairing happens in the pass that reported ---------------------------------------- */
    Bc250NotifyPairingReset(&p);
    for (i = 0; i < 187; i++) Bc250NotifyPairingCompletion(&p, 1);
    CHECK(p.Reports == 187u && p.SamePass == 187u && p.Deferred == 0u && p.Unpaired == 0u);
    /* One device pass per completion now, and the report pass pairs itself: one notification fewer and, more to
     * the point, one whole Bc250DpcRoutine round trip through dxgkrnl fewer per completion. */
    CHECK(p.Pairings + p.IdleNotifies == 2u * 187u && p.IdleNotifies == 187u);
    hop_same = p.SamePass;
    /* Both nodes reporting in one pass are paired by one notification, not two. */
    Bc250NotifyPairingReset(&p);
    Bc250NotifyPairingReportPass(&p, 2u, 1);
    CHECK(p.Reports == 2u && p.Pairings == 1u && p.SamePass == 2u && p.Unpaired == 0u);
    /* A report pass with nothing to report must not call the notification at all. */
    Bc250NotifyPairingReset(&p);
    Bc250NotifyPairingReportPass(&p, 0u, 1);
    CHECK(p.Reports == 0u && p.Pairings == 0u && p.IdleNotifies == 0u);

    /* ---- the VSync reports, which are DxgkCbNotifyInterrupt calls too --------------------------------- */
    /* Counting only the completion path made "notifications carried no report" wrong by one per vblank, about 62
     * a second, and left the flip path's own pairing unwatched. They are counted apart because the two paths that
     * make one are a DPC of their own (the software timer) and the device pass itself (the hardware vblank). */
    Bc250NotifyPairingReset(&p);
    Bc250NotifyPairingVsyncReport(&p);              /* the software timer's report, from its own DPC */
    CHECK(p.VsyncReports == 1u && p.VsyncPending == 1u && p.MaxVsyncPending == 1u && p.Reports == 0u);
    Bc250NotifyPairingDevicePass(&p, 0);            /* the next dxgkrnl DPC pairs it */
    CHECK(p.VsyncPending == 0u && p.Pairings == 1u && p.IdleNotifies == 0u);
    CHECK(p.SamePass == 0u && p.Deferred == 0u);    /* a vblank report carries no same-pass classification */
    /* The hardware vblank: WddmDcnVsync reports inside the device pass, so that pass is not an idle notification. */
    Bc250NotifyPairingReset(&p);
    Bc250NotifyPairingDevicePass(&p, 1);
    CHECK(p.VsyncReports == 1u && p.VsyncPending == 0u && p.Pairings == 1u && p.IdleNotifies == 0u);
    /* A device pass that neither reported nor found anything waiting is the one that really is idle. */
    Bc250NotifyPairingDevicePass(&p, 0);
    CHECK(p.IdleNotifies == 1u);
    /* One notification pairs whatever is waiting of both kinds; the completion keeps its classification. */
    Bc250NotifyPairingReset(&p);
    Bc250NotifyPairingReport(&p);
    Bc250NotifyPairingVsyncReport(&p);
    CHECK(Bc250NotifyPairingNotifyDpc(&p, 0) == 1u);
    CHECK(p.Deferred == 1u && p.Unpaired == 0u && p.VsyncPending == 0u && p.Pairings == 1u);

    /* ---- a contended notification is deferred, never lost -------------------------------------------- */
    Bc250NotifyPairingReset(&p);
    Bc250NotifyPairingReport(&p);
    Bc250NotifyPairingContend(&p);                  /* another processor held DxgkCbNotifyDpc; we queued the DPC */
    CHECK(p.Contended == 1u && p.Unpaired == 1u && p.Pairings == 0u && p.IdleNotifies == 0u);
    Bc250NotifyPairingDevicePass(&p, 0);            /* which arrives and pairs it */
    CHECK(p.Unpaired == 0u && p.Deferred == 1u);

    /* ---- the shape that matters: the completion that opens a stall ------------------------------------- */
    /* Nothing of ours is queued after it, so with the gate closed the scheduler holds an unpaired report until
     * the next unrelated DPC - in sessions 418-420 the display's VSync, a median 8.1 ms later. With the gate
     * open there is nothing left to wait for. The stall itself is not caused by this: the scheduler woke 19 us
     * after the completion and refused (c48-result.json), which is why C48 died on its own clause 1. */
    Bc250NotifyPairingReset(&p);
    CHECK(Bc250NotifyPairingStallHead(&p, 0) != 0 && p.Unpaired == 1u);
    Bc250NotifyPairingDevicePass(&p, 0);            /* the VSync DPC, 8 ms later */
    CHECK(p.Deferred == 1u && p.Unpaired == 0u);
    Bc250NotifyPairingReset(&p);
    CHECK(Bc250NotifyPairingStallHead(&p, 1) == 0 && p.Unpaired == 0u && p.SamePass == 1u);

    printf("PASS: ring gap buckets/edges/vsync window/overflow, the 418 window reproduced (%lu gaps, %lu long, "
           "%lu vsync-ended, %llu us max, %llu ns a frame), report pairing over 187 completions: %lu deferred "
           "with the gate closed, %lu paired in their own pass with it open, vsync reports, contention and the "
           "stall head covered\n",
           window_gaps, window_long, window_vsync, window_max, window_vsync_ns, hop_deferred, hop_same);
    return 0;
}
