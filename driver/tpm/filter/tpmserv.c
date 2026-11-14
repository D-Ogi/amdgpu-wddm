/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * tpmserv.sys: the kernel half of the BC-250 TPM servicer. An extension-INF lower filter below the inbox tpm.sys on
 * ACPI\MSFT0101. The boot stage reserves one page of DRAM, describes it in the device _CRS and in the ACPI TPM2
 * table (start method 7, CRB), and tpm.sys talks to that page as to a CRB TPM. This filter plays the TPM side of
 * the page (crb.c) and moves each command to the user-mode TPM core service through a control device
 * (tpmserv_ioctl.h), and the answer back.
 *
 * Identification, before anything attaches: the SMBIOS tables pass the shared board rule
 * (BoardIdentitySelectSmbios), and the firmware TPM2 table carries the OEM table ID BC250TPM of the stage. On any
 * other machine DriverEntry creates no control device and AddDevice attaches nothing, so a real TPM below tpm.sys
 * is never touched.
 *
 * Order at start: the start request goes down to the ACPI bus driver first; on the way back up this filter maps
 * the page and resets it to the TPM reset state (EvtDevicePrepareHardware) before tpm.sys sees the start complete.
 *
 * Polling: a high-resolution one-shot timer. 1 ms while the page shows activity, 10 ms after one second without
 * any. The PTP has no doorbell for software-to-TPM requests on this page, so polling is the only way to see them.
 *
 * Logging: TraceLogging provider Bc250TpmServicer.
 */
#include <ntddk.h>
#include <wdf.h>
#include <wdmsec.h>
#include <aux_klib.h>
#include <TraceLoggingProvider.h>
#include <acpiioct.h>
#include "../crb.h"
#include "../handoff.h"
#include "../tpmserv_ioctl.h"
#include "../../common/board/board_identity.h"

TRACELOGGING_DEFINE_PROVIDER(g_TpmServ, "Bc250TpmServicer",
	(0x9f3c7a11, 0x2b64, 0x4d0e, 0x8a, 0x51, 0x7c, 0x3e, 0x19, 0xb2, 0x6d, 0x04));

#define TPM2_OEM_TABLE_ID	"BC250TPM"
#define TPM2_CONTROL_OFFSET	40u	/* AddressOfControlArea in the ACPI TPM2 table */
#define TPM2_START_OFFSET	48u	/* StartMethod */
#define TPM2_START_CRB		7u
#define POLL_FAST_MS		1
#define POLL_SLOW_MS		10
#define POLL_FAST_TICKS		1000u	/* quiet fast ticks before the slow rate */
/* How long a command waits for a core service when none is connected, before the filter answers it with
 * TPM_RC_FAILURE. Long enough for a restart of the service; short, because tpm.sys sends its last commands
 * (TPM2_Shutdown) in the kernel phase of a shutdown, after every service has ended, and without an answer it waits
 * for its own timeouts: the shutdown of unit A took more than 180 s that way (2026-11-13). */
#define ORPHAN_WAIT_100NS	(2000ll * 10000ll)

/* One servicer per machine: the platform has one TPM. The lock orders the timer, the I/O callbacks and the PnP
 * callbacks; every access to the window and to the command state holds it. */
typedef struct _SERVICER {
	WDFSPINLOCK Lock;
	WDFDEVICE Control;
	WDFQUEUE GetQueue;		/* GET_COMMAND requests that wait for a command */
	WDFTIMER Timer;
	ULONGLONG ExpectedPage;		/* from the TPM2 table: the control area minus 0x40 */
	PVOID Window;
	SIZE_T WindowBytes;
	ULONGLONG Page;
	BOOLEAN Attached;
	BOOLEAN CommandOut;		/* crb_poll gave a command and crb_complete has not run */
	BOOLEAN Delivered;		/* the command went to the service */
	WDFFILEOBJECT DeliveredTo;
	BOOLEAN ServiceOpen;		/* the core service holds the control device open */
	ULONGLONG OrphanDeadline;	/* interrupt time at which a command without a service is failed, 0 if none */
	ULONG Redelivered;
	ULONG QuietTicks;
	struct crb Crb;
	UCHAR Command[TPMSERV_MAX_COMMAND];
	SIZE_T CommandBytes;
	/* The handoff record of the pre-OS TPM: read at the first start of the device, given out once. */
	ULONG Handoff;			/* TPMSERV_HANDOFF_* */
	PUCHAR HandoffRecord;		/* nonpaged copy while READY */
	ULONG HandoffBytes;
} SERVICER;

static SERVICER g;

/* The control area of the TPM2 table when the table is the stage's (OEM table ID BC250TPM, start method CRB),
 * else 0. AuxKlib is the kernel-side reader of firmware tables. */
static ULONGLONG Tpm2ControlArea(VOID)
{
	ULONG size = 0;
	PUCHAR table;
	ULONGLONG control = 0;

	if (AuxKlibGetSystemFirmwareTable('ACPI', '2MPT', NULL, 0, &size) != STATUS_BUFFER_TOO_SMALL || size < 52)
		return 0;
	table = (PUCHAR)ExAllocatePool2(POOL_FLAG_PAGED, size, 'pmsT');
	if (table == NULL) return 0;
	if (NT_SUCCESS(AuxKlibGetSystemFirmwareTable('ACPI', '2MPT', table, size, &size)) && size >= 52 &&
	    RtlEqualMemory(table + 16, TPM2_OEM_TABLE_ID, 8) &&
	    *(UNALIGNED ULONG *)(table + TPM2_START_OFFSET) == TPM2_START_CRB)
		control = *(UNALIGNED ULONGLONG *)(table + TPM2_CONTROL_OFFSET);
	ExFreePoolWithTag(table, 'pmsT');
	return control;
}

/* TRUE when the SMBIOS tables are a documented BC-250 (driver/common/board: the rule the display driver uses,
 * without its PCI half, which a TPM filter cannot read). */
static BOOLEAN BoardIsBc250(VOID)
{
	ULONG size = 0;
	PUCHAR p;
	unsigned reason = 0;
	BOOLEAN found = FALSE;

	if (AuxKlibGetSystemFirmwareTable('RSMB', 0, NULL, 0, &size) != STATUS_BUFFER_TOO_SMALL || size < 8)
		return FALSE;
	p = (PUCHAR)ExAllocatePool2(POOL_FLAG_PAGED, size, 'pmsB');
	if (p == NULL) return FALSE;
	if (NT_SUCCESS(AuxKlibGetSystemFirmwareTable('RSMB', 0, p, size, &size)))
		found = BoardIdentitySelectSmbios(p, size, &reason) == 1;
	ExFreePoolWithTag(p, 'pmsB');
	return found;
}

static ULONG ActivitySum(const struct crb_counters *k)
{
	return k->commands + k->bad_size + k->start_while_idle + k->cancels + k->ready + k->idle +
	       k->locality_requests + k->locality_relinquish + k->failed;
}

/* Under the lock: hand the command out if a request waits. Returns the request to complete, or NULL. */
static WDFREQUEST TakeWaitingRequestLocked(VOID)
{
	WDFREQUEST request;
	if (!g.CommandOut || g.Delivered) return NULL;
	if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(g.GetQueue, &request))) return NULL;
	g.Delivered = TRUE;
	g.DeliveredTo = WdfRequestGetFileObject(request);
	return request;
}

/* Outside the lock: copy the command into a GET_COMMAND request and complete it. The command bytes cannot change
 * between TakeWaitingRequestLocked and here: a new command needs crb_complete first, which needs this delivery. */
static VOID CompleteGet(WDFREQUEST request)
{
	PVOID out;
	size_t outBytes;
	NTSTATUS status = WdfRequestRetrieveOutputBuffer(request, TPMSERV_MAX_COMMAND, &out, &outBytes);
	if (NT_SUCCESS(status)) {
		RtlCopyMemory(out, g.Command, g.CommandBytes);
		WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, g.CommandBytes);
	} else {
		WdfRequestComplete(request, status);
	}
}

/* Under the lock: a command that is out while no service is connected gets TPM_RC_FAILURE after ORPHAN_WAIT_100NS.
 * A service that connects in that time gets the command. */
static VOID FailOrphanLocked(VOID)
{
	ULONGLONG now;

	if (!g.CommandOut || g.ServiceOpen) {
		g.OrphanDeadline = 0;
		return;
	}
	now = KeQueryInterruptTime();
	if (g.OrphanDeadline == 0) {
		g.OrphanDeadline = now + ORPHAN_WAIT_100NS;
		return;
	}
	if (now < g.OrphanDeadline) return;
	crb_fail(&g.Crb, CRB_TPM_RC_FAILURE);
	g.CommandOut = FALSE;
	g.Delivered = FALSE;
	g.DeliveredTo = NULL;
	g.OrphanDeadline = 0;
	TraceLoggingWrite(g_TpmServ, "orphan",
		TraceLoggingHexUInt32(((ULONG)g.Command[6] << 24) | ((ULONG)g.Command[7] << 16) |
				      ((ULONG)g.Command[8] << 8) | g.Command[9], "code"));
}

static VOID EvtTimer(_In_ WDFTIMER Timer)
{
	WDFREQUEST deliver = NULL;
	ULONG before, after;
	LONG delay = 0;
	size_t bytes = 0;

	WdfSpinLockAcquire(g.Lock);
	if (g.Attached) {
		before = ActivitySum(&g.Crb.count);
		if (crb_poll(&g.Crb, g.Command, sizeof(g.Command), &bytes) == CRB_POLL_COMMAND) {
			g.CommandBytes = bytes;
			g.CommandOut = TRUE;
			g.Delivered = FALSE;
			TraceLoggingWrite(g_TpmServ, "command",
				TraceLoggingUInt32((ULONG)bytes, "bytes"),
				TraceLoggingHexUInt32(((ULONG)g.Command[6] << 24) | ((ULONG)g.Command[7] << 16) |
						      ((ULONG)g.Command[8] << 8) | g.Command[9], "code"));
		}
		FailOrphanLocked();
		after = ActivitySum(&g.Crb.count);
		deliver = TakeWaitingRequestLocked();
		if (after != before || g.CommandOut) g.QuietTicks = 0;
		else if (g.QuietTicks < POLL_FAST_TICKS) g.QuietTicks++;
		delay = g.QuietTicks < POLL_FAST_TICKS ? POLL_FAST_MS : POLL_SLOW_MS;
	}
	WdfSpinLockRelease(g.Lock);
	if (deliver != NULL) CompleteGet(deliver);
	/* Detached: the timer stops; the next EvtPrepareHardware starts it again. */
	if (delay != 0) WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(delay));
}

static VOID KickTimer(VOID)
{
	if (g.Timer != NULL) WdfTimerStart(g.Timer, WDF_REL_TIMEOUT_IN_MS(POLL_FAST_MS));
}

static VOID GetCommand(WDFREQUEST request)
{
	WDFREQUEST deliver = NULL;
	NTSTATUS status;

	WdfSpinLockAcquire(g.Lock);
	status = WdfRequestForwardToIoQueue(request, g.GetQueue);
	if (NT_SUCCESS(status)) deliver = TakeWaitingRequestLocked();
	WdfSpinLockRelease(g.Lock);
	if (!NT_SUCCESS(status)) WdfRequestComplete(request, status);
	else if (deliver != NULL) CompleteGet(deliver);
}

static VOID PutResponse(WDFREQUEST request)
{
	PVOID in;
	size_t inBytes;
	NTSTATUS status = WdfRequestRetrieveInputBuffer(request, CRB_TPM_HEADER_BYTES, &in, &inBytes);

	if (NT_SUCCESS(status)) {
		WdfSpinLockAcquire(g.Lock);
		if (g.Attached && g.CommandOut && g.Delivered && g.DeliveredTo == WdfRequestGetFileObject(request)) {
			crb_complete(&g.Crb, (const unsigned char *)in, inBytes);
			g.CommandOut = FALSE;
			g.Delivered = FALSE;
			g.QuietTicks = 0;
		} else {
			status = STATUS_INVALID_DEVICE_STATE;
		}
		WdfSpinLockRelease(g.Lock);
		TraceLoggingWrite(g_TpmServ, "response", TraceLoggingUInt32((ULONG)inBytes, "bytes"),
				  TraceLoggingNTStatus(status, "status"));
	}
	WdfRequestComplete(request, status);
}

static VOID GetStatus(WDFREQUEST request)
{
	struct tpmserv_status *s;
	size_t bytes;
	NTSTATUS status = WdfRequestRetrieveOutputBuffer(request, sizeof(*s), (PVOID *)&s, &bytes);

	if (!NT_SUCCESS(status)) {
		WdfRequestComplete(request, status);
		return;
	}
	RtlZeroMemory(s, sizeof(*s));
	WdfSpinLockAcquire(g.Lock);
	s->version = TPMSERV_STATUS_VERSION;
	s->attached = g.Attached;
	s->page = g.Attached ? g.Page : 0;
	s->commands = g.Crb.count.commands;
	s->bad_size = g.Crb.count.bad_size;
	s->start_while_idle = g.Crb.count.start_while_idle;
	s->cancels = g.Crb.count.cancels;
	s->ready = g.Crb.count.ready;
	s->idle = g.Crb.count.idle;
	s->locality_requests = g.Crb.count.locality_requests;
	s->locality_relinquish = g.Crb.count.locality_relinquish;
	s->redelivered = g.Redelivered;
	s->failed = g.Crb.count.failed;
	s->handoff = g.Handoff;
	s->handoff_bytes = g.HandoffBytes;
	WdfSpinLockRelease(g.Lock);
	WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(*s));
}

static VOID GetHandoff(WDFREQUEST request);

static VOID EvtControlIoctl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t OutputBufferLength,
			    _In_ size_t InputBufferLength, _In_ ULONG IoControlCode)
{
	UNREFERENCED_PARAMETER(Queue);
	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);
	switch (IoControlCode) {
	case IOCTL_TPMSERV_GET_COMMAND: GetCommand(Request); break;
	case IOCTL_TPMSERV_PUT_RESPONSE: PutResponse(Request); break;
	case IOCTL_TPMSERV_GET_STATUS: GetStatus(Request); break;
	case IOCTL_TPMSERV_GET_HANDOFF: GetHandoff(Request); break;
	default: WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST); break;
	}
}

/* The service opened the control device (it is exclusive: one service). */
static VOID EvtFileCreate(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ WDFFILEOBJECT FileObject)
{
	UNREFERENCED_PARAMETER(Device);
	UNREFERENCED_PARAMETER(FileObject);
	WdfSpinLockAcquire(g.Lock);
	g.ServiceOpen = TRUE;
	WdfSpinLockRelease(g.Lock);
	WdfRequestComplete(Request, STATUS_SUCCESS);
}

/* The service closed its handle: a command it held goes out again to the next GET_COMMAND, or gets
 * TPM_RC_FAILURE when no service connects in time (FailOrphanLocked). */
static VOID EvtFileCleanup(_In_ WDFFILEOBJECT FileObject)
{
	WdfSpinLockAcquire(g.Lock);
	g.ServiceOpen = FALSE;
	if (g.Delivered && g.DeliveredTo == FileObject) {
		g.Delivered = FALSE;
		g.DeliveredTo = NULL;
		g.Redelivered++;
	}
	WdfSpinLockRelease(g.Lock);
}

static NTSTATUS CreateControlDevice(_In_ WDFDRIVER Driver)
{
	DECLARE_CONST_UNICODE_STRING(name, TPMSERV_DEVICE_NAME);
	DECLARE_CONST_UNICODE_STRING(link, TPMSERV_SYMLINK_NAME);
	PWDFDEVICE_INIT init;
	WDF_FILEOBJECT_CONFIG fileConfig;
	WDF_IO_QUEUE_CONFIG queueConfig;
	WDF_TIMER_CONFIG timerConfig;
	WDF_OBJECT_ATTRIBUTES attributes;
	WDFDEVICE control;
	NTSTATUS status;

	init = WdfControlDeviceInitAllocate(Driver, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
	if (init == NULL) return STATUS_INSUFFICIENT_RESOURCES;
	WdfDeviceInitSetExclusive(init, TRUE);	/* one core service */
	WdfDeviceInitSetDeviceType(init, TPMSERV_DEVICE_TYPE);
	status = WdfDeviceInitAssignName(init, &name);
	if (!NT_SUCCESS(status)) { WdfDeviceInitFree(init); return status; }
	WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, EvtFileCreate, WDF_NO_EVENT_CALLBACK, EvtFileCleanup);
	WdfDeviceInitSetFileObjectConfig(init, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);
	status = WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &control);
	if (!NT_SUCCESS(status)) { if (init != NULL) WdfDeviceInitFree(init); return status; }

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = control;
	status = WdfSpinLockCreate(&attributes, &g.Lock);
	if (NT_SUCCESS(status)) status = WdfDeviceCreateSymbolicLink(control, &link);
	if (NT_SUCCESS(status)) {
		WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
		queueConfig.EvtIoDeviceControl = EvtControlIoctl;
		status = WdfIoQueueCreate(control, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
	}
	if (NT_SUCCESS(status)) {
		WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
		status = WdfIoQueueCreate(control, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &g.GetQueue);
	}
	if (NT_SUCCESS(status)) {
		WDF_TIMER_CONFIG_INIT(&timerConfig, EvtTimer);
		timerConfig.UseHighResolutionTimer = WdfTrue;	/* a normal KMDF timer has the 15.6 ms clock tick */
		WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
		attributes.ParentObject = control;
		status = WdfTimerCreate(&timerConfig, &attributes, &g.Timer);
	}
	if (!NT_SUCCESS(status)) {
		WdfObjectDelete(control);
		g.Lock = NULL; g.GetQueue = NULL; g.Timer = NULL;
		return status;
	}
	g.Control = control;
	WdfControlFinishInitializing(control);
	return STATUS_SUCCESS;
}

/* Name (BCHO, Package () { base, bytes }) of Device (TPM0): the handoff range of the pre-OS TPM
 * (docs/design/tpm-preos.md, "The handoff range"). The ACPI bus driver below this filter evaluates it. Returns
 * STATUS_SUCCESS with the range, or the status of the evaluation (STATUS_OBJECT_NAME_NOT_FOUND: no BCHO). */
static NTSTATUS EvaluateBcho(WDFDEVICE device, PULONGLONG base, PULONG bytes)
{
	ACPI_EVAL_INPUT_BUFFER in;
	UCHAR outBuffer[sizeof(ACPI_EVAL_OUTPUT_BUFFER) + 2 * sizeof(ACPI_METHOD_ARGUMENT) + 16];
	PACPI_EVAL_OUTPUT_BUFFER out = (PACPI_EVAL_OUTPUT_BUFFER)outBuffer;
	WDF_MEMORY_DESCRIPTOR inDesc, outDesc;
	PACPI_METHOD_ARGUMENT arg;
	ULONGLONG value[2];
	ULONG_PTR got = 0;
	ULONG i;
	NTSTATUS status;

	RtlZeroMemory(&in, sizeof(in));
	RtlZeroMemory(outBuffer, sizeof(outBuffer));
	in.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
	in.MethodNameAsUlong = (ULONG)'OHCB';	/* "BCHO" in memory order */
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inDesc, &in, sizeof(in));
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outDesc, outBuffer, sizeof(outBuffer));
	status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(device), NULL, IOCTL_ACPI_EVAL_METHOD, &inDesc,
						   &outDesc, NULL, &got);
	if (!NT_SUCCESS(status)) return status;
	/* A package comes back as its elements (ACPI_EVAL_OUTPUT_BUFFER): two integers, 32 or 64 bits each. */
	if (got < FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) || out->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE ||
	    out->Count != 2 || out->Length > got)
		return STATUS_ACPI_INVALID_DATA;
	arg = out->Argument;
	for (i = 0; i < 2; i++) {
		if ((PUCHAR)arg + FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) > outBuffer + got ||
		    arg->Type != ACPI_METHOD_ARGUMENT_INTEGER ||
		    (arg->DataLength != sizeof(ULONG) && arg->DataLength != sizeof(ULONGLONG)) ||
		    (PUCHAR)arg + FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) + arg->DataLength > outBuffer + got)
			return STATUS_ACPI_INVALID_DATA;
		value[i] = arg->DataLength == sizeof(ULONG) ? *(UNALIGNED ULONG *)arg->Data : *(UNALIGNED ULONGLONG *)arg->Data;
		arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
	}
	if (value[1] > TPM_HANDOFF_MAX_BYTES || value[1] < TPM_HANDOFF_HEADER_BYTES || (value[0] & (PAGE_SIZE - 1)) != 0 ||
	    value[0] == 0)
		return STATUS_ACPI_INVALID_DATA;
	*base = value[0];
	*bytes = (ULONG)value[1];
	return STATUS_SUCCESS;
}

/* At the first start of the device: copy the handoff record out of its range and write zeros over the range, which
 * holds the seeds of the TPM. Only the frame is checked here; the core checks the digest of its copy. */
static VOID ReadHandoff(WDFDEVICE device)
{
	ULONGLONG base = 0;
	ULONG bytes = 0, state, record = 0;
	PHYSICAL_ADDRESS at;
	PUCHAR window, copy = NULL;
	struct tpm_handoff_view view;
	enum tpm_handoff_result r;
	NTSTATUS status = EvaluateBcho(device, &base, &bytes);

	RtlZeroMemory(&view, sizeof(view));
	if (!NT_SUCCESS(status)) {
		state = status == STATUS_OBJECT_NAME_NOT_FOUND ? TPMSERV_HANDOFF_NONE : TPMSERV_HANDOFF_BAD;
		TraceLoggingWrite(g_TpmServ, "handoff", TraceLoggingNTStatus(status, "bcho"), TraceLoggingUInt32(state, "state"));
		WdfSpinLockAcquire(g.Lock);
		g.Handoff = state;
		WdfSpinLockRelease(g.Lock);
		return;
	}
	at.QuadPart = (LONGLONG)base;
	/* Cached: the range is reserved DRAM that only this filter reads, after the firmware wrote it. */
	window = (PUCHAR)MmMapIoSpaceEx(at, bytes, PAGE_READWRITE);
	if (window == NULL) {
		state = TPMSERV_HANDOFF_BAD;
	} else {
		r = tpm_handoff_read(window, bytes, NULL, &view);
		if (r == TPM_HANDOFF_OK) {
			record = view.record_bytes;
			copy = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, record, 'hmsT');
			if (copy != NULL) RtlCopyMemory(copy, window, record);
		}
		state = r == TPM_HANDOFF_NONE ? TPMSERV_HANDOFF_NONE : copy != NULL ? TPMSERV_HANDOFF_READY : TPMSERV_HANDOFF_BAD;
		RtlSecureZeroMemory(window, bytes);
		MmUnmapIoSpace(window, bytes);
	}
	TraceLoggingWrite(g_TpmServ, "handoff", TraceLoggingHexUInt64(base, "base"), TraceLoggingUInt32(bytes, "range"),
			  TraceLoggingUInt32(state, "state"), TraceLoggingUInt32(copy ? record : 0, "record"));
	WdfSpinLockAcquire(g.Lock);
	g.Handoff = state;
	g.HandoffRecord = copy;
	g.HandoffBytes = copy ? record : 0;
	WdfSpinLockRelease(g.Lock);
}

static VOID GetHandoff(WDFREQUEST request)
{
	PUCHAR record = NULL;
	ULONG bytes = 0;
	PVOID out;
	size_t outBytes = 0;
	NTSTATUS status = WdfRequestRetrieveOutputBuffer(request, 1, &out, &outBytes);

	if (!NT_SUCCESS(status)) {
		WdfRequestComplete(request, status);
		return;
	}
	/* Take the record out under the lock, copy it outside: a copy of up to 1 MiB does not belong under a spin lock. */
	WdfSpinLockAcquire(g.Lock);
	if (g.Handoff != TPMSERV_HANDOFF_READY) {
		status = STATUS_NOT_FOUND;
	} else if (outBytes < g.HandoffBytes) {
		status = STATUS_BUFFER_TOO_SMALL;
	} else {
		record = g.HandoffRecord;
		bytes = g.HandoffBytes;
		g.HandoffRecord = NULL;
		g.Handoff = TPMSERV_HANDOFF_TAKEN;
	}
	WdfSpinLockRelease(g.Lock);
	if (record != NULL) {
		RtlCopyMemory(out, record, bytes);
		RtlSecureZeroMemory(record, bytes);
		ExFreePoolWithTag(record, 'hmsT');
	}
	TraceLoggingWrite(g_TpmServ, "gethandoff", TraceLoggingNTStatus(status, "status"), TraceLoggingUInt32(bytes, "bytes"));
	WdfRequestCompleteWithInformation(request, status, bytes);
}

static NTSTATUS EvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
	ULONG i, count = WdfCmResourceListGetCount(Translated);
	PHYSICAL_ADDRESS at;
	PVOID window;
	BOOLEAN look;

	UNREFERENCED_PARAMETER(Raw);
	WdfSpinLockAcquire(g.Lock);
	look = g.Handoff == TPMSERV_HANDOFF_PENDING;
	WdfSpinLockRelease(g.Lock);
	if (look) ReadHandoff(Device);
	for (i = 0; i < count; i++) {
		PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(Translated, i);
		if (d == NULL || d->Type != CmResourceTypeMemory) continue;
		if ((ULONGLONG)d->u.Memory.Start.QuadPart != g.ExpectedPage || d->u.Memory.Length < CRB_PAGE_BYTES) {
			TraceLoggingWrite(g_TpmServ, "foreignrange",
				TraceLoggingHexUInt64((ULONGLONG)d->u.Memory.Start.QuadPart, "start"),
				TraceLoggingUInt32(d->u.Memory.Length, "bytes"),
				TraceLoggingHexUInt64(g.ExpectedPage, "expected"));
			continue;
		}
		at.QuadPart = d->u.Memory.Start.QuadPart;
		/* Uncached, as tpm.sys maps the same page: the two sides see each other's writes in program order. */
		window = MmMapIoSpaceEx(at, CRB_PAGE_BYTES, PAGE_READWRITE | PAGE_NOCACHE);
		if (window == NULL) return STATUS_INSUFFICIENT_RESOURCES;
		WdfSpinLockAcquire(g.Lock);
		g.Window = window;
		g.WindowBytes = CRB_PAGE_BYTES;
		g.Page = (ULONGLONG)at.QuadPart;
		crb_reset(&g.Crb, window, g.Page);
		g.CommandOut = FALSE;
		g.Delivered = FALSE;
		g.QuietTicks = 0;
		g.Attached = TRUE;
		WdfSpinLockRelease(g.Lock);
		TraceLoggingWrite(g_TpmServ, "map", TraceLoggingHexUInt64(g.Page, "page"));
		KickTimer();
		return STATUS_SUCCESS;
	}
	/* Without the page tpm.sys has nothing to talk to; failing the start makes that a visible device problem. */
	TraceLoggingWrite(g_TpmServ, "nomap", TraceLoggingHexUInt64(g.ExpectedPage, "expected"));
	return STATUS_DEVICE_CONFIGURATION_ERROR;
}

static NTSTATUS EvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
	PVOID window;
	SIZE_T bytes;

	UNREFERENCED_PARAMETER(Device);
	UNREFERENCED_PARAMETER(Translated);
	WdfSpinLockAcquire(g.Lock);
	window = g.Window;
	bytes = g.WindowBytes;
	g.Attached = FALSE;
	g.CommandOut = FALSE;	/* the requester restarts with the device; an answer to its old command has no reader */
	g.Delivered = FALSE;
	g.Window = NULL;
	WdfSpinLockRelease(g.Lock);
	if (window != NULL) MmUnmapIoSpace(window, bytes);
	TraceLoggingWrite(g_TpmServ, "unmap");
	return STATUS_SUCCESS;
}

static NTSTATUS EvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
	WDF_PNPPOWER_EVENT_CALLBACKS pnp;
	WDFDEVICE device;

	UNREFERENCED_PARAMETER(Driver);
	if (g.Control == NULL) {
		/* Not this platform (see DriverEntry): stay out of the stack. */
		return STATUS_SUCCESS;
	}
	WdfFdoInitSetFilter(DeviceInit);
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
	pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
	pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
	WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);
	TraceLoggingWrite(g_TpmServ, "add");
	return WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
}

static VOID EvtDriverUnload(_In_ WDFDRIVER Driver)
{
	UNREFERENCED_PARAMETER(Driver);
	if (g.HandoffRecord != NULL) {
		RtlSecureZeroMemory(g.HandoffRecord, g.HandoffBytes);
		ExFreePoolWithTag(g.HandoffRecord, 'hmsT');
		g.HandoffRecord = NULL;
	}
	TraceLoggingWrite(g_TpmServ, "unload");
	TraceLoggingUnregister(g_TpmServ);
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
	WDF_DRIVER_CONFIG config;
	WDFDRIVER driver;
	ULONGLONG control;
	BOOLEAN board;
	NTSTATUS status;

	TraceLoggingRegister(g_TpmServ);
	RtlZeroMemory(&g, sizeof(g));
	WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
	config.EvtDriverUnload = EvtDriverUnload;
	status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
	if (!NT_SUCCESS(status)) {
		TraceLoggingUnregister(g_TpmServ);
		return status;
	}
	/* A failed identification is not a driver failure: the filter must never stop a real TPM from starting. */
	board = NT_SUCCESS(AuxKlibInitialize()) && BoardIsBc250();
	control = board ? Tpm2ControlArea() : 0;
	TraceLoggingWrite(g_TpmServ, "identity", TraceLoggingBool(board, "board"),
			  TraceLoggingHexUInt64(control, "control"));
	if (!board || control < CRB_CONTROL_AREA) return STATUS_SUCCESS;
	g.ExpectedPage = control - CRB_CONTROL_AREA;
	status = CreateControlDevice(driver);
	TraceLoggingWrite(g_TpmServ, "control", TraceLoggingNTStatus(status, "status"));
	return STATUS_SUCCESS;
}
