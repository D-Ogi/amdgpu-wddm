/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The skeleton of the TPM servicer: a lower filter of the ACPI TPM device that forwards everything unchanged and
 * only records what it sees. It answers no command. Its purpose is to measure two things before the servicer is
 * written: when tpm.sys sends its first command, and what it does while the command is not answered.
 *
 * It attaches only to a board that identifies itself as a BC-250 and whose ACPI TPM2 table carries the OEM table ID
 * BC250TPM. On any other machine AddDevice returns success without creating a device, so the filter is harmless.
 *
 * Logging is TraceLogging (ETW), one provider, events:
 *   1 add: the device was attached
 *   2 start: IRP_MN_START_DEVICE arrived, with the tick count
 *   3 started: the start completed, with the tick count
 *   4 crb: a CRB register of locality 0 changed, with the offset, the value and the tick count
 *   5 noattach: the identification refused, with the reason
 */
#include <ntddk.h>
#include <wdf.h>
#include <trace.h>
#include "tpmserv.tmh"

#define TPM2_TABLE_SIGNATURE	0x324D5054u	/* "TPM2" */
#define TPM2_OEM_TABLE_ID	"BC250TPM"
#define CRB_PAGE_OFFSETS	4

typedef struct _TPM2_HEADER {
	UINT32 Signature;
	UINT32 Length;
	UINT8  Revision;
	UINT8  Checksum;
	UINT8  OemId[6];
	UINT8  OemTableId[8];
} TPM2_HEADER;

typedef struct _TPM2_FIRMWARE_TABLE {
	TPM2_HEADER Header;
	UINT32 Flags;
	UINT64 AddressOfControlArea;
	UINT32 StartMethod;
} TPM2_FIRMWARE_TABLE;

typedef struct _DEVICE_CONTEXT {
	WDFDEVICE Device;
	PVOID ControlArea;
	SIZE_T ControlBytes;
	ULONG LastValue[CRB_PAGE_OFFSETS];
	WDFTIMER Timer;
	BOOLEAN Started;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

/* The CRB registers of locality 0 that the skeleton watches, relative to the control area. */
static const ULONG CrbWatch[CRB_PAGE_OFFSETS] = {0x00, 0x04, 0x0C, 0x18};

/* 1 when the firmware's TPM2 table names this project's OEM table ID. */
static BOOLEAN Tpm2IsOurs(VOID)
{
	TPM2_FIRMWARE_TABLE *table;
	DWORD size;
	BOOLEAN ours = FALSE;

	size = GetSystemFirmwareTable('ACPI', '2MPT' /* 'TPM2' in little-endian order */, NULL, 0);
	if (size < sizeof(TPM2_FIRMWARE_TABLE)) return FALSE;
	table = (TPM2_FIRMWARE_TABLE *)ExAllocatePool2(POOL_FLAG_PAGED, size, 'pmsT');
	if (table == NULL) return FALSE;
	if (GetSystemFirmwareTable('ACPI', '2MPT', table, size) >= sizeof(TPM2_FIRMWARE_TABLE) &&
	    table->Header.Signature == TPM2_TABLE_SIGNATURE &&
	    RtlEqualMemory(table->Header.OemTableId, TPM2_OEM_TABLE_ID, sizeof(table->Header.OemTableId)))
		ours = TRUE;
	ExFreePoolWithTag(table, 'pmsT');
	return ours;
}

/* 1 when the board is a BC-250: the PCI identity of the APU is the one the driver work already trusts. */
static BOOLEAN BoardIsBc250(VOID)
{
	UNICODE_STRING name;
	NTSTATUS status;
	HANDLE key = NULL;
	OBJECT_ATTRIBUTES attributes;
	UCHAR value[4];
	ULONG length = 0;

	RtlInitUnicodeString(&name, L"\\Registry\\Machine\\HARDWARE\\DEVICEMAP\\Vid\\BC250");
	InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
	status = ZwOpenKey(&key, KEY_READ, &attributes);
	if (!NT_SUCCESS(status)) return FALSE;
	status = ZwQueryValueKey(key, &name, KeyValuePartialInformation, value, sizeof(value), &length);
	ZwClose(key);
	return NT_SUCCESS(status);
}

static VOID EvtTimer(_In_ WDFTIMER Timer)
{
	PDEVICE_CONTEXT ctx = DeviceGetContext(WdfTimerGetParentObject(Timer));
	ULONG k, now;

	if (ctx->ControlArea == NULL) return;
	for (k = 0; k < CRB_PAGE_OFFSETS; k++) {
		now = READ_REGISTER_ULONG((PULONG)((PUCHAR)ctx->ControlArea + CrbWatch[k]));
		if (now != ctx->LastValue[k]) {
			ctx->LastValue[k] = now;
			TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "crb offset=%u value=%u", CrbWatch[k], now);
		}
	}
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
		TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER, "noattach board=%u tpm2=%u", BoardIsBc250(), Tpm2IsOurs());
		WdfDeviceInitFree(DeviceInit);
		return STATUS_SUCCESS;	/* the filter stays off this machine */
	}
	status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
	if (!NT_SUCCESS(status)) return status;
	ctx = DeviceGetContext(device);
	ctx->Device = device;
	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "add");

	WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, EvtTimer, 1);
	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = device;
	if (NT_SUCCESS(WdfTimerCreate(&timerConfig, &attributes, &ctx->Timer)))
		WdfTimerStart(ctx->Timer, WDF_REL_TIMEOUT_IN_MS(1));
	return STATUS_SUCCESS;
}

static NTSTATUS EvtDeviceStart(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesRaw, _In_ WDFCMRESLIST ResourcesTranslated)
{
	PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
	(void)ResourcesRaw;
	(void)ResourcesTranslated;
	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "start");
	ctx->Started = TRUE;
	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "started");
	return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
	WDF_DRIVER_CONFIG config;
	NTSTATUS status;

	WPP_INIT_TRACING(DriverObject, RegistryPath);
	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "entry");
	WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
	config.EvtDriverUnload = NULL;
	status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
	if (!NT_SUCCESS(status)) WPP_CLEANUP(DriverObject);
	return status;
}
