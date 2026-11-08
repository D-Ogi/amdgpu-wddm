// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
#pragma once
#include "gdi_private.h"
typedef struct BC250_MPO_STORAGE {
    unsigned long long Serial;
    unsigned UmdAlloc, ScanoutRequested;
    BC250_WDDM_ALLOCATION_PRIVATE Surface;
    BC250_WDDM_NV12_PRIVATE Nv12;
} BC250_MPO_STORAGE;
