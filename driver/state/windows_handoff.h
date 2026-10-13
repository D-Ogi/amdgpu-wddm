#pragma once
#include <windows.h>
// Installer worker only. Requires LocalSystem; fixed DriverData hierarchy.
// Existing pending content must be identical. No overwrite of a different job.
DWORD AmdgpuWindowsPublishHandoff(const void *buffer,DWORD bytes);
// All operations require LocalSystem, retain protected ancestor handles and use
// fixed names. The caller frees Read with HeapFree(GetProcessHeap(),0,...).
DWORD AmdgpuWindowsReadHandoff(void **buffer,DWORD *bytes);
DWORD AmdgpuWindowsArchiveState(const void *buffer,DWORD bytes);
DWORD AmdgpuWindowsReadRemoval(void **buffer,DWORD *bytes);
// Archive before removing pending. Active must name this exact transaction;
// checked by the state adapter and checked again while holding the writer lock.
DWORD AmdgpuWindowsAcknowledgeHandoff(const unsigned char transaction[16],
    const unsigned char device_hash[32],const char *product_version);
