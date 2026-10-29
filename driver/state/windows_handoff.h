#pragma once
#include <windows.h>
// Installer worker only. Requires LocalSystem; fixed DriverData hierarchy.
// Existing pending content must be identical. No overwrite of a different job.
DWORD AmdgpuWindowsPublishHandoff(const void *buffer,DWORD bytes);
// All operations require LocalSystem, retain protected ancestor handles and use
// fixed names. The caller frees Read with HeapFree(GetProcessHeap(),0,...).
DWORD AmdgpuWindowsReadHandoff(void **buffer,DWORD *bytes);
DWORD AmdgpuWindowsArchiveState(const void *buffer,DWORD bytes);
// The removal record that an uninstall archived. own: only the record that this release writes (a resumed
// uninstall); otherwise the newest record of either name (an install). ERROR_UNSUPPORTED_TYPE: the record is
// intact, but of another state catalog, and no bytes are returned.
DWORD AmdgpuWindowsReadRemoval(void **buffer,DWORD *bytes,BOOL own);
// Moves removal records aside as retired-<transaction>-<name> evidence; deletes nothing. keep: the record that
// ReadRemoval returns stays. Otherwise every removal record moves.
DWORD AmdgpuWindowsRetireRemoval(const unsigned char transaction[16],BOOL keep);
// Archive before removing pending. Active must name this exact transaction;
// checked by the state adapter and checked again while holding the writer lock.
DWORD AmdgpuWindowsAcknowledgeHandoff(const unsigned char transaction[16],
    const unsigned char device_hash[32],const char *product_version);
