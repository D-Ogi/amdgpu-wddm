/* KMD172 host control: the pure decisions behind the drain quota and the hang detector (paging_drain.h,
 * progress.h). The actual drain against concurrent callers is exercised by the paging queue test
 * (experiments/E27-m9-inference/generate-paging-queue-test.py, run_paging_queue.ps1). */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "paging_drain.h"
#include "progress.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void)
{
    unsigned n;
    unsigned long long age;
    BC250_HANG_WATCH_STATE s;
    unsigned long long sec = 10000000ull, limit = 10 * sec;
    unsigned retired = 0;

    /* The drain helper's boundary: seven passes continue, the eighth retirement yields, no retirement returns. */
    for (n = 1; n < PAGING_DRAIN_QUOTA; n++) CHECK(PagingDrainNext(&retired, 1, PAGING_DRAIN_QUOTA) == PagingDrainContinue);
    CHECK(PagingDrainNext(&retired, 1, PAGING_DRAIN_QUOTA) == PagingDrainYield && retired == PAGING_DRAIN_QUOTA);
    retired = 0; CHECK(PagingDrainNext(&retired, 0, PAGING_DRAIN_QUOTA) == PagingDrainReturn && retired == 0);

    /* HangBugcheckSeconds: 5..60 taken as is, anything else is the default. */
    CHECK(Bc250HangSeconds(5) == 5 && Bc250HangSeconds(60) == 60 && Bc250HangSeconds(30) == 30);
    CHECK(Bc250HangSeconds(0) == 10 && Bc250HangSeconds(4) == 10 && Bc250HangSeconds(61) == 10);

    /* A heartbeat that keeps moving never fires, over 100 one-second checks. */
    memset(&s, 0, sizeof(s));
    for (n = 1; n <= 100; n++) CHECK(!Bc250HangWatchCheck(&s, (long)n, n * sec, limit, &age) && age == 0);
    /* Standing still: nine seconds pass, the tenth fires with the age of the stall. */
    for (n = 101; n < 110; n++) CHECK(!Bc250HangWatchCheck(&s, 100, n * sec, limit, &age));
    CHECK(Bc250HangWatchCheck(&s, 100, 110 * sec, limit, &age) && age == limit);
    /* A gap in the checks as long as the window is not stale time: a new window starts, the next limit fires. */
    memset(&s, 0, sizeof(s));
    CHECK(!Bc250HangWatchCheck(&s, 7, 1 * sec, limit, &age));
    CHECK(!Bc250HangWatchCheck(&s, 7, 30 * sec, limit, &age) && age == 0);
    for (n = 31; n < 40; n++) CHECK(!Bc250HangWatchCheck(&s, 7, n * sec, limit, &age));
    CHECK(Bc250HangWatchCheck(&s, 7, 40 * sec, limit, &age) && age == limit);

    /* The record is findable: the signature is the eight ASCII bytes, the sites fit the bugcheck's bitmask. */
    { unsigned long long sig = BC250_PROGRESS_SIGNATURE; CHECK(memcmp(&sig, "BC250PRG", 8) == 0); }
    CHECK(ProgressSiteCount <= 32);
    /* Layout 2: six longs then two times, 8-byte aligned with no implicit padding: 40 bytes a site. */
    CHECK(sizeof(BC250_PROGRESS_SITE) == 40 && offsetof(BC250_PROGRESS_SITE, LastEntryTime) == 24);
    CHECK(BC250_PROGRESS_VERSION == 2);
    printf("PASS: drain quota helper boundary, seconds range, heartbeat moving/stale/gap, signature (record %u bytes)\n",
           (unsigned)sizeof(BC250_PROGRESS));
    return 0;
}
