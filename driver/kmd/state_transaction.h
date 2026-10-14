#pragma once
#include <ntddk.h>
#include "../state/transfer.h"
// Internal SYSTEM initializer only. The caller verifies the protected input
// file, device/package/boot/transaction binding before this registry commit.
// An already applied transaction is never replayed over current user choices.
NTSTATUS DriverStateCommitTransfer(HANDLE shared,const void *buffer,ULONG bytes);
NTSTATUS DriverStateFinishAccepted(HANDLE shared,const AMDGPU_TRANSFER_HEADER *receipt);
