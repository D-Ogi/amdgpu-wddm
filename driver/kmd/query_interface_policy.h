// Which DxgkDdiQueryInterface requests get a log line (BD-410 stage 1). Header only, with no WDK type, so that the
// host test (test/opm_policy_test.c) and the kernel (query_interface.c) run the same text.
//
// The purpose of the line is to show which interfaces dxgkrnl and other kernel clients ask this adapter for, in
// which version and size, and the answer. The same request can come again at each start and for each OPM client,
// so a line is written only for the first request of each kind: the same GUID, size, version and answer. At most
// QI_LOG_KINDS kinds are kept per driver load; the first request of a further kind writes one last line that says
// that later kinds are not logged. So one load writes at most QI_LOG_KINDS + 1 lines, in either flavour.
#pragma once

#define QI_LOG_KINDS 12u

#define QI_LOG_NONE     0  // a kind that is already logged, or a kind after the overflow line
#define QI_LOG_LINE     1  // write the line of this request
#define QI_LOG_OVERFLOW 2  // write the line that later kinds are not logged

typedef struct _QI_LOG_KIND {
    unsigned char Guid[16];
    unsigned Size, Version;
    long Status;
} QI_LOG_KIND;

typedef struct _QI_LOG {
    QI_LOG_KIND Kinds[QI_LOG_KINDS];
    unsigned Count;      // kinds kept
    unsigned Requests;   // every request, also the ones not logged
    unsigned Overflow;   // 1 after the overflow line
} QI_LOG;

// Record one request and say what to write. Guid is the 16 bytes of the requested GUID, or 0 for a request without
// a GUID (kept as 16 zero bytes).
static __inline int QiLogNote(QI_LOG* Log, const void* Guid, unsigned Size, unsigned Version, long Status)
{
    unsigned char key[16];
    unsigned i, j;
    const unsigned char* g = (const unsigned char*)Guid;

    for (j = 0; j < 16u; ++j) key[j] = g ? g[j] : 0;
    Log->Requests++;
    for (i = 0; i < Log->Count; ++i) {
        const QI_LOG_KIND* k = &Log->Kinds[i];
        int same = k->Size == Size && k->Version == Version && k->Status == Status;
        for (j = 0; same && j < 16u; ++j) same = k->Guid[j] == key[j];
        if (same) return QI_LOG_NONE;
    }
    if (Log->Count < QI_LOG_KINDS) {
        QI_LOG_KIND* k = &Log->Kinds[Log->Count++];
        for (j = 0; j < 16u; ++j) k->Guid[j] = key[j];
        k->Size = Size;
        k->Version = Version;
        k->Status = Status;
        return QI_LOG_LINE;
    }
    if (Log->Overflow) return QI_LOG_NONE;
    Log->Overflow = 1;
    return QI_LOG_OVERFLOW;
}
