#pragma once
#include <windows.h>
// Installer worker only. Requires LocalSystem; fixed DriverData hierarchy.
// Existing pending content must be identical. No overwrite of a different job.
DWORD AmdgpuWindowsPublishHandoff(const void *buffer,DWORD bytes);
// All operations require LocalSystem, retain protected ancestor handles and use
// fixed names. The caller frees Read with HeapFree(GetProcessHeap(),0,...).
DWORD AmdgpuWindowsReadHandoff(void **buffer,DWORD *bytes);
// The final archive of an uninstall: archive-<transaction>.bin and the removal record removed-v2.bin. A legacy archive
// (origin 2: the store of an install whose first transaction did not finish, BD-298) is written as
// archive-<transaction>.bin only, and every removal record moves aside: no released install reads a legacy record.
DWORD AmdgpuWindowsArchiveState(const void *buffer,DWORD bytes);
// The archive-<transaction>.bin of an uninstall: the record of a resumed legacy uninstall (no removed-v2.bin).
DWORD AmdgpuWindowsReadArchive(const unsigned char transaction[16],void **buffer,DWORD *bytes);
// The removal record that an uninstall archived. own: only the record that this release writes (a resumed
// uninstall); otherwise the newest record of either name (an install). ERROR_UNSUPPORTED_TYPE: the record is
// intact, but of another state catalog; its bytes are returned and the caller frees them.
DWORD AmdgpuWindowsReadRemoval(void **buffer,DWORD *bytes,BOOL own);
// Moves removal records aside as retired-<transaction>-<name> evidence; deletes nothing. keep: the record that
// ReadRemoval returns stays. Otherwise every removal record moves.
DWORD AmdgpuWindowsRetireRemoval(const unsigned char transaction[16],BOOL keep);
// BD-298: moves the pending.bin of a stopped install aside as retired-<transaction>-pending.bin. Its header must name
// this transaction, device and product (32 bytes, zero padded). A record of another state catalog is accepted. No
// pending.bin is success. A retired copy with the same bytes completes the step: the pending file is removed after a
// byte compare. ERROR_REVISION_MISMATCH: the handoff of another transaction; ERROR_INVALID_DATA: a damaged file;
// ERROR_ALREADY_EXISTS: a retired copy with other bytes. On each error the files stay.
DWORD AmdgpuWindowsAbandonHandoff(const unsigned char transaction[16],const unsigned char device_hash[32],
    const char product_version[32]);
// Archive before removing pending. Active must name this exact transaction;
// checked by the state adapter and checked again while holding the writer lock.
DWORD AmdgpuWindowsAcknowledgeHandoff(const unsigned char transaction[16],
    const unsigned char device_hash[32],const char *product_version);
