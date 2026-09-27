// Host-only test of redirblt_policy.h: no window, no DWM, no D3DKMT. build.ps1 runs it before the probe is
// built; a failing case fails the build.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "redirblt_policy.h"

static int g_Failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); g_Failures++; } } while (0)

int main(void)
{
    const char* why = "";
    REDIRBLT_HANDLE_SET set;
    HANDLE h1 = (HANDLE)(ULONG_PTR)0x1234, h2 = (HANDLE)(ULONG_PTR)0x5678;

    // 1. pending worker
    CHECK(RedirbltAfterWait(WAIT_OBJECT_0, 0, &why) == WaitProceed);
    CHECK(RedirbltAfterWait(WAIT_TIMEOUT, 0, &why) == WaitTerminate);
    CHECK(RedirbltAfterWait(WAIT_FAILED, 6, &why) == WaitTerminate && strstr(why, "error") != NULL);
    CHECK(RedirbltAfterWait(WAIT_ABANDONED, 0, &why) == WaitTerminate);

    // 2. ordinal 101 only after S_OK
    CHECK(RedirbltUpdateAllowed(TRUE, REDIRBLT_HR_S_OK, &why));
    CHECK(!RedirbltUpdateAllowed(TRUE, REDIRBLT_HR_GDI_SURFACE, &why));
    CHECK(!RedirbltUpdateAllowed(TRUE, REDIRBLT_HR_BLT_VIA_GDI, &why));
    CHECK(!RedirbltUpdateAllowed(FALSE, REDIRBLT_HR_S_OK, &why));

    // 3. destination readiness
    CHECK(RedirbltDestinationReady(TRUE, TRUE, TRUE, TRUE, TRUE, &why));
    CHECK(!RedirbltDestinationReady(FALSE, TRUE, TRUE, TRUE, TRUE, &why));
    CHECK(!RedirbltDestinationReady(TRUE, FALSE, TRUE, TRUE, TRUE, &why));       // wrong LUID
    CHECK(!RedirbltDestinationReady(TRUE, TRUE, FALSE, TRUE, TRUE, &why));       // unknown geometry
    CHECK(!RedirbltDestinationReady(TRUE, TRUE, TRUE, FALSE, TRUE, &why));       // map failed
    CHECK(!RedirbltDestinationReady(TRUE, TRUE, TRUE, TRUE, FALSE, &why));       // residency failed

    // 4. variants
    CHECK(RedirbltVariantAllowed(VariantV0, FALSE, 0, FALSE, FALSE, FALSE, &why));
    CHECK(RedirbltVariantAllowed(VariantA, TRUE, REDIRBLT_HR_GDI_SURFACE, FALSE, FALSE, FALSE, &why));
    CHECK(!RedirbltVariantAllowed(VariantA, TRUE, REDIRBLT_HR_S_OK, FALSE, TRUE, TRUE, &why));
    CHECK(!RedirbltVariantAllowed(VariantA, TRUE, REDIRBLT_HR_BLT_VIA_GDI, FALSE, TRUE, TRUE, &why));
    CHECK(!RedirbltVariantAllowed(VariantA, FALSE, REDIRBLT_HR_GDI_SURFACE, FALSE, TRUE, TRUE, &why));
    CHECK(RedirbltVariantAllowed(VariantB, TRUE, REDIRBLT_HR_GDI_SURFACE, FALSE, TRUE, TRUE, &why));
    CHECK(!RedirbltVariantAllowed(VariantB, TRUE, REDIRBLT_HR_GDI_SURFACE, TRUE, TRUE, TRUE, &why));    // --no-open
    CHECK(!RedirbltVariantAllowed(VariantC, TRUE, REDIRBLT_HR_GDI_SURFACE, FALSE, FALSE, TRUE, &why));  // not ready
    CHECK(!RedirbltVariantAllowed(VariantD, TRUE, REDIRBLT_HR_GDI_SURFACE, FALSE, TRUE, FALSE, &why));  // other handle

    // 5. destination after a fresh handshake: keep, rebind or reopen
    CHECK(RedirbltDestinationAction(FALSE, NULL, 0, h1, 7, &why) == DestinationReopen);        // nothing opened
    CHECK(RedirbltDestinationAction(TRUE, h1, 7, h1, 7, &why) == DestinationKeep);             // same handle, same id
    CHECK(RedirbltDestinationAction(TRUE, h1, 7, h2, 7, &why) == DestinationReopen);           // changed handle
    CHECK(RedirbltDestinationAction(TRUE, h1, 7, h2, 8, &why) == DestinationReopen);           // changed handle and id
    CHECK(RedirbltDestinationAction(TRUE, h1, 7, h1, 8, &why) == DestinationRebind);           // same handle, new id (238)
    CHECK(RedirbltDestinationBound(TRUE, h1, 7, h1, 7));
    CHECK(!RedirbltDestinationBound(TRUE, h1, 7, h1, 8));                                      // not bound before the rebind
    CHECK(RedirbltDestinationBound(TRUE, h1, 8, h1, 8));                                       // bound after adopting id 8
    CHECK(!RedirbltDestinationBound(TRUE, h1, 8, h2, 8));
    CHECK(!RedirbltDestinationBound(FALSE, h1, 8, h1, 8));
    // a run of handshakes on one surface: id 7, 8, 9 must each end bound without a reopen
    {
        UINT64 boundId = 7, id;
        for (id = 7; id <= 9; id++)
        {
            REDIRBLT_DESTINATION_ACTION action = RedirbltDestinationAction(TRUE, h1, boundId, h1, id, &why);
            CHECK(action != DestinationReopen);
            if (action == DestinationRebind) boundId = id;
            CHECK(RedirbltDestinationBound(TRUE, h1, boundId, h1, id));
        }
    }

    // 6. handle ownership
    ZeroMemory(&set, sizeof(set));
    CHECK(!RedirbltHandleSetAdd(&set, h1, HandleUnknown));
    CHECK(!RedirbltHandleSetAdd(&set, h1, HandleGlobal));
    CHECK(RedirbltHandleSetAdd(&set, h1, HandleNt));
    CHECK(!RedirbltHandleSetAdd(&set, h1, HandleNt));                            // repeated handle, one close
    CHECK(RedirbltHandleSetAdd(&set, h2, HandleNt));
    CHECK(set.Count == 2);
    CHECK(!RedirbltHandleSetAdd(&set, NULL, HandleNt));

    // 7. --handshake-only scope
    CHECK(!RedirbltNeedDevice(TRUE, TRUE));
    CHECK(RedirbltNeedDevice(TRUE, FALSE));
    CHECK(RedirbltNeedDevice(FALSE, TRUE));
    CHECK(!RedirbltNeedSourceAndContext(TRUE));
    CHECK(RedirbltNeedSourceAndContext(FALSE));

    printf("redirblt policy test: %d failure(s)\n", g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
