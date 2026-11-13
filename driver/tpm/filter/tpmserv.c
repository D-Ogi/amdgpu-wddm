/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The skeleton of the TPM servicer: an extension-INF lower filter of the ACPI TPM device. It forwards every request
 * unchanged and only records what it sees. It answers no command: the purpose is to measure when tpm.sys sends its
 * first command and what it does while the command is not answered.
 *
 * Identification, from firmware data that exists before this boot-start filter runs:
 *   - the SMBIOS baseboard and system product strings name a BC-250 (the same rule as the project's board identity);
 *   - the firmware's TPM2 table carries the OEM table ID BC250TPM.
 * Either check fails: AddDevice returns success without a device, so the filter stays off any other machine.
 *
 * Logging is TraceLogging with one provider GUID, no generated WPP file:
 *   add, start, started, crb (offset, value, tick), noattach (reason).
 */
#include <ntddk.h>
#include <wdf.h>
#include <aux_klib.h>
#include <TraceLoggingProvider.h>

TRACELOGGING_DEFINE_PROVIDER(g_TpmServ, "Bc250TpmServicer",
	(0x9f3c7a11, 0x2b64, 0x4d0e, 0x8a, 0x51, 0x7c, 0x3e, 0x19, 0xb2, 0x6d, 0x04));

#define TPM2_OEM_TABLE_ID	"BC250TPM"
#define CRB_WATCH_COUNT		4u
#define CRB_CONTROL_AREA	0x40u

typedef struct _DEVICE_CONTEXT {
	PVOID ControlArea;
	SIZE_T ControlBytes;
	ULONG LastValue[CRB_WATCH_COUNT];
	WDFTIMER Timer;
	LARGE_INTEGER FirstTick;
	ULONG TicksSeen;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

static const ULONG CrbWatch[CRB_WATCH_COUNT] = {0x00, 0x04, 0x0C, 0x18};

/* 1 when the firmware's TPM2 table names this project's OEM table ID. AuxKlib is the kernel-side reader. */
static BOOLEAN Tpm2IsOurs(VOID)
{
	ULONG size = 0;
	PVOID table;
	BOOLEAN ours = FALSE;

	if (!NT_SUCCESS(AuxKlibGetSystemFirmwareTable('ACPI', '2MPT', NULL, &size)) || size < 40) return FALSE;
	table = ExAllocatePool2(POOL_FLAG_PAGED, size, 'pmsT');
	if (table == NULL) return FALSE;
	if (NT_SUCCESS(AuxKlibGetSystemFirmwareTable('ACPI', '2MPT', table, &size)))
		ours = RtlEqualMemory((PUCHAR)table + 16, TPM2_OEM_TABLE_ID, 8);
	ExFreePoolWithTag(table, 'pmsT');
	return ours;
}

/* 1 when the SMBIOS baseboard or system product string names a BC-250. */
static BOOLEAN BoardIsBc250(VOID)
{
	static const CHAR needle[] = "BC-250";
	ULONG size = 0, offset;
	PVOID raw;
	PUCHAR p;
	BOOLEAN found = FALSE;

	if (!NT_SUCCESS(AuxKlibGetSystemFirmwareTable('RSMB', 0, NULL, &size)) || size < 8) return FALSE;
	raw = ExAllocatePool2(POOL_FLAG_PAGED, size, 'pmsB');
	if (raw == NULL) return FALSE;
	if (!NT_SUCCESS(AuxKlibGetSystemFirmwareTable('RSMB', 0, raw, &size))) {
		ExFreePoolWithTag(raw, 'pmsB');
		return FALSE;
	}
	p = (PUCHAR)raw;
	for (offset = 0; offset + 4 < size;) {
		UCHAR type = p[offset];
		UCHAR length = p[offset + 1];
		ULONG next;
		if (length < 4 || offset + length > size) break;
		if (type == 1 || type == 2) {			/* System and Baseboard carry the product strings */
			ULONG at;
			for (at = offset + length; at + sizeof(needle) - 1 < size; at++) {
				if (p[at] == 0 && p[at + 1] == 0) break;
				if (RtlEqualMemory(p + at, needle, sizeof(needle) - 1)) { found = TRUE; break; }
			}
			if (found) break;
		}
		next = offset + length;
		while (next + 1 < size && (p[next] != 0 || p[next + 1] != 0)) next++;
		offset = next + 2;
		if (type == 127) break;
	}
	ExFreePoolWithTag(raw, 'pmsB');
	return found;
}

static VOID EvtTimer(_In_ WDFTIMER Timer)
{
	PDEVICE_CONTEXT ctx = DeviceGetContext(WdfTimerGetParentObject(Timer));
	ULONG k, now;
	LARGE_INTEGER tick;

	if (ctx->ControlArea == NULL) return;
	tick.QuadPart = KeQueryPerformanceCounter(NULL).QuadPart;
	if (ctx->TicksSeen < 100) ctx->TicksSeen++;	/* the first hundred ticks state the timer's own resolution */
	for (k = 0; k < CRB_WATCH_COUNT; k++) {
		now = READ_REGISTER_ULONG((PULONG)((PUCHAR)ctx->ControlArea + CrbWatch[k]));
		if (now != ctx->LastValue[k]) {
			ctx->LastValue[k] = now;
			TraceLoggingWrite(g_TpmServ, "crb",
				TraceLoggingUInt32(CrbWatch[k], "offset"),
				TraceLoggingUInt32(now, "value"),
				TraceLoggingInt64(tick.QuadPart, "tick"),
				TraceLoggingUInt32(ctx->TicksSeen, "tickIndex"));
		}
	}
}

static NTSTATUS EvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
	PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
	ULONG i, count = WdfCmResourceListGetCount(Translated);
	(void)Raw;
	for (i = 0; i < count; i++) {
		PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(Translated, i);
		if (d != NULL && d->Type == CmResourceTypeMemory && d->u.Memory.Length >= CRB_CONTROL_AREA) {
			PHYSICAL_ADDRESS a;
			a.QuadPart = d->u.Memory.Start.QuadPart;
			ctx->ControlArea = MmMapIoSpaceEx(a, d->u.Memory.Length, PAGE_READWRITE | PAGE_NOCACHE);
			ctx->ControlBytes = d->u.Memory.Length;
			if (ctx->ControlArea != NULL)
				TraceLoggingWrite(g_TpmServ, "map",
					TraceLoggingUInt64(d->u.Memory.Start.QuadPart, "page"),
					TraceLoggingUInt32((ULONG)d->u.Memory.Length, "bytes"));
			return STATUS_SUCCESS;
		}
	}
	TraceLoggingWrite(g_TpmServ, "nomap");
	return STATUS_SUCCESS;
}

static NTSTATUS EvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
	PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
	(void)Translated;
	if (ctx->ControlArea != NULL) {
		MmUnmapIoSpace(ctx->ControlArea, ctx->ControlBytes);
		ctx->ControlArea = NULL;
	}
	return STATUS_SUCCESS;
}

static NTSTATUS EvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
	WDFDEVICE device;
	PDEVICE_CONTEXT ctx;
	WDF_TIMER_CONFIG timerConfig;
	WDF_OBJECT_ATTRIBUTES attributes;
	NTSTATUS status;

	UNREFERENCED_PARAMETER(Driver);
	if (!BoardIsBc250() || !Tpm2IsOurs()) {
		TraceLoggingWrite(g_TpmServ, "noattach",
			TraceLoggingBool(BoardIsBc250(), "board"),
			TraceLoggingBool(Tpm2IsOurs(), "tpm2"));
		WdfDeviceInitFree(DeviceInit);
		return STATUS_SUCCESS;
	}
	status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
	if (!NT_SUCCESS(status)) return status;
	ctx = DeviceGetContext(device);
	TraceLoggingWrite(g_TpmServ, "add");

	WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, EvtTimer, 1);
	timerConfig.UseHighResolutionTimer = WdfTrue;	/* otherwise a periodic KMDF timer is 15.6 ms */
	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = device;
	if (NT_SUCCESS(WdfTimerCreate(&timerConfig, &attributes, &ctx->Timer)))
		WdfTimerStart(ctx->Timer, WDF_REL_TIMEOUT_IN_MS(1));
	return STATUS_SUCCESS;
}

static NTSTATUS EvtDeviceStart(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesRaw, _In_ WDFCMRESLIST ResourcesTranslated)
{
	(void)Device; (void)ResourcesRaw; (void)ResourcesTranslated;
	TraceLoggingWrite(g_TpmServ, "start", TraceLoggingInt64(KeQueryPerformanceCounter(NULL).QuadPart, "tick"));
	TraceLoggingWrite(g_TpmServ, "started");
	return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
	WDF_DRIVER_CONFIG config;
	NTSTATUS status;

	TraceLoggingRegister(g_TpmServ);
	TraceLoggingWrite(g_TpmServ, "entry");
	if (!NT_SUCCESS(AuxKlibInitialize())) {
		TraceLoggingWrite(g_TpmServ, "noattach", TraceLoggingUInt32(1, "auxklib"));
		return STATUS_UNSUCCESSFUL;
	}
	WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
	status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
	if (!NT_SUCCESS(status)) TraceLoggingUnregister(g_TpmServ);
	return status;
}
