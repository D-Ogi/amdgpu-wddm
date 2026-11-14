/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * bc250-stage: the UEFI application that prepares a BC-250 before Windows starts
 * (docs/design/efi-resident-stage.md). This file is the core. It owns the console, the load options, the volume
 * of the stage, the board identity and the start of the Windows Boot Manager, and it runs the modules in their
 * order. It holds no job of a module: the core unlock, the ACPI repair and the Secure Boot check each live in
 * their own directory behind struct stage_module (stage.h).
 *
 * Every path ends in "start Windows", with one exception that the operator asks for: a module may refuse the
 * image (sb.mode=enforce). When the start fails, the application returns the error, and the firmware goes on with
 * its boot order.
 *
 * Kto pyta, nie błądzi - who asks does not get lost. The stage asks the firmware before it changes anything.
 */
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "efi.h"
#include "efl_acpi.h"
#include "efl_smbios.h"
#include "efl_text.h"
#include "stage.h"
#include "board_identity.h"

#define STAGE_LINE_CHARS	256u
#define STAGE_DEVPATH_MAX_BYTES	4096u
#define STAGE_SMBIOS_MAX_BYTES	(1024u * 1024u)	/* the bound driver/kmd/board_memory_identity.c sets on 'RSMB' */
#define STAGE_IMAGE_MAX_BYTES	(64u * 1024u * 1024u)	/* a boot manager or a boot loader is a few megabytes */
#define STAGE_LOG_NAME		"bc250-stage.log"
#define STAGE_LOG_BYTES		(16u * 1024u)		/* the console lines that wait for their write */
#define STAGE_LOG_FILE_MAX	(256u * 1024u)		/* a larger log file starts again */

static const EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static const EFI_GUID device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static const EFI_GUID file_system_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static const EFI_GUID pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;
static const EFI_GUID acpi20_guid = EFI_ACPI_20_TABLE_GUID;
static const EFI_GUID acpi10_guid = EFI_ACPI_10_TABLE_GUID;
static const EFI_GUID smbios3_guid = EFI_SMBIOS3_TABLE_GUID;
static const EFI_GUID smbios_guid = EFI_SMBIOS_TABLE_GUID;

/* The image that the stage starts after its work, on the volume of the stage. The load option next=<path> names
 * another one, for example the boot loader of the lab stick. */
static const char default_next[] = "\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
static unsigned short next_image[STAGE_OPTION_CHARS];

/* The modules, in the order of their work. The core unlock goes first: when it resets the board, the work of the
 * others would be lost, and they must see the processor count that Windows gets. The TPM goes before the ACPI
 * repair, which describes the handoff range of the TPM, and before the Secure Boot check, so that the TPM measures
 * the image that the check then sees. */
static const struct stage_module *const modules[] = { &cu_module, &tpm_module, &ar_module, &sb_module };
#define MODULES (sizeof(modules) / sizeof(modules[0]))
static int module_on[MODULES];

/* ---- console and log ----------------------------------------------------------------------------- */

/* The log is the console text of this start, written behind the text of earlier starts in a file beside the
 * stage. A board without a person at its screen gives its console back that way. The load option log= chooses
 * the moment of the write. log_text holds only the lines that are not yet in the file: a write empties it, and a
 * line that finds it full causes a write first. So the length of the log has no bound in memory. */
enum { LOG_UNDECIDED = -1, LOG_OFF = 0, LOG_AT_END = 1, LOG_EACH_LINE = 2 };
static const char *const log_modes[] = { "0", "1", "line" };
static char log_text[STAGE_LOG_BYTES];
static size_t log_used;
static unsigned log_lost;		/* lines that found log_text full and could not be written first */
static int log_mode = LOG_UNDECIDED;	/* until the load options are read: lines are kept, none is written */

static void log_keep(struct stage *stage, const CHAR16 *wide, size_t chars)
{
	size_t i;
	if (log_mode == LOG_OFF) return;
	if (log_used + chars > STAGE_LOG_BYTES) stage_log_flush(stage);
	if (log_mode == LOG_OFF) return;	/* the write failed, and its line said so */
	if (log_used + chars > STAGE_LOG_BYTES) {
		log_lost++;
		return;
	}
	for (i = 0; i < chars; i++)
		if (wide[i] != '\r') log_text[log_used++] = (char)wide[i];
}

void stage_log_flush(struct stage *stage)
{
	unsigned short path[STAGE_PATH_CHARS];
	EFI_FILE_PROTOCOL *file = NULL;
	EFI_STATUS status;
	UINT64 size = 0;
	UINTN bytes = log_used;
	const UINT64 mode = EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE;

	if (log_mode <= LOG_OFF || bytes == 0 || stage->root == NULL) return;
	if (!stage_sibling(stage, STAGE_LOG_NAME, path, STAGE_PATH_CHARS)) return;
	status = stage->root->Open(stage->root, &file, path, mode, 0);
	if (!EFI_ERROR(status) && file == NULL) status = EFI_NOT_FOUND;
	if (!EFI_ERROR(status)) {
		/* The new text goes behind the old text: the position after a seek to the end is the size. */
		status = file->SetPosition(file, ~0ull);
		if (!EFI_ERROR(status)) status = file->GetPosition(file, &size);
		if (!EFI_ERROR(status) && size > STAGE_LOG_FILE_MAX) {
			/* Delete closes the handle. The file starts again with this start. */
			(void)file->Delete(file);
			file = NULL;
			status = stage->root->Open(stage->root, &file, path, mode, 0);
			if (!EFI_ERROR(status) && file == NULL) status = EFI_NOT_FOUND;
		}
		if (!EFI_ERROR(status)) status = file->Write(file, &bytes, log_text);
		/* A volume that is full takes fewer bytes and can still answer with success. */
		if (!EFI_ERROR(status) && bytes != log_used) status = EFI_VOLUME_FULL;
		if (!EFI_ERROR(status)) status = file->Flush(file);
		if (file != NULL) file->Close(file);
	}
	if (EFI_ERROR(status)) {
		/* One line on the console, and no second try: a volume that does not take the file stays as it is. */
		log_mode = LOG_OFF;
		log_used = 0;
		stage_sayf(stage, NULL, "the log file did not write, status 0x%llx: no more log for this start",
			   (unsigned long long)status);
		return;
	}
	log_used = 0;
}

/* Reads the load option log= and writes what the console said before the option was known. */
static void log_start(struct stage *stage)
{
	log_mode = (int)stage_option_choice(&stage->options, "log", log_modes, 3u, LOG_AT_END);
	if (log_mode != LOG_OFF && (stage->root == NULL || !stage->image_file_known)) {
		log_mode = LOG_OFF;
		stage_say(stage, NULL, "no volume or no path of this application: no log file");
	}
	if (log_mode == LOG_OFF) {
		log_used = 0;
		return;
	}
	if (log_mode == LOG_EACH_LINE) stage_log_flush(stage);
}

/* One line: to the console, and to the log when to_log is set. */
static void say(struct stage *stage, const char *module, const char *line, int to_log)
{
	static const char prefix[] = "bc250-stage: ";
	CHAR16 wide[STAGE_LINE_CHARS + 64u];
	size_t n = 0, i;
	for (i = 0; prefix[i]; i++) wide[n++] = (unsigned char)prefix[i];
	if (module != NULL) {
		for (i = 0; module[i] && i < 24u; i++) wide[n++] = (unsigned char)module[i];
		wide[n++] = ':';
		wide[n++] = ' ';
	}
	for (i = 0; line[i] && i < STAGE_LINE_CHARS; i++) wide[n++] = (unsigned char)line[i];
	wide[n++] = '\r';
	wide[n++] = '\n';
	wide[n] = 0;
	if (stage->st->ConOut != NULL) stage->st->ConOut->OutputString(stage->st->ConOut, wide);
	if (!to_log) return;
	log_keep(stage, wide, n);
	if (log_mode == LOG_EACH_LINE) stage_log_flush(stage);
}

void stage_say(struct stage *stage, const char *module, const char *line)
{
	say(stage, module, line, 1);
}

void stage_say_console(struct stage *stage, const char *module, const char *line)
{
	say(stage, module, line, 0);
}

void stage_sayf(struct stage *stage, const char *module, const char *format, ...)
{
	char line[STAGE_LINE_CHARS];
	va_list args;
	va_start(args, format);
	efl_vformat(line, sizeof(line), format, args);
	va_end(args);
	stage_say(stage, module, line);
}

/* ---- memory and firmware tables ------------------------------------------------------------------ */

void *stage_alloc(struct stage *stage, size_t bytes)
{
	VOID *memory = NULL;
	if (EFI_ERROR(stage->bs->AllocatePool(EfiLoaderData, bytes, &memory))) return NULL;
	return memory;
}

void stage_free(struct stage *stage, void *memory)
{
	if (memory != NULL) stage->bs->FreePool(memory);
}

static const unsigned char *map_identity(void *context, unsigned long long address, unsigned long bytes)
{
	/* Boot services run with physical memory mapped one to one (UEFI Specification section 2.3.4). */
	(void)context;
	(void)bytes;
	return address != 0 ? (const unsigned char *)(UINTN)address : NULL;
}

static void *config_table(const struct stage *stage, const EFI_GUID *guid)
{
	UINTN i;
	for (i = 0; i < stage->st->NumberOfTableEntries; i++)
		if (!memcmp(&stage->st->ConfigurationTable[i].VendorGuid, guid, sizeof(*guid)))
			return stage->st->ConfigurationTable[i].VendorTable;
	return NULL;
}

void stage_acpi(struct stage *stage, struct efl_acpi *acpi)
{
	void *rsdp = config_table(stage, &acpi20_guid);
	if (rsdp == NULL) rsdp = config_table(stage, &acpi10_guid);
	acpi->map = map_identity;
	acpi->context = NULL;
	acpi->rsdp = (UINTN)rsdp;
}

unsigned char *stage_smbios_rsmb(struct stage *stage, size_t *bytes)
{
	struct efl_smbios_table table;
	const unsigned char *data;
	unsigned char *rsmb;
	size_t extent;
	void *entry;
	*bytes = 0;
	entry = config_table(stage, &smbios3_guid);
	if (entry == NULL || !efl_smbios_entry(map_identity, NULL, (UINTN)entry, 1, &table)) {
		entry = config_table(stage, &smbios_guid);
		if (entry == NULL || !efl_smbios_entry(map_identity, NULL, (UINTN)entry, 0, &table)) return NULL;
	}
	if (table.max_bytes > STAGE_SMBIOS_MAX_BYTES) table.max_bytes = STAGE_SMBIOS_MAX_BYTES;
	data = map_identity(NULL, table.address, table.max_bytes);
	extent = efl_smbios_extent(data, table.max_bytes);
	if (extent == 0) return NULL;
	rsmb = stage_alloc(stage, EFL_RSMB_HEADER_BYTES + extent);
	if (rsmb == NULL) return NULL;
	*bytes = efl_smbios_rsmb(data, extent, table.major, table.minor, rsmb, EFL_RSMB_HEADER_BYTES + extent);
	return rsmb;
}

/* ---- the board identity -------------------------------------------------------------------------- */

static int identify_board(struct stage *stage)
{
	EFI_HANDLE *handles = NULL;
	UINTN count = 0, i;
	unsigned char *rsmb;
	size_t rsmb_bytes;
	unsigned matches = 0, candidates = 0, reason = 0, bios = 0;
	unsigned char machine[16];

	rsmb = stage_smbios_rsmb(stage, &rsmb_bytes);
	if (rsmb == NULL) {
		stage_say(stage, NULL, "no SMBIOS table that ends with a type 127 structure");
		return 0;
	}
	if (EFI_ERROR(stage->bs->LocateHandleBuffer(ByProtocol, &pci_io_guid, NULL, &count, &handles))) count = 0;
	for (i = 0; i < count; i++) {
		EFI_PCI_IO_PROTOCOL *pci = NULL;
		UINT32 config[STAGE_PCI_CONFIG_BYTES / 4u];
		if (EFI_ERROR(stage->bs->HandleProtocol(handles[i], &pci_io_guid, (VOID **)&pci)) || pci == NULL) continue;
		if (EFI_ERROR(pci->Pci.Read(pci, EfiPciIoWidthUint32, 0, STAGE_PCI_CONFIG_BYTES / 4u, config))) continue;
		/* The PCI function first, without the firmware table, as driver/kmd/board_memory_identity.c does:
		 * reason 3 (FIRMWARE) says that the function matched. */
		(void)BoardIdentitySelect((const unsigned char *)config, STAGE_PCI_CONFIG_BYTES, NULL, 0, &reason);
		if (reason != 3u) continue;
		candidates++;
		if (BoardIdentitySelect((const unsigned char *)config, STAGE_PCI_CONFIG_BYTES, rsmb, rsmb_bytes, &reason) ==
		    1u) {
			UINTN segment = 0, bus = 0, device = 0, function = 0;
			matches++;
			stage->gpu = pci;
			memcpy(stage->gpu_config, config, STAGE_PCI_CONFIG_BYTES);
			if (!EFI_ERROR(pci->GetLocation(pci, &segment, &bus, &device, &function)))
				stage_sayf(stage, NULL, "GPU function at %u:%u:%u.%u", (unsigned)segment, (unsigned)bus,
					   (unsigned)device, (unsigned)function);
		}
	}
	if (handles != NULL) stage->bs->FreePool(handles);
	if (matches == 1u && BoardIdentityQualification(rsmb, rsmb_bytes, &bios, machine))
		stage_sayf(stage, NULL, "board BC-250, BIOS P%u.00", bios);
	memset(machine, 0, sizeof(machine));
	stage_free(stage, rsmb);
	if (matches != 1u) {
		stage_sayf(stage, NULL, "not a BC-250: %u matching GPU functions, %u with a matching SMBIOS table",
			   candidates, matches);
		stage->gpu = NULL;
		return 0;
	}
	return 1;
}

int stage_board_bc250(struct stage *stage)
{
	if (!stage->board_checked) {
		stage->board_bc250 = identify_board(stage);
		stage->board_checked = 1;
	}
	return stage->board_bc250;
}

/* ---- the volume of the stage --------------------------------------------------------------------- */

static void open_volume(struct stage *stage)
{
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
	if (efl_devpath_file((const unsigned char *)stage->loaded->FilePath, STAGE_DEVPATH_MAX_BYTES, stage->image_file,
			     STAGE_PATH_CHARS))
		stage->image_file_known = 1;
	else
		stage_say(stage, NULL, "the path of this application is not known: no file beside it can be used");
	if (EFI_ERROR(stage->bs->HandleProtocol(stage->loaded->DeviceHandle, &file_system_guid, (VOID **)&fs)) ||
	    fs == NULL || EFI_ERROR(fs->OpenVolume(fs, &stage->root)))
		stage->root = NULL;
}

int stage_sibling(struct stage *stage, const char *name, unsigned short *out, size_t capacity)
{
	return stage->image_file_known && efl_sibling_path(stage->image_file, name, out, capacity);
}

enum stage_read stage_read_file(struct stage *stage, const unsigned short *path, size_t max_bytes,
				unsigned char **data, size_t *bytes)
{
	EFI_FILE_PROTOCOL *file = NULL;
	EFI_STATUS status;
	UINT64 size = 0;
	UINTN got;
	unsigned char *buffer;
	*data = NULL;
	*bytes = 0;
	if (stage->root == NULL || path == NULL) return STAGE_READ_ABSENT;
	status = stage->root->Open(stage->root, &file, path, EFI_FILE_MODE_READ, 0);
	if (status == EFI_NOT_FOUND) return STAGE_READ_ABSENT;
	if (EFI_ERROR(status) || file == NULL) return STAGE_READ_FAILED;
	/* The size: the position after a seek to the end (UEFI 2.10, EFI_FILE_PROTOCOL.SetPosition). */
	if (EFI_ERROR(file->SetPosition(file, ~0ull)) || EFI_ERROR(file->GetPosition(file, &size)) ||
	    EFI_ERROR(file->SetPosition(file, 0)) || size == 0 || size > max_bytes) {
		file->Close(file);
		return STAGE_READ_FAILED;
	}
	buffer = stage_alloc(stage, (size_t)size);
	if (buffer == NULL) {
		file->Close(file);
		return STAGE_READ_FAILED;
	}
	got = (UINTN)size;
	status = file->Read(file, &got, buffer);
	file->Close(file);
	if (EFI_ERROR(status) || got != size) {
		stage_free(stage, buffer);
		return STAGE_READ_FAILED;
	}
	*data = buffer;
	*bytes = (size_t)size;
	return STAGE_READ_OK;
}

/* ---- Windows ------------------------------------------------------------------------------------- */

static void unwind_modules(struct stage *stage)
{
	size_t i;
	for (i = MODULES; i-- > 0;)
		if (module_on[i] && modules[i]->unwind != NULL) modules[i]->unwind(stage);
}

/* Sets next_image from the load option, or to the Windows Boot Manager. A value that is not an absolute path is
 * refused, and the default stays. */
static void choose_next(struct stage *stage)
{
	const char *text = stage_option_text(&stage->options, "next");
	size_t i;
	if (text != NULL && text[0] != '\\') {
		stage_say(stage, NULL, "the load option next= is not a path that starts with a backslash: ignored");
		text = NULL;
	}
	if (text == NULL) text = default_next;
	for (i = 0; text[i] && i + 1u < STAGE_OPTION_CHARS; i++) next_image[i] = (unsigned char)text[i];
	next_image[i] = 0;
	stage_sayf(stage, NULL, "next image: %s", text);
}

static EFI_STATUS start_next(struct stage *stage)
{
	EFI_DEVICE_PATH_PROTOCOL *device = NULL;
	unsigned char *path, *image = NULL;
	EFI_HANDLE windows = NULL;
	EFI_STATUS status;
	enum stage_gate gate = STAGE_GATE_PASS;
	size_t bytes, image_bytes = 0, i;
	UINTN capacity;

	status = stage->bs->HandleProtocol(stage->loaded->DeviceHandle, &device_path_guid, (VOID **)&device);
	if (EFI_ERROR(status) || device == NULL) {
		stage_sayf(stage, NULL, "no device path for the boot partition, status 0x%llx", (unsigned long long)status);
		return EFI_ERROR(status) ? status : EFI_NOT_FOUND;
	}
	capacity = STAGE_DEVPATH_MAX_BYTES + sizeof(next_image) + 8u;
	path = stage_alloc(stage, capacity);
	if (path == NULL) return EFI_OUT_OF_RESOURCES;
	bytes = efl_devpath_with_file((const unsigned char *)device, STAGE_DEVPATH_MAX_BYTES, next_image, path, capacity);
	if (bytes == 0) {
		stage_free(stage, path);
		stage_say(stage, NULL, "the device path of the boot partition is malformed");
		return EFI_LOAD_ERROR;
	}

	/* The modules that have a say see the bytes of the image first. */
	for (i = 0; i < MODULES; i++)
		if (module_on[i] && modules[i]->gate != NULL) break;
	if (i < MODULES) {
		if (stage_read_file(stage, next_image, STAGE_IMAGE_MAX_BYTES, &image, &image_bytes) != STAGE_READ_OK)
			stage_say(stage, NULL, "the file of the next image did not read for the check");
		for (i = 0; i < MODULES; i++)
			if (module_on[i] && modules[i]->gate != NULL) {
				enum stage_gate answer = modules[i]->gate(stage, image, image_bytes);
				if (answer > gate) gate = answer;
			}
	}
	if (gate == STAGE_GATE_DENY) {
		stage_say(stage, NULL, "a module refused the next image: not started");
		stage_free(stage, image);
		stage_free(stage, path);
		return EFI_ACCESS_DENIED;
	}

	stage_say(stage, NULL, "starting the next image");
	if (gate == STAGE_GATE_BIND && image != NULL)
		/* A load from memory: the bytes that start are the bytes that were checked. The device path gives the
		 * image its device handle and its file path (UEFI 2.10, EFI_BOOT_SERVICES.LoadImage). */
		status = stage->bs->LoadImage(0, stage->image, (EFI_DEVICE_PATH_PROTOCOL *)path, image, image_bytes, &windows);
	else
		status = stage->bs->LoadImage(0, stage->image, (EFI_DEVICE_PATH_PROTOCOL *)path, NULL, 0, &windows);
	stage_free(stage, path);
	for (i = 0; i < MODULES; i++)
		if (module_on[i] && modules[i]->loaded != NULL) modules[i]->loaded(stage, status);
	stage_free(stage, image);
	if (EFI_ERROR(status)) {
		stage_sayf(stage, NULL, "the next image did not load, status 0x%llx", (unsigned long long)status);
		/* EFI_SECURITY_VIOLATION leaves a loaded image that must not start. */
		if (windows != NULL) stage->bs->UnloadImage(windows);
		return status;
	}
	/* The last moment of the stage before the next image: the log must be on the volume now. */
	stage_log_flush(stage);
	status = stage->bs->StartImage(windows, NULL, NULL);
	stage_sayf(stage, NULL, "the next image returned, status 0x%llx", (unsigned long long)status);
	return status;
}

/* ---- the entry point ----------------------------------------------------------------------------- */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	static struct stage stage;
	EFI_STATUS status;
	unsigned unclaimed;
	int probe;
	size_t i;

	memset(&stage, 0, sizeof(stage));
	log_used = 0;
	log_lost = 0;
	log_mode = LOG_UNDECIDED;
	stage.image = image;
	stage.st = st;
	stage.bs = st->BootServices;
	stage_say(&stage, NULL, "BC-250 stage, version " STAGE_VERSION);
	if (EFI_ERROR(stage.bs->HandleProtocol(image, &loaded_image_guid, (VOID **)&stage.loaded)) ||
	    stage.loaded == NULL) {
		stage_say(&stage, NULL, "no loaded image protocol: cannot find the boot partition");
		return EFI_LOAD_ERROR;
	}
	stage_options_parse(stage.loaded->LoadOptions, stage.loaded->LoadOptionsSize / 2u, &stage.options);
	open_volume(&stage);
	log_start(&stage);
	probe = stage_option_flag(&stage.options, "probe", 0);

	for (i = 0; i < MODULES; i++) {
		module_on[i] = stage_option_flag(&stage.options, modules[i]->off_switch, 1);
		if (!module_on[i]) {
			stage_sayf(&stage, modules[i]->name, "off by the load option %s=0", modules[i]->off_switch);
			/* probe=1: a module that is off says what the firmware has for it, and changes nothing. */
			if (probe && modules[i]->probe != NULL) modules[i]->probe(&stage);
			continue;
		}
		modules[i]->run(&stage);
	}
	/* The core reads its own option before the count, so next= is not in the count. */
	choose_next(&stage);
	unclaimed = stage_options_unclaimed(&stage.options);
	if (unclaimed) stage_sayf(&stage, NULL, "%u load option(s) not understood and ignored", unclaimed);

	status = start_next(&stage);
	unwind_modules(&stage);
	if (log_lost) stage_sayf(&stage, NULL, "%u console line(s) did not fit in the log", log_lost);
	/* The last line of the stage. A log that ends with it says that the stage returned to its loader, and that a
	 * stop after it is in the loader or in the image that the loader starts next. */
	stage_sayf(&stage, NULL, "returning to the loader, status 0x%llx", (unsigned long long)(EFI_ERROR(status) ? status : EFI_SUCCESS));
	stage_log_flush(&stage);
	if (stage.root != NULL) stage.root->Close(stage.root);
	stage.root = NULL;
	return EFI_ERROR(status) ? status : EFI_SUCCESS;
}
