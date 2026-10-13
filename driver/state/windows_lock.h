// One installer/settings writer at a time across processes and x86/x64 clients.
#pragma once
#include <windows.h>
DWORD AmdgpuStateWriterLock(HANDLE *lock);
void AmdgpuStateWriterUnlock(HANDLE lock);
