/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * core-unlock: the UEFI application that enables the two CPU cores the BC-250 is sold without, before Windows
 * starts (docs/design/efi-core-unlock.md). This file is the UEFI binding only: it gives cu_run() its operations
 * and then starts the Windows Boot Manager. The decision is cu_decide.c, the steps are cu_run.c, the message is
 * cu_smu.c over the driver's own transport and policy, and the board identity is the driver's own
 * BoardIdentitySelect (driver/kmd/board_identity.c) over the firmware's SMBIOS table.
 *
 * Every path ends in "start Windows". When that fails too, the application returns the error, and the firmware
 * goes on with its boot order.
 *
 * Kto pyta, nie błądzi - who asks does not get lost. This application asks the firmware four questions before it
 * sends one message.
 */
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "efi.h"
#include "cu_acpi.h"
#include "cu_decide.h"
#include "cu_run.h"
#include "cu_smbios.h"
#include "cu_smu.h"
#include "cu_text.h"
#include "cu_report.h"
#include "report_uefi.h"
#include "bc250_mmio_bar.h"
#include "board_identity.h"

#define CU_VERSION		"1"
#define CU_KEY_WINDOW_MS	1500u
#define CU_KEY_POLL_MS		10u
#define CU_RESET_PAUSE_MS	1000u	/* the console lines stay readable for this long before a reset */
#define CU_RESET_WAIT_MS	1000u	/* a reset register write that has not reset the board by then did not work */
#define CU_PATH_CHARS		260u
#define CU_MARKER_MAX_BYTES	64u
#define CU_DEVPATH_MAX_BYTES	4096u
#define CU_SMBIOS_MAX_BYTES	(1024u * 1024u)	/* the bound driver/kmd/board_memory_identity.c sets on 'RSMB' */
#define CU_LINE_CHARS		256u

/* The type 0 configuration header (PCI Local Bus Specification 3.0, section 6.1). BoardIdentitySelect takes its
 * first 64 bytes, which is what the Windows driver reads as well. */
#define PCI_CONFIG_BYTES	64u
#define PCI_COMMAND		0x04u
#define PCI_COMMAND_MEMORY	0x0002u
#define PCI_BAR0		0x10u
#define PCI_BAR_IO		0x1u
#define PCI_BAR_ADDRESS_MASK	(~0xFu)

static const EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static const EFI_GUID device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static const EFI_GUID file_system_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static const EFI_GUID pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;
static const EFI_GUID mp_services_guid = EFI_MP_SERVICES_PROTOCOL_GUID;
static const EFI_GUID acpi20_guid = EFI_ACPI_20_TABLE_GUID;
static const EFI_GUID acpi10_guid = EFI_ACPI_10_TABLE_GUID;
static const EFI_GUID smbios3_guid = EFI_SMBIOS3_TABLE_GUID;
static const EFI_GUID smbios_guid = EFI_SMBIOS_TABLE_GUID;

static const unsigned short boot_manager[] = u"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";

struct app {
	EFI_HANDLE image;
	EFI_SYSTEM_TABLE *st;
	EFI_BOOT_SERVICES *bs;
	EFI_LOADED_IMAGE_PROTOCOL *loaded;
	EFI_FILE_PROTOCOL *root;
	unsigned short marker[CU_PATH_CHARS];
	int marker_known;
	unsigned short report[CU_PATH_CHARS];
	int report_known;
	EFI_PCI_IO_PROTOCOL *gpu;
	unsigned char gpu_config[PCI_CONFIG_BYTES];
	struct cu_options options;
	unsigned long long rsdp;
};

/* ---- console ----------------------------------------------------------------------------------- */

static void say_line(struct app *app, const char *line)
{
	static const char prefix[] = "core-unlock: ";
	CHAR16 wide[CU_LINE_CHARS + sizeof(prefix) + 3u];
	size_t n = 0, i;
	for (i = 0; prefix[i]; i++) wide[n++] = (unsigned char)prefix[i];
	for (i = 0; line[i] && n < CU_LINE_CHARS; i++) wide[n++] = (unsigned char)line[i];
	wide[n++] = '\r';
	wide[n++] = '\n';
	wide[n] = 0;
	if (app->st->ConOut != NULL) app->st->ConOut->OutputString(app->st->ConOut, wide);
}

static void sayf(struct app *app, const char *format, ...)
{
	char line[CU_LINE_CHARS];
	va_list args;
	va_start(args, format);
	cu_vformat(line, sizeof(line), format, args);
	va_end(args);
	say_line(app, line);
}

/* ---- firmware tables --------------------------------------------------------------------------- */

static const unsigned char *map_identity(void *context, unsigned long long address, unsigned long bytes)
{
	/* Boot services run with physical memory mapped one to one (UEFI Specification section 2.3.4). */
	(void)context;
	(void)bytes;
	return address != 0 ? (const unsigned char *)(UINTN)address : NULL;
}

static void *config_table(const struct app *app, const EFI_GUID *guid)
{
	UINTN i;
	for (i = 0; i < app->st->NumberOfTableEntries; i++)
		if (!memcmp(&app->st->ConfigurationTable[i].VendorGuid, guid, sizeof(*guid)))
			return app->st->ConfigurationTable[i].VendorTable;
	return NULL;
}

/* The SMBIOS table in the RSMB form, in pool memory the caller frees. NULL when there is none. */
static unsigned char *smbios_rsmb(struct app *app, size_t *bytes)
{
	struct cu_smbios_table table;
	const unsigned char *data;
	unsigned char *rsmb = NULL;
	size_t extent;
	void *entry;
	*bytes = 0;
	entry = config_table(app, &smbios3_guid);
	if (entry == NULL || !cu_smbios_entry(map_identity, NULL, (UINTN)entry, 1, &table)) {
		entry = config_table(app, &smbios_guid);
		if (entry == NULL || !cu_smbios_entry(map_identity, NULL, (UINTN)entry, 0, &table)) return NULL;
	}
	if (table.max_bytes > CU_SMBIOS_MAX_BYTES) table.max_bytes = CU_SMBIOS_MAX_BYTES;
	data = map_identity(NULL, table.address, table.max_bytes);
	extent = cu_smbios_extent(data, table.max_bytes);
	if (extent == 0) return NULL;
	if (EFI_ERROR(app->bs->AllocatePool(EfiLoaderData, CU_RSMB_HEADER_BYTES + extent, (VOID **)&rsmb))) return NULL;
	*bytes = cu_smbios_rsmb(data, extent, table.major, table.minor, rsmb, CU_RSMB_HEADER_BYTES + extent);
	return rsmb;
}

/* ---- the operations of cu_run ------------------------------------------------------------------ */

static int op_board(void *context)
{
	struct app *app = context;
	EFI_HANDLE *handles = NULL;
	UINTN count = 0, i;
	unsigned char *rsmb;
	size_t rsmb_bytes;
	unsigned matches = 0, candidates = 0, reason = 0, bios = 0;
	unsigned char machine[16];

	rsmb = smbios_rsmb(app, &rsmb_bytes);
	if (rsmb == NULL) {
		say_line(app, "no SMBIOS table that ends with a type 127 structure");
		return 0;
	}
	if (EFI_ERROR(app->bs->LocateHandleBuffer(ByProtocol, &pci_io_guid, NULL, &count, &handles))) count = 0;
	for (i = 0; i < count; i++) {
		EFI_PCI_IO_PROTOCOL *pci = NULL;
		UINT32 config[PCI_CONFIG_BYTES / 4u];
		if (EFI_ERROR(app->bs->HandleProtocol(handles[i], &pci_io_guid, (VOID **)&pci)) || pci == NULL) continue;
		if (EFI_ERROR(pci->Pci.Read(pci, EfiPciIoWidthUint32, 0, PCI_CONFIG_BYTES / 4u, config))) continue;
		/* The PCI function first, without the firmware table, as driver/kmd/board_memory_identity.c does:
		 * reason 3 (FIRMWARE) says that the function matched. */
		(void)BoardIdentitySelect((const unsigned char *)config, PCI_CONFIG_BYTES, NULL, 0, &reason);
		if (reason != 3u) continue;
		candidates++;
		if (BoardIdentitySelect((const unsigned char *)config, PCI_CONFIG_BYTES, rsmb, rsmb_bytes, &reason) == 1u) {
			UINTN segment = 0, bus = 0, device = 0, function = 0;
			matches++;
			app->gpu = pci;
			memcpy(app->gpu_config, config, PCI_CONFIG_BYTES);
			if (!EFI_ERROR(pci->GetLocation(pci, &segment, &bus, &device, &function)))
				sayf(app, "GPU function at %u:%u:%u.%u", (unsigned)segment, (unsigned)bus, (unsigned)device,
				     (unsigned)function);
		}
	}
	if (handles != NULL) app->bs->FreePool(handles);
	if (matches == 1u && BoardIdentityQualification(rsmb, rsmb_bytes, &bios, machine))
		sayf(app, "board BC-250, BIOS P%u.00", bios);
	memset(machine, 0, sizeof(machine));
	app->bs->FreePool(rsmb);
	if (matches != 1u) {
		sayf(app, "%u matching GPU functions, %u with a matching SMBIOS table", candidates, matches);
		app->gpu = NULL;
		return 0;
	}
	return 1;
}

static unsigned op_processors(void *context)
{
	struct app *app = context;
	EFI_MP_SERVICES_PROTOCOL *mp = NULL;
	struct cu_acpi acpi;
	UINTN all = 0, enabled = 0;
	unsigned madt = 0;
	if (!EFI_ERROR(app->bs->LocateProtocol(&mp_services_guid, NULL, (VOID **)&mp)) && mp != NULL &&
	    !EFI_ERROR(mp->GetNumberOfProcessors(mp, &all, &enabled))) {
		sayf(app, "MP Services: %u logical processors, %u enabled", (unsigned)all, (unsigned)enabled);
		return (unsigned)enabled;
	}
	/* The fallback: the enabled processors of the ACPI MADT. */
	acpi.map = map_identity;
	acpi.context = NULL;
	acpi.rsdp = app->rsdp;
	if (cu_acpi_madt_processors(&acpi, &madt) == CU_ACPI_OK) {
		sayf(app, "no MP Services; the MADT lists %u enabled logical processors", madt);
		return madt;
	}
	say_line(app, "no MP Services and no MADT: the processor count is not known");
	return 0;
}

static int marker_open(struct app *app, EFI_FILE_PROTOCOL **file, UINT64 mode, EFI_STATUS *status)
{
	*file = NULL;
	if (app->root == NULL || !app->marker_known) {
		*status = EFI_NOT_READY;
		return -1;
	}
	*status = app->root->Open(app->root, file, app->marker, mode, 0);
	return EFI_ERROR(*status) ? -1 : 0;
}

static enum cu_marker op_marker_read(void *context)
{
	struct app *app = context;
	EFI_FILE_PROTOCOL *file;
	EFI_STATUS status;
	unsigned char data[CU_MARKER_MAX_BYTES];
	UINTN bytes = sizeof(data);
	if (marker_open(app, &file, EFI_FILE_MODE_READ, &status))
		return status == EFI_NOT_FOUND ? CU_MARKER_NONE : CU_MARKER_ERROR;
	status = file->Read(file, &bytes, data);
	file->Close(file);
	if (EFI_ERROR(status)) return CU_MARKER_ERROR;
	return cu_marker_parse(data, (unsigned long)bytes);
}

static int op_marker_write(void *context, enum cu_marker state)
{
	struct app *app = context;
	EFI_FILE_PROTOCOL *file;
	EFI_STATUS status;
	const char *text = cu_marker_text(state);
	UINTN bytes;
	if (text == NULL) return -1;
	if (marker_open(app, &file, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, &status)) {
		sayf(app, "marker create failed, status 0x%llx", (unsigned long long)status);
		return -1;
	}
	bytes = strlen(text);
	status = file->Write(file, &bytes, (VOID *)text);
	if (!EFI_ERROR(status)) status = file->Flush(file);
	file->Close(file);
	if (EFI_ERROR(status) || bytes != strlen(text)) {
		sayf(app, "marker write failed, status 0x%llx", (unsigned long long)status);
		return -1;
	}
	/* The guard counts only when it reads back. */
	return op_marker_read(app) == state ? 0 : -1;
}

static int op_marker_delete(void *context)
{
	struct app *app = context;
	EFI_FILE_PROTOCOL *file;
	EFI_STATUS status;
	if (marker_open(app, &file, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, &status))
		return status == EFI_NOT_FOUND ? 0 : -1;
	/* Delete closes the handle. A warning status means that the file is still there. */
	return file->Delete(file) == EFI_SUCCESS ? 0 : -1;
}

static int op_key_window(void *context)
{
	struct app *app = context;
	EFI_SIMPLE_TEXT_INPUT_PROTOCOL *in = app->st->ConIn;
	EFI_INPUT_KEY key;
	unsigned waited;
	if (in == NULL) return 0;
	/* Keys pressed before the window, at the firmware's own prompt for example, do not count. */
	in->Reset(in, 0);
	sayf(app, "press any key within %u ms to skip and start Windows", CU_KEY_WINDOW_MS);
	for (waited = 0; waited < CU_KEY_WINDOW_MS; waited += CU_KEY_POLL_MS) {
		if (in->ReadKeyStroke(in, &key) == EFI_SUCCESS) return 1;
		app->bs->Stall(CU_KEY_POLL_MS * 1000u);
	}
	return 0;
}

/* ---- the register BAR -------------------------------------------------------------------------- */

static int bar_read32(void *context, unsigned offset, unsigned *value)
{
	struct app *app = context;
	UINT32 v = 0;
	if ((offset & 3u) || offset >= BC250_BAR5_LENGTH) return -1;
	if (EFI_ERROR(app->gpu->Mem.Read(app->gpu, EfiPciIoWidthUint32, BC250_BAR5_INDEX, offset, 1, &v))) return -1;
	*value = v;
	return 0;
}

static int bar_write32(void *context, unsigned offset, unsigned value)
{
	struct app *app = context;
	UINT32 v = value;
	if ((offset & 3u) || offset >= BC250_BAR5_LENGTH) return -1;
	return EFI_ERROR(app->gpu->Mem.Write(app->gpu, EfiPciIoWidthUint32, BC250_BAR5_INDEX, offset, 1, &v)) ? -1 : 0;
}

static void bar_stall(void *context, unsigned usec)
{
	((struct app *)context)->bs->Stall(usec);
}

static unsigned le32(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* The register BAR is identified twice, as driver/kmd/mmio.c does: by what the configuration header says BAR5
 * holds, and by the resource of the expected length at that very address that the PCI bus driver reports. The
 * memory decode must already be on: this application does not change the function's command register. */
static int bar_identified(struct app *app)
{
	CU_ACPI_QWORD_ADDRESS_DESCRIPTOR *resource = NULL;
	UINT64 supports = 0;
	unsigned bar = le32(app->gpu_config + PCI_BAR0 + 4u * BC250_BAR5_INDEX);
	unsigned command = (unsigned)app->gpu_config[PCI_COMMAND] | ((unsigned)app->gpu_config[PCI_COMMAND + 1] << 8);
	int ok;
	if (!(command & PCI_COMMAND_MEMORY)) {
		say_line(app, "the GPU function's memory decode is off");
		return 0;
	}
	if ((bar & PCI_BAR_IO) || (bar & PCI_BAR_ADDRESS_MASK) == 0) {
		sayf(app, "BAR%u is not a memory BAR (0x%x)", (unsigned)BC250_BAR5_INDEX, bar);
		return 0;
	}
	if (EFI_ERROR(app->gpu->GetBarAttributes(app->gpu, BC250_BAR5_INDEX, &supports, (VOID **)&resource)) ||
	    resource == NULL) {
		sayf(app, "BAR%u has no resource descriptor", (unsigned)BC250_BAR5_INDEX);
		return 0;
	}
	ok = resource->Desc == CU_ACPI_QWORD_DESCRIPTOR && resource->ResType == CU_ACPI_RESOURCE_MEMORY &&
	     resource->AddrLen == BC250_BAR5_LENGTH && resource->AddrRangeMin == (bar & PCI_BAR_ADDRESS_MASK);
	if (!ok)
		sayf(app, "BAR%u is not the register BAR: length 0x%llx at 0x%llx", (unsigned)BC250_BAR5_INDEX,
		     (unsigned long long)resource->AddrLen, (unsigned long long)resource->AddrRangeMin);
	app->bs->FreePool(resource);
	return ok;
}

static int op_unlock(void *context)
{
	struct app *app = context;
	struct cu_regs regs;
	struct cu_unlock_report report;
	if (app->gpu == NULL || !bar_identified(app)) return -1;
	regs.context = app;
	regs.read32 = bar_read32;
	regs.write32 = bar_write32;
	regs.stall_us = bar_stall;
	(void)cu_unlock_send(&regs, &report);
	sayf(app, "queue 3: CPU %u mV, temperature %d mC%s", report.millivolts, report.temperature_mc,
	     report.temperature_valid ? "" : " (not read)");
	sayf(app, "%s (status %d, response 0x%x, %u messages)", cu_unlock_result_text(report.result), report.status,
	     report.response, report.messages);
	return report.result == CU_UNLOCK_SENT ? 0 : -1;
}

/* ---- the reset ---------------------------------------------------------------------------------- */

static void outb(unsigned short port, unsigned char value)
{
	__asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static int reset_fadt(struct app *app)
{
	struct cu_acpi acpi;
	struct cu_reset_register reg;
	enum cu_acpi_result result;
	acpi.map = map_identity;
	acpi.context = NULL;
	acpi.rsdp = app->rsdp;
	result = cu_acpi_reset_register(&acpi, &reg);
	if (result != CU_ACPI_OK) {
		sayf(app, "no usable FADT reset register (reason %u)", (unsigned)result);
		return -1;
	}
	sayf(app, "FADT reset register: space %u, address 0x%llx, value 0x%x", reg.space, reg.address,
	     (unsigned)reg.value);
	if (reg.space == CU_ACPI_SPACE_IO && reg.address <= 0xFFFFu) {
		app->bs->Stall(CU_RESET_PAUSE_MS * 1000u);
		outb((unsigned short)reg.address, reg.value);
	} else if (reg.space == CU_ACPI_SPACE_MEMORY) {
		app->bs->Stall(CU_RESET_PAUSE_MS * 1000u);
		*(volatile UINT8 *)(UINTN)reg.address = reg.value;
	} else {
		say_line(app, "a reset register in this address space is not supported");
		return -1;
	}
	app->bs->Stall(CU_RESET_WAIT_MS * 1000u);
	return -1;
}

static int op_reset(void *context)
{
	struct app *app = context;
	switch (app->options.reset) {
	case CU_RESET_WARM:
		app->bs->Stall(CU_RESET_PAUSE_MS * 1000u);
		app->st->RuntimeServices->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
		return -1;
	case CU_RESET_FADT:
		return reset_fadt(app);
	case CU_RESET_NONE:
	default:
		return -1;
	}
}

static void op_say(void *context, const char *line)
{
	say_line(context, line);
}

/* ---- Windows ----------------------------------------------------------------------------------- */

static EFI_STATUS start_windows(struct app *app)
{
	EFI_DEVICE_PATH_PROTOCOL *device = NULL;
	unsigned char *path = NULL;
	EFI_HANDLE windows = NULL;
	EFI_STATUS status;
	size_t bytes;
	UINTN capacity;
	status = app->bs->HandleProtocol(app->loaded->DeviceHandle, &device_path_guid, (VOID **)&device);
	if (EFI_ERROR(status) || device == NULL) {
		sayf(app, "no device path for the boot partition, status 0x%llx", (unsigned long long)status);
		return EFI_ERROR(status) ? status : EFI_NOT_FOUND;
	}
	capacity = CU_DEVPATH_MAX_BYTES + sizeof(boot_manager) + 8u;
	status = app->bs->AllocatePool(EfiLoaderData, capacity, (VOID **)&path);
	if (EFI_ERROR(status)) return status;
	bytes = cu_devpath_with_file((const unsigned char *)device, CU_DEVPATH_MAX_BYTES, boot_manager, path, capacity);
	if (bytes == 0) {
		app->bs->FreePool(path);
		say_line(app, "the device path of the boot partition is malformed");
		return EFI_LOAD_ERROR;
	}
	say_line(app, "starting Windows");
	status = app->bs->LoadImage(0, app->image, (EFI_DEVICE_PATH_PROTOCOL *)path, NULL, 0, &windows);
	app->bs->FreePool(path);
	if (EFI_ERROR(status)) {
		sayf(app, "the Windows Boot Manager did not load, status 0x%llx", (unsigned long long)status);
		return status;
	}
	status = app->bs->StartImage(windows, NULL, NULL);
	sayf(app, "the Windows Boot Manager returned, status 0x%llx", (unsigned long long)status);
	return status;
}

/* ---- the read-only report ----------------------------------------------------------------------- */

/* The load option report=1: the report goes first, so it shows the state that the decision then reads. It does
 * not feed the decision. */
static void run_report(struct app *app)
{
	struct report_uefi r;
	struct cu_acpi acpi;
	acpi.map = map_identity;
	acpi.context = NULL;
	acpi.rsdp = app->rsdp;
	r.st = app->st;
	r.root = app->root;
	r.path = app->report_known ? app->report : NULL;
	r.acpi = &acpi;
	r.version = CU_VERSION;
	r.say = op_say;
	r.say_context = app;
	report_uefi_run(&r);
}

/* ---- the entry point --------------------------------------------------------------------------- */

static void open_marker_volume(struct app *app)
{
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
	unsigned short image_file[CU_PATH_CHARS];
	if (cu_devpath_file((const unsigned char *)app->loaded->FilePath, CU_DEVPATH_MAX_BYTES, image_file,
			    CU_PATH_CHARS) &&
	    cu_marker_path(image_file, app->marker, CU_PATH_CHARS)) {
		app->marker_known = 1;
		app->report_known = cu_sibling_path(image_file, CU_REPORT_NAME, app->report, CU_PATH_CHARS);
	} else
		say_line(app, "the path of this application is not known: no marker, so no reset");
	if (EFI_ERROR(app->bs->HandleProtocol(app->loaded->DeviceHandle, &file_system_guid, (VOID **)&fs)) || fs == NULL ||
	    EFI_ERROR(fs->OpenVolume(fs, &app->root)))
		app->root = NULL;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	static struct app app;
	struct cu_ops ops;
	EFI_STATUS status;
	void *rsdp;

	memset(&app, 0, sizeof(app));
	app.image = image;
	app.st = st;
	app.bs = st->BootServices;
	sayf(&app, "BC-250 eight-core unlock, version %s", CU_VERSION);
	if (EFI_ERROR(app.bs->HandleProtocol(image, &loaded_image_guid, (VOID **)&app.loaded)) || app.loaded == NULL) {
		say_line(&app, "no loaded image protocol: cannot find the boot partition");
		return EFI_LOAD_ERROR;
	}
	cu_options_parse(app.loaded->LoadOptions, app.loaded->LoadOptionsSize / 2u, &app.options);
	sayf(&app, "reset method: %s%s", cu_reset_method_name(app.options.reset),
	     app.options.report ? ", read-only report on" : "");
	if (app.options.unknown) sayf(&app, "%u load option(s) not understood and ignored", app.options.unknown);
	rsdp = config_table(&app, &acpi20_guid);
	if (rsdp == NULL) rsdp = config_table(&app, &acpi10_guid);
	app.rsdp = (UINTN)rsdp;
	open_marker_volume(&app);
	if (app.options.report) run_report(&app);

	ops.context = &app;
	ops.board = op_board;
	ops.processors = op_processors;
	ops.marker_read = op_marker_read;
	ops.marker_write = op_marker_write;
	ops.marker_delete = op_marker_delete;
	ops.key_window = op_key_window;
	ops.unlock = op_unlock;
	ops.reset = op_reset;
	ops.say = op_say;
	(void)cu_run(&ops, app.options.reset != CU_RESET_NONE);

	if (app.root != NULL) app.root->Close(app.root);
	app.root = NULL;
	status = start_windows(&app);
	return EFI_ERROR(status) ? status : EFI_SUCCESS;
}
