/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * tpmserv.sys: the kernel half of the BC-250 TPM servicer. An extension-INF lower filter below the inbox tpm.sys on
 * ACPI\MSFT0101. The boot stage reserves one page of DRAM, describes it in the device _CRS and in the ACPI TPM2
 * table (start method 7, CRB), and tpm.sys talks to that page as to a CRB TPM. This filter plays the TPM side of
 * the page (crb.c) and moves each command to the user-mode TPM core service through a control device
 * (tpmserv_ioctl.h), and the answer back.
 *
 * Identification, before anything attaches: the SMBIOS system or baseboard product names a BC-250, and the
 * firmware TPM2 table carries the OEM table ID BC250TPM of the stage. On any other machine DriverEntry creates no
 * control device and AddDevice attaches nothing, so a real TPM below tpm.sys is never touched.
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
#include "../crb.h"
#include "../tpmserv_ioctl.h"

TRACELOGGING_DEFINE_PROVIDER(g_TpmServ, "Bc250TpmServicer",
	(0x9f3c7a11, 0x2b64, 0x4d0e, 0x8a, 0x51, 0x7c, 0x3e, 0x19, 0xb2, 0x6d, 0x04));

#define TPM2_OEM_TABLE_ID	"BC250TPM"
#define TPM2_CONTROL_OFFSET	40u	/* AddressOfControlArea in the ACPI TPM2 table */
#define TPM2_START_OFFSET	48u	/* StartMethod */
#define TPM2_START_CRB		7u
#define POLL_FAST_MS		1
#define POLL_SLOW_MS		10
#define POLL_FAST_TICKS		1000u	/* quiet fast ticks before the slow rate */

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
	ULONG Redelivered;
	ULONG QuietTicks;
	struct crb Crb;
	UCHAR Command[TPMSERV_MAX_COMMAND];
	SIZE_T CommandBytes;
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

/* TRUE when the SMBIOS system (type 1) or baseboard (type 2) strings name a BC-250. */
static BOOLEAN BoardIsBc250(VOID)
{
	static const CHAR needle[] = "BC-250";
	ULONG size = 0, offset;
	PUCHAR p;
	BOOLEAN found = FALSE;

	if (AuxKlibGetSystemFirmwareTable('RSMB', 0, NULL, 0, &size) != STATUS_BUFFER_TOO_SMALL || size < 8)
		return FALSE;
	p = (PUCHAR)ExAllocatePool2(POOL_FLAG_PAGED, size, 'pmsB');
	if (p == NULL) return FALSE;
	if (!NT_SUCCESS(AuxKlibGetSystemFirmwareTable('RSMB', 0, p, size, &size))) {
		ExFreePoolWithTag(p, 'pmsB');
		return FALSE;
	}
	/* RawSMBIOSData: 8 header bytes, then the structures. Each one is a formatted area of p[1] bytes and a
	 * string set that ends with two zero bytes. */
	for (offset = 8; offset + 4 <= size && !found;) {
		UCHAR type = p[offset], length = p[offset + 1];
		ULONG next;
		if (length < 4 || offset + length > size) break;
		next = offset + length;
		while (next + 1 < size && (p[next] != 0 || p[next + 1] != 0)) {
			if ((type == 1 || type == 2) && next + sizeof(needle) - 1 <= size &&
			    RtlEqualMemory(p + next, needle, sizeof(needle) - 1))
				found = TRUE;
			next++;
		}
		if (type == 127) break;
		offset = next + 2;
	}
	ExFreePoolWithTag(p, 'pmsB');
	return found;
}

static ULONG ActivitySum(const struct crb_counters *k)
{
	return k->commands + k->bad_size + k->start_while_idle + k->cancels + k->ready + k->idle +
	       k->locality_requests + k->locality_relinquish;
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
	WdfSpinLockRelease(g.Lock);
	WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(*s));
}

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
	default: WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST); break;
	}
}

/* The service closed its handle: a command it held goes out again to the next GET_COMMAND. */
static VOID EvtFileCleanup(_In_ WDFFILEOBJECT FileObject)
{
	WdfSpinLockAcquire(g.Lock);
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
	WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, EvtFileCleanup);
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

static NTSTATUS EvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
	ULONG i, count = WdfCmResourceListGetCount(Translated);
	PHYSICAL_ADDRESS at;
	PVOID window;

	UNREFERENCED_PARAMETER(Device);
	UNREFERENCED_PARAMETER(Raw);
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
