// User mode only (installer worker). Transfer records of the state catalogs of earlier released packages
// (driver/state/history, catalog_history.generated.h). The KMD never reads such a record: the worker maps it into a
// record of the current catalog by setting name, as an update in place reads the generation of an earlier catalog.
#pragma once
#include "transfer.h"
#define AMDGPU_HISTORY_NONE 0xffffffffu
// The frame of a record of any catalog: magic, version, length and the checksum of transfer.c. OK or SCHEMA.
unsigned int AmdgpuHistoryFrame(const void *buffer,unsigned int bytes);
// A record of an earlier catalog. OK: *catalog names that catalog and every entry is in bounds. UNKNOWN: the catalog
// is not in the history (*catalog is AMDGPU_HISTORY_NONE). SCHEMA: a damaged frame or entry.
unsigned int AmdgpuHistoryCheck(const void *buffer,unsigned int bytes,unsigned int *catalog);
// The entry of the current spec in a record that AmdgpuHistoryCheck accepted, found by path and setting name. A name
// that the earlier catalog does not have is ABSENT. Unknown names of the earlier catalog are never returned.
unsigned int AmdgpuHistoryFind(unsigned int catalog,const void *buffer,unsigned int bytes,
    const AMDGPU_STATE_SPEC *spec,AMDGPU_STATE_VALUE *value,const void **data);
