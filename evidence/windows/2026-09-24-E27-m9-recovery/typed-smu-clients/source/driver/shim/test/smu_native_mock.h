#pragma once
#include <windows.h>
#include <stdio.h>
#include <string.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
typedef SRWLOCK EX_PUSH_LOCK;
typedef HANDLE PETHREAD;
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#define PASSIVE_LEVEL 0
static unsigned KeGetCurrentIrql(void) { return 0; }
#define MAXULONG 0xffffffffUL
#define KeEnterCriticalRegion() ((void)0)
#define KeLeaveCriticalRegion() ((void)0)
#define ExInitializePushLock InitializeSRWLock
#define ExAcquirePushLockExclusive AcquireSRWLockExclusive
#define ExReleasePushLockExclusive ReleaseSRWLockExclusive
#define PsGetCurrentThread() ((PETHREAD)(ULONG_PTR)GetCurrentThreadId())
static volatile LONG native_checks,native_failures;
#define CHECK(x) do { InterlockedIncrement(&native_checks); if(!(x)) { InterlockedIncrement(&native_failures);printf("FAIL line %d: %s\n",__LINE__,#x); } } while(0)
#define NT_ASSERT(x) CHECK(x)
ULONG NativeRead(PULONG address);
void NativeWrite(PULONG address,ULONG value);
#define READ_REGISTER_ULONG NativeRead
#define WRITE_REGISTER_ULONG NativeWrite
static ULONG64 KeQueryInterruptTimePrecise(PULONG64 qpc) {
    CHECK(qpc!=NULL);*qpc=GetTickCount64();return *qpc*10000;
}
static void KeStallExecutionProcessor(unsigned usec) { CHECK(usec==1); }
