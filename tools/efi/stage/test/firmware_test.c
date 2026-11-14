/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host tests of the UEFI bindings of the BC-250 stage: the core (stage/src/main.c), the ACPI module binding and
 * the Secure Boot module binding. They run the real efi_main against a model of the firmware in this file: a
 * console, pool memory, a volume with files, the handle database with ReinstallProtocolInterface, LoadImage with
 * its call of the Security2 protocol, the ACPI table and SDT protocols over real root tables, a PCI function and
 * an SMBIOS table. The volume takes one written file, the log of the stage.
 *
 * The model follows the edk2 behaviour that the bindings rely on (commit 8628578dacd3): LoadImage asks the
 * Security2 protocol that is installed at that moment, EFI_ACCESS_DENIED gives no image handle, and
 * EFI_SECURITY_VIOLATION gives one. It is a model, not the AMI firmware: the lab steps of the design page stay
 * necessary. The core unlock binding is not run here, because it reads CPU registers: each test gives unlock=0.
 * Its probe runs here: the dump and the identification of the register BAR over a model of the PCI I/O and the
 * PCI Root Bridge I/O protocols.
 */
#include <malloc.h>
#include <setjmp.h>
#include <stdio.h>
#include "efl_test.h"
#include "efi.h"
#include "efl_text.h"
#include "stage.h"
#include "cu_bar.h"
#include "sb_crc32.h"
#include "sb_windows.h"
#include "engine_model.h"
#include "efl_pe.h"
#include "handoff.h"
#include "efl_sha256.h"
#include "tl_log.h"
#include "tl_tpm2.h"

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st);

/* ---- the model ----------------------------------------------------------------------------------- */

#define FW_FILES	8u
#define FW_TABLES	24u
#define FW_PROTOCOLS	12u
#define FW_LOG_BYTES	(256u * 1024u)
#define SDT_HEADER	36u
#define FW_BARS		6u
#define FW_BAR5		0xFE800000ull	/* test data: the address of BAR5 in the model */
#define FW_PAGE_BLOCKS	16u
#define FW_NEW_HANDLES	4u
#define FW_CONFIG	6u

struct fw_file { char path[96]; unsigned char *data; size_t bytes; };
struct fw_open { EFI_FILE_PROTOCOL protocol; struct fw_file *file; UINT64 position; };
struct fw_protocol { EFI_HANDLE handle; EFI_GUID guid; VOID *interface; };
struct fw_table { unsigned char *bytes; size_t length; UINTN key; };

static struct {
	char log[FW_LOG_BYTES];
	size_t log_bytes;
	long pool_live;
	struct fw_file file[FW_FILES];
	unsigned files;
	int read_only;			/* the volume takes no write */
	unsigned write_budget;		/* not 0: the volume takes this many writes and then answers EFI_VOLUME_FULL */
	unsigned file_writes, file_deletes;
	int log_at_start;		/* at StartImage the log file had the line "starting the next image" */
	unsigned extra_checks;		/* LoadImage asks the Security2 protocol this many more times */
	struct fw_protocol protocol[FW_PROTOCOLS];
	unsigned protocols;
	/* LoadImage and StartImage. */
	unsigned loads, loads_from_buffer, starts, unloads, firmware_checks;
	int buffer_is_file;		/* the last load from a buffer had the bytes of the file */
	int stage_check_at_start;	/* the Security2 protocol at StartImage was not the firmware's */
	EFI_STATUS firmware_check_status, start_status;
	/* The handle database. */
	unsigned reinstalls;
	int refuse_put_back;		/* ReinstallProtocolInterface refuses the firmware's own Security2 */
	int child_hooks;		/* the started image installs a Security2 of its own and does not unwind */
	unsigned stalls, resets;
	jmp_buf reset;
	/* The ACPI tables. */
	struct fw_table table[FW_TABLES];
	unsigned tables;
	UINTN next_key;
	unsigned installs, uninstalls;
	unsigned char rsdp[36], *xsdt, *dsdt;
	unsigned char pci[64];
	unsigned char *variable_dbx;	/* the dbx variable of the firmware, or NULL */
	size_t variable_dbx_bytes;
	unsigned char *variable_secureboot;	/* a SecureBoot variable of the firmware, or NULL */
	size_t variable_secureboot_bytes;
	/* The runtime side of sb.windows: the event, the conversion and two refusals that a test can ask for. */
	int allocate_fails, create_event_fails;
	unsigned converts, events_open;
	EFI_EVENT_NOTIFY event_notify;
	VOID *event_context;
	/* The register BAR: the descriptor list of each BarIndex, the index that Mem.Read takes, the windows of the
	 * root bridge, and the value of the control register. */
	unsigned char bar_list[FW_BARS][64];
	size_t bar_bytes[FW_BARS];
	unsigned register_index;
	unsigned char bridge_list[64];
	size_t bridge_bytes;
	unsigned control_value;
	unsigned mem_reads, mem_writes, bridge_reads;
	/* The TPM module: pages, the event of ExitBootServices, the handles of new protocols, a real stall. */
	void *page_block[FW_PAGE_BLOCKS];
	EFI_MEMORY_TYPE page_type[FW_PAGE_BLOCKS];
	size_t page_bytes[FW_PAGE_BLOCKS];
	unsigned page_blocks;
	EFI_EVENT_NOTIFY exit_notify;
	VOID *exit_context;
	int new_handle[FW_NEW_HANDLES];
	unsigned new_handles;
	int real_stall;
	void (*on_start)(void);
} fw;

static int fw_handle_image, fw_handle_device, fw_handle_security, fw_handle_gpu, fw_handle_child, fw_handle_bridge;
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL fw_bridge;
static const EFI_GUID guid_bridge = EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
static EFI_SYSTEM_TABLE fw_st;
static EFI_BOOT_SERVICES fw_bs;
static EFI_RUNTIME_SERVICES fw_rt;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL fw_console;
static EFI_LOADED_IMAGE_PROTOCOL fw_loaded;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fw_fs;
static EFI_SECURITY2_ARCH_PROTOCOL fw_security2, fw_child_security2;
static EFI_ACPI_TABLE_PROTOCOL fw_acpi_table;
static EFI_ACPI_SDT_PROTOCOL fw_acpi_sdt;
static EFI_PCI_IO_PROTOCOL fw_pci;
static EFI_CONFIGURATION_TABLE fw_config[FW_CONFIG];
static unsigned char fw_device_path[4] = { EFI_DP_TYPE_END, EFI_DP_END_ENTIRE, 4, 0 };
static unsigned char fw_image_path[160];
static unsigned char fw_smbios_entry[24], fw_smbios[512];
static CHAR16 fw_options[128];

static const EFI_GUID guid_loaded = EFI_LOADED_IMAGE_PROTOCOL_GUID, guid_device_path = EFI_DEVICE_PATH_PROTOCOL_GUID;
static const EFI_GUID guid_fs = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, guid_security2 = EFI_SECURITY2_ARCH_PROTOCOL_GUID;
static const EFI_GUID guid_acpi_table = EFI_ACPI_TABLE_PROTOCOL_GUID, guid_acpi_sdt = EFI_ACPI_SDT_PROTOCOL_GUID;
static const EFI_GUID guid_pci = EFI_PCI_IO_PROTOCOL_GUID, guid_acpi20 = EFI_ACPI_20_TABLE_GUID;
static const EFI_GUID guid_smbios3 = EFI_SMBIOS3_TABLE_GUID;

#define LOG_FILE "\\EFI\\BC250\\bc250-stage.log"

static void narrow(const CHAR16 *wide, char *out, size_t capacity)
{
	size_t i;
	for (i = 0; i + 1u < capacity && wide[i]; i++) out[i] = (char)wide[i];
	out[i] = 0;
}

static void put64(unsigned char *p, unsigned long long v)
{
	unsigned i;
	for (i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8u * i));
}

static void fix_sum(unsigned char *bytes, size_t length, size_t at)
{
	unsigned char sum = 0;
	size_t i;
	bytes[at] = 0;
	for (i = 0; i < length; i++) sum = (unsigned char)(sum + bytes[i]);
	bytes[at] = (unsigned char)(0u - sum);
}

/* -- console, memory, time -- */

static EFI_STATUS EFIAPI fw_output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *this, const CHAR16 *text)
{
	(void)this;
	for (; *text; text++)
		if (fw.log_bytes + 1u < FW_LOG_BYTES) fw.log[fw.log_bytes++] = (char)*text;
	fw.log[fw.log_bytes] = 0;
	return EFI_SUCCESS;
}

/* Memory of the type EfiRuntimeServicesCode holds code that the test calls, so the model gives it pages that can be
 * executed. A firmware's pool of that type is executable too (UEFI 2.10, AllocatePool). The two Win32 functions come
 * from kernel32 through their own declarations: the header would collide with the type names of efi.h. */
#ifdef _WIN32
__declspec(dllimport) void *__stdcall VirtualAlloc(void *address, size_t size, unsigned int type, unsigned int protect);
__declspec(dllimport) int __stdcall VirtualFree(void *address, size_t size, unsigned int type);
/* The real wait of fw_stall, with the same reason for the own declarations. */
__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long *count);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long *frequency);
#endif
#define FW_MEM_COMMIT		0x00001000u
#define FW_MEM_RESERVE		0x00002000u
#define FW_MEM_RELEASE		0x00008000u
#define FW_PAGE_EXECUTE_READWRITE 0x00000040u
#define FW_EXEC_BLOCKS 4u
static struct { void *pointer; unsigned index; } fw_exec[FW_EXEC_BLOCKS];

static int fw_exec_find(const void *pointer)
{
	unsigned i;
	for (i = 0; i < FW_EXEC_BLOCKS; i++)
		if (fw_exec[i].pointer == pointer) return (int)i;
	return -1;
}

static EFI_STATUS EFIAPI fw_allocate(EFI_MEMORY_TYPE type, UINTN size, VOID **buffer)
{
	if (fw.allocate_fails) return EFI_OUT_OF_RESOURCES;
	if (size == 0u) size = 1u;
	if (type == EfiRuntimeServicesCode) {
		unsigned slot = (unsigned)fw_exec_find(NULL);
		if (slot >= FW_EXEC_BLOCKS) return EFI_OUT_OF_RESOURCES;
		*buffer = VirtualAlloc(NULL, (size_t)size, FW_MEM_COMMIT | FW_MEM_RESERVE, FW_PAGE_EXECUTE_READWRITE);
		if (*buffer == NULL) return EFI_OUT_OF_RESOURCES;
		fw_exec[slot].pointer = *buffer;
		fw.pool_live++;
		return EFI_SUCCESS;
	}
	*buffer = malloc((size_t)size);
	if (*buffer == NULL) return EFI_OUT_OF_RESOURCES;
	fw.pool_live++;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fw_free(VOID *buffer)
{
	int at = fw_exec_find(buffer);
	if (at >= 0) {
		fw_exec[at].pointer = NULL;
		(void)VirtualFree(buffer, 0, FW_MEM_RELEASE);
	} else {
		free(buffer);
	}
	fw.pool_live--;
	return EFI_SUCCESS;
}

/* The runtime side: one event of the address change, a pointer conversion that keeps the model's one address
 * space, and the table checksum. */
static EFI_STATUS EFIAPI fw_create_event(UINT32 type, UINTN notify_tpl, EFI_EVENT_NOTIFY notify, VOID *context,
					 EFI_EVENT *event)
{
	(void)notify_tpl;
	if (fw.create_event_fails) return EFI_OUT_OF_RESOURCES;
	if (type == EVT_SIGNAL_EXIT_BOOT_SERVICES && notify != NULL && event != NULL) {
		if (fw.exit_notify != NULL) return EFI_OUT_OF_RESOURCES;	/* the model keeps one */
		fw.exit_notify = notify;
		fw.exit_context = context;
		*event = &fw_bs;
		return EFI_SUCCESS;
	}
	if (type != EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE || notify == NULL || event == NULL) return EFI_INVALID_PARAMETER;
	fw.event_notify = notify;
	fw.event_context = context;
	fw.events_open++;
	*event = &fw_rt;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fw_close_event(EFI_EVENT event)
{
	if (event == NULL) return EFI_INVALID_PARAMETER;
	if (event == &fw_bs) {
		fw.exit_notify = NULL;
		fw.exit_context = NULL;
		return EFI_SUCCESS;
	}
	if (fw.events_open > 0u) fw.events_open--;
	fw.event_notify = NULL;
	fw.event_context = NULL;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fw_convert_pointer(UINTN disposition, VOID **address)
{
	(void)disposition;
	if (address == NULL || *address == NULL) return EFI_INVALID_PARAMETER;
	/* The model keeps one address space: the conversion counts, and the pointer stays usable. */
	fw.converts++;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fw_crc32(VOID *data, UINTN bytes, UINT32 *crc)
{
	if (crc == NULL) return EFI_INVALID_PARAMETER;
	*crc = (UINT32)sb_crc32(data, (size_t)bytes);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fw_stall(UINTN microseconds)
{
	fw.stalls++;
	/* A real wait only where a test asks for one: the TPM module measures the time stamp counter over a stall. */
	if (fw.real_stall) {
		long long frequency, start, now;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&start);
		do QueryPerformanceCounter(&now);
		while ((unsigned long long)(now - start) * 1000000ull < (unsigned long long)microseconds * (unsigned long long)frequency);
	}
	return EFI_SUCCESS;
}

/* -- pages, the protocol interfaces that a module adds, the configuration tables -- */

static EFI_STATUS EFIAPI fw_allocate_pages(EFI_ALLOCATE_TYPE type, EFI_MEMORY_TYPE memory, UINTN pages, EFI_PHYSICAL_ADDRESS *address)
{
	void *block;
	if (type != AllocateAnyPages || pages == 0 || address == NULL || fw.page_blocks == FW_PAGE_BLOCKS) return EFI_INVALID_PARAMETER;
	if (fw.allocate_fails) return EFI_OUT_OF_RESOURCES;
	block = _aligned_malloc((size_t)pages * 4096u, 4096u);
	if (block == NULL) return EFI_OUT_OF_RESOURCES;
	memset(block, 0xA5, (size_t)pages * 4096u);	/* the firmware does not clear pages */
	fw.page_block[fw.page_blocks] = block;
	fw.page_type[fw.page_blocks] = memory;
	fw.page_bytes[fw.page_blocks++] = (size_t)pages * 4096u;
	*address = (EFI_PHYSICAL_ADDRESS)(size_t)block;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fw_free_pages(EFI_PHYSICAL_ADDRESS address, UINTN pages)
{
	unsigned i;
	for (i = 0; i < fw.page_blocks; i++)
		if (fw.page_block[i] == (void *)(size_t)address) {
			if (fw.page_bytes[i] != (size_t)pages * 4096u) failures++;	/* the caller gives back what it took */
			memset(fw.page_block[i], 0xDD, fw.page_bytes[i]);
			_aligned_free(fw.page_block[i]);
			fw.page_blocks--;
			fw.page_block[i] = fw.page_block[fw.page_blocks];
			fw.page_type[i] = fw.page_type[fw.page_blocks];
			fw.page_bytes[i] = fw.page_bytes[fw.page_blocks];
			return EFI_SUCCESS;
		}
	return EFI_NOT_FOUND;
}

/* The memory type of the pages at this address, or -1. */
static int page_type(const void *address)
{
	unsigned i;
	for (i = 0; i < fw.page_blocks; i++)
		if (fw.page_block[i] == address) return (int)fw.page_type[i];
	return -1;
}

static void add_protocol(EFI_HANDLE handle, const EFI_GUID *guid, VOID *interface);
static struct fw_protocol *find_protocol(EFI_HANDLE handle, const EFI_GUID *guid);

static EFI_STATUS EFIAPI fw_install_protocol(EFI_HANDLE *handle, const EFI_GUID *guid, EFI_INTERFACE_TYPE type, VOID *interface)
{
	if (handle == NULL || guid == NULL || type != EFI_NATIVE_INTERFACE || fw.protocols == FW_PROTOCOLS) return EFI_INVALID_PARAMETER;
	if (*handle == NULL) *handle = &fw.new_handle[fw.new_handles++ % FW_NEW_HANDLES];
	if (find_protocol(*handle, guid) != NULL) return EFI_INVALID_PARAMETER;
	add_protocol(*handle, guid, interface);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fw_uninstall_protocol(EFI_HANDLE handle, const EFI_GUID *guid, VOID *interface)
{
	struct fw_protocol *p = find_protocol(handle, guid);
	if (p == NULL || p->interface != interface) return EFI_NOT_FOUND;
	memmove(p, p + 1, (size_t)(&fw.protocol[fw.protocols] - (p + 1)) * sizeof(*p));
	fw.protocols--;
	return EFI_SUCCESS;
}

/* InstallConfigurationTable (UEFI 2.10, section 7.3.16): a NULL table removes the entry. The model keeps the
 * module's tables beside its own two, which stay first. */
static EFI_STATUS EFIAPI fw_install_configuration(const EFI_GUID *guid, VOID *table)
{
	unsigned i;
	for (i = 0; i < fw_st.NumberOfTableEntries; i++)
		if (!memcmp(&fw_config[i].VendorGuid, guid, sizeof(*guid))) {
			if (table != NULL) {
				fw_config[i].VendorTable = table;
				return EFI_SUCCESS;
			}
			memmove(&fw_config[i], &fw_config[i + 1u], (fw_st.NumberOfTableEntries - i - 1u) * sizeof(fw_config[0]));
			fw_st.NumberOfTableEntries--;
			return EFI_SUCCESS;
		}
	if (table == NULL) return EFI_NOT_FOUND;
	if (fw_st.NumberOfTableEntries == FW_CONFIG) return EFI_OUT_OF_RESOURCES;
	fw_config[fw_st.NumberOfTableEntries].VendorGuid = *guid;
	fw_config[fw_st.NumberOfTableEntries++].VendorTable = table;
	return EFI_SUCCESS;
}

static VOID *configuration_table(const EFI_GUID *guid)
{
	unsigned i;
	for (i = 0; i < fw_st.NumberOfTableEntries; i++)
		if (!memcmp(&fw_config[i].VendorGuid, guid, sizeof(*guid))) return fw_config[i].VendorTable;
	return NULL;
}

static VOID EFIAPI fw_reset_system(EFI_RESET_TYPE type, EFI_STATUS status, UINTN size, VOID *data)
{
	(void)type; (void)status; (void)size; (void)data;
	fw.resets++;
	longjmp(fw.reset, 1);	/* a reset does not return */
}

/* -- the volume -- */

static struct fw_file *find_file(const char *path)
{
	unsigned i;
	for (i = 0; i < fw.files; i++)
		if (!strcmp(fw.file[i].path, path)) return &fw.file[i];
	return NULL;
}

static EFI_STATUS EFIAPI file_close(EFI_FILE_PROTOCOL *this)
{
	free(this);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_read(EFI_FILE_PROTOCOL *this, UINTN *size, VOID *buffer)
{
	struct fw_open *open = (struct fw_open *)this;
	UINT64 left = open->file->bytes - open->position;
	if (*size > left) *size = left;
	memcpy(buffer, open->file->data + open->position, *size);
	open->position += *size;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_get_position(EFI_FILE_PROTOCOL *this, UINT64 *position)
{
	*position = ((struct fw_open *)this)->position;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_set_position(EFI_FILE_PROTOCOL *this, UINT64 position)
{
	struct fw_open *open = (struct fw_open *)this;
	open->position = position > open->file->bytes ? open->file->bytes : position;	/* ~0 is the end */
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_write(EFI_FILE_PROTOCOL *this, UINTN *size, VOID *buffer)
{
	struct fw_open *open = (struct fw_open *)this;
	size_t end = (size_t)open->position + *size;
	if (fw.write_budget && fw.file_writes >= fw.write_budget) return EFI_VOLUME_FULL;
	if (end > open->file->bytes) {
		open->file->data = realloc(open->file->data, end);
		open->file->bytes = end;
	}
	memcpy(open->file->data + open->position, buffer, *size);
	open->position = end;
	fw.file_writes++;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_flush(EFI_FILE_PROTOCOL *this)
{
	(void)this;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_delete(EFI_FILE_PROTOCOL *this)
{
	struct fw_file *file = ((struct fw_open *)this)->file;
	free(file->data);
	*file = fw.file[--fw.files];	/* the last file takes the place of the deleted one */
	fw.file_deletes++;
	free(this);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_open(EFI_FILE_PROTOCOL *this, EFI_FILE_PROTOCOL **out, const CHAR16 *name, UINT64 mode,
				   UINT64 attributes)
{
	static struct fw_file root = { "", NULL, 0 };
	struct fw_open *open;
	struct fw_file *file = &root;
	char path[96];
	(void)attributes;
	if (this != NULL) {
		narrow(name, path, sizeof(path));
		file = find_file(path);
		if (mode != EFI_FILE_MODE_READ) {
			/* The one file that the stage writes in these tests is its log. */
			if (strcmp(path, LOG_FILE) != 0) failures++;
			if (fw.read_only) return EFI_WRITE_PROTECTED;
			if (file == NULL && (mode & EFI_FILE_MODE_CREATE) && fw.files < FW_FILES) {
				file = &fw.file[fw.files++];
				snprintf(file->path, sizeof(file->path), "%s", path);
				file->data = malloc(1);
				file->bytes = 0;
			}
		}
		if (file == NULL) return EFI_NOT_FOUND;
	}
	open = calloc(1, sizeof(*open));
	open->protocol.Open = file_open;
	open->protocol.Close = file_close;
	open->protocol.Delete = file_delete;
	open->protocol.Write = file_write;
	open->protocol.Flush = file_flush;
	open->protocol.Read = file_read;
	open->protocol.GetPosition = file_get_position;
	open->protocol.SetPosition = file_set_position;
	open->file = file;
	*out = &open->protocol;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fs_open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *this, EFI_FILE_PROTOCOL **root)
{
	(void)this;
	return file_open(NULL, root, NULL, EFI_FILE_MODE_READ, 0);
}

/* 1 when the file is there and holds this text. */
static int file_has(const char *path, const char *text)
{
	struct fw_file *file = find_file(path);
	size_t n = strlen(text), i;
	if (file == NULL) return 0;
	for (i = 0; i + n <= file->bytes; i++)
		if (!memcmp(file->data + i, text, n)) return 1;
	return 0;
}

/* The offset of the first copy of the text in the file, or -1. */
static long file_at(const char *path, const char *text)
{
	struct fw_file *file = find_file(path);
	size_t n = strlen(text), i;
	if (file == NULL) return -1;
	for (i = 0; i + n <= file->bytes; i++)
		if (!memcmp(file->data + i, text, n)) return (long)i;
	return -1;
}

/* 1 when the file ends with this line and its line end. */
static int file_ends_with(const char *path, const char *line)
{
	struct fw_file *file = find_file(path);
	size_t n = strlen(line);
	if (file == NULL || file->bytes < n + 1u) return 0;
	return !memcmp(file->data + file->bytes - n - 1u, line, n) && file->data[file->bytes - 1u] == '\n';
}

/* -- the handle database -- */

static struct fw_protocol *find_protocol(EFI_HANDLE handle, const EFI_GUID *guid)
{
	unsigned i;
	for (i = 0; i < fw.protocols; i++)
		if ((handle == NULL || fw.protocol[i].handle == handle) && !memcmp(&fw.protocol[i].guid, guid, sizeof(*guid)))
			return &fw.protocol[i];
	return NULL;
}

static void add_protocol(EFI_HANDLE handle, const EFI_GUID *guid, VOID *interface)
{
	struct fw_protocol *p = &fw.protocol[fw.protocols++];
	p->handle = handle;
	p->guid = *guid;
	p->interface = interface;
}

static EFI_STATUS EFIAPI fw_handle_protocol(EFI_HANDLE handle, const EFI_GUID *guid, VOID **interface)
{
	struct fw_protocol *p = find_protocol(handle, guid);
	if (p == NULL) return EFI_UNSUPPORTED;
	*interface = p->interface;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fw_locate_protocol(const EFI_GUID *guid, VOID *registration, VOID **interface)
{
	struct fw_protocol *p = find_protocol(NULL, guid);
	(void)registration;
	if (p == NULL) return EFI_NOT_FOUND;
	*interface = p->interface;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fw_locate_handle_buffer(EFI_LOCATE_SEARCH_TYPE type, const EFI_GUID *guid, VOID *key,
						 UINTN *count, EFI_HANDLE **buffer)
{
	EFI_HANDLE *list = NULL;
	unsigned i, n = 0;
	(void)type; (void)key;
	fw_allocate(EfiBootServicesData, FW_PROTOCOLS * sizeof(EFI_HANDLE), (VOID **)&list);
	for (i = 0; i < fw.protocols; i++)
		if (!memcmp(&fw.protocol[i].guid, guid, sizeof(*guid))) list[n++] = fw.protocol[i].handle;
	if (n == 0) {
		fw_free(list);
		return EFI_NOT_FOUND;
	}
	*count = n;
	*buffer = list;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fw_reinstall(EFI_HANDLE handle, const EFI_GUID *guid, VOID *old, VOID *new)
{
	struct fw_protocol *p = find_protocol(handle, guid);
	if (p == NULL || p->interface != old) return EFI_NOT_FOUND;
	if (fw.refuse_put_back && new == &fw_security2) return EFI_ACCESS_DENIED;
	p->interface = new;
	fw.reinstalls++;
	return EFI_SUCCESS;
}

/* -- images -- */

static EFI_STATUS EFIAPI fw_firmware_check(const EFI_SECURITY2_ARCH_PROTOCOL *this, const EFI_DEVICE_PATH_PROTOCOL *file,
					   VOID *buffer, UINTN bytes, BOOLEAN boot_policy)
{
	(void)file; (void)buffer; (void)bytes; (void)boot_policy;
	if (this != &fw_security2) failures++;	/* the firmware's function gets the firmware's structure */
	fw.firmware_checks++;
	return fw.firmware_check_status;
}

static EFI_STATUS EFIAPI fw_load_image(BOOLEAN boot_policy, EFI_HANDLE parent, EFI_DEVICE_PATH_PROTOCOL *path,
				       VOID *buffer, UINTN size, EFI_HANDLE *image)
{
	struct fw_protocol *security = find_protocol(NULL, &guid_security2);
	struct fw_file *file;
	unsigned short wide[96];
	char name[96];
	EFI_STATUS status = EFI_SUCCESS;
	(void)parent;
	*image = NULL;
	fw.loads++;
	if (!efl_devpath_file((const unsigned char *)path, 4096, wide, 96)) return EFI_NOT_FOUND;
	narrow(wide, name, sizeof(name));
	file = find_file(name);
	if (buffer != NULL) {
		fw.loads_from_buffer++;
		fw.buffer_is_file = file != NULL && file->bytes == size && !memcmp(file->data, buffer, size);
	} else {
		if (file == NULL) return EFI_NOT_FOUND;
		buffer = file->data;
		size = file->bytes;
	}
	if (security != NULL) {
		EFI_SECURITY2_ARCH_PROTOCOL *check = security->interface;
		unsigned i;
		/* A firmware can ask more than one time for a load. Each call gives two console lines of the stage. */
		for (i = 0; i < fw.extra_checks; i++) (void)check->FileAuthentication(check, path, buffer, size, boot_policy);
		status = check->FileAuthentication(check, path, buffer, size, boot_policy);
	}
	if (status == EFI_ACCESS_DENIED) return status;
	*image = &fw_handle_child;
	return status == EFI_SUCCESS ? EFI_SUCCESS : EFI_SECURITY_VIOLATION;
}

static EFI_STATUS EFIAPI fw_start_image(EFI_HANDLE image, UINTN *exit_size, CHAR16 **exit_data)
{
	struct fw_protocol *security = find_protocol(NULL, &guid_security2);
	(void)exit_size; (void)exit_data;
	if (image != &fw_handle_child) failures++;
	fw.starts++;
	fw.log_at_start = file_has(LOG_FILE, "starting the next image");
	fw.stage_check_at_start = security != NULL && security->interface != &fw_security2;
	if (fw.child_hooks && security != NULL) security->interface = &fw_child_security2;
	if (fw.on_start != NULL) fw.on_start();	/* what the started image does, as a test gives it */
	return fw.start_status;
}

static EFI_STATUS EFIAPI fw_unload_image(EFI_HANDLE image)
{
	if (image != &fw_handle_child) failures++;
	fw.unloads++;
	return EFI_SUCCESS;
}

/* -- variables: read only -- */

static EFI_STATUS EFIAPI fw_get_variable(CHAR16 *name, EFI_GUID *vendor, UINT32 *attributes, UINTN *size, VOID *data)
{
	char text[16];
	(void)vendor; (void)attributes;
	narrow(name, text, sizeof(text));
	if (strcmp(text, "SecureBoot") == 0 && fw.variable_secureboot != NULL) {
		if (data == NULL || *size < fw.variable_secureboot_bytes) {
			*size = fw.variable_secureboot_bytes;
			return EFI_BUFFER_TOO_SMALL;
		}
		memcpy(data, fw.variable_secureboot, fw.variable_secureboot_bytes);
		*size = fw.variable_secureboot_bytes;
		return EFI_SUCCESS;
	}
	if (strcmp(text, "dbx") != 0 || fw.variable_dbx == NULL) return EFI_NOT_FOUND;
	if (data == NULL || *size < fw.variable_dbx_bytes) {
		*size = fw.variable_dbx_bytes;
		return EFI_BUFFER_TOO_SMALL;
	}
	memcpy(data, fw.variable_dbx, fw.variable_dbx_bytes);
	*size = fw.variable_dbx_bytes;
	return EFI_SUCCESS;
}

/* -- ACPI: the root tables are built again after each change, at a new address, as a firmware can do -- */

static void build_roots(void)
{
	size_t length = SDT_HEADER + 8u * fw.tables;
	unsigned i;
	free(fw.xsdt);
	fw.xsdt = calloc(1, length);
	memcpy(fw.xsdt, "XSDT", 4);
	fw.xsdt[4] = (unsigned char)length;
	fw.xsdt[5] = (unsigned char)(length >> 8);
	fw.xsdt[8] = 1;
	for (i = 0; i < fw.tables; i++) put64(fw.xsdt + SDT_HEADER + 8u * i, (unsigned long long)(size_t)fw.table[i].bytes);
	fix_sum(fw.xsdt, length, 9);
	memset(fw.rsdp, 0, sizeof(fw.rsdp));
	memcpy(fw.rsdp, "RSD PTR ", 8);
	fw.rsdp[15] = 2;
	fw.rsdp[20] = 36;
	put64(fw.rsdp + 24, (unsigned long long)(size_t)fw.xsdt);
	fix_sum(fw.rsdp, 20, 8);
	fix_sum(fw.rsdp, 36, 32);
}

static void add_table(const unsigned char *bytes, size_t length, int sum)
{
	struct fw_table *t = &fw.table[fw.tables++];
	t->bytes = malloc(length);
	memcpy(t->bytes, bytes, length);
	if (sum) fix_sum(t->bytes, length, 9);
	t->length = length;
	t->key = ++fw.next_key;
	build_roots();
}

static EFI_STATUS EFIAPI fw_install_table(EFI_ACPI_TABLE_PROTOCOL *this, VOID *table, UINTN size, UINTN *key)
{
	(void)this;
	if (fw.tables == FW_TABLES || size < SDT_HEADER) return EFI_OUT_OF_RESOURCES;
	add_table(table, size, 1);
	*key = fw.next_key;
	fw.installs++;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fw_uninstall_table(EFI_ACPI_TABLE_PROTOCOL *this, UINTN key)
{
	unsigned i;
	(void)this;
	for (i = 0; i < fw.tables; i++)
		if (fw.table[i].key == key) {
			/* The memory of the table is given back, and its bytes are destroyed. */
			memset(fw.table[i].bytes, 0xDD, fw.table[i].length);
			free(fw.table[i].bytes);
			memmove(&fw.table[i], &fw.table[i + 1u], (fw.tables - i - 1u) * sizeof(fw.table[0]));
			fw.tables--;
			fw.uninstalls++;
			build_roots();
			return EFI_SUCCESS;
		}
	return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI fw_get_table(UINTN index, VOID **table, UINT32 *version, UINTN *key)
{
	if (index >= fw.tables) return EFI_NOT_FOUND;
	*table = fw.table[index].bytes;
	*version = 1u << 5;
	*key = fw.table[index].key;
	return EFI_SUCCESS;
}

static int has_table(const char *oem_table_id)
{
	unsigned i;
	for (i = 0; i < fw.tables; i++)
		if (!strncmp((const char *)fw.table[i].bytes + 16, oem_table_id, strlen(oem_table_id))) return 1;
	return 0;
}

/* -- the board -- */

static EFI_STATUS EFIAPI pci_read(EFI_PCI_IO_PROTOCOL *this, EFI_PCI_IO_PROTOCOL_WIDTH width, UINT32 offset, UINTN count,
				  VOID *buffer)
{
	(void)this;
	if (width != EfiPciIoWidthUint32 || offset + 4u * count > sizeof(fw.pci)) return EFI_INVALID_PARAMETER;
	memcpy(buffer, fw.pci + offset, 4u * count);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI pci_location(EFI_PCI_IO_PROTOCOL *this, UINTN *segment, UINTN *bus, UINTN *device, UINTN *function)
{
	(void)this;
	*segment = 0; *bus = 1; *device = 0; *function = 0;
	return EFI_SUCCESS;
}

/* GetBarAttributes gives a copy of the list of the model in pool memory, which the caller frees. An index without
 * a list is not valid, as in edk2. */
static EFI_STATUS EFIAPI pci_bar_attributes(EFI_PCI_IO_PROTOCOL *this, UINT8 index, UINT64 *supports, VOID **resources)
{
	(void)this;
	if (index >= FW_BARS || fw.bar_bytes[index] == 0) return EFI_UNSUPPORTED;
	if (supports != NULL) *supports = 0;
	if (resources != NULL) {
		if (fw_allocate(EfiBootServicesData, fw.bar_bytes[index], resources) != EFI_SUCCESS) return EFI_OUT_OF_RESOURCES;
		memcpy(*resources, fw.bar_list[index], fw.bar_bytes[index]);
	}
	return EFI_SUCCESS;
}

/* A read of the register BAR gives the control value at the control register and 0 elsewhere. */
static EFI_STATUS mem_read(unsigned long long offset, VOID *buffer)
{
	UINT32 v = offset == cu_bar_control_offset() ? fw.control_value : 0u;
	memcpy(buffer, &v, sizeof(v));
	fw.mem_reads++;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI pci_mem_read(EFI_PCI_IO_PROTOCOL *this, EFI_PCI_IO_PROTOCOL_WIDTH width, UINT8 index,
				      UINT64 offset, UINTN count, VOID *buffer)
{
	(void)this;
	if (width != EfiPciIoWidthUint32 || count != 1u || index != fw.register_index) return EFI_UNSUPPORTED;
	return mem_read(offset, buffer);
}

static EFI_STATUS EFIAPI pci_mem_write(EFI_PCI_IO_PROTOCOL *this, EFI_PCI_IO_PROTOCOL_WIDTH width, UINT8 index,
				       UINT64 offset, UINTN count, VOID *buffer)
{
	(void)this; (void)width; (void)index; (void)offset; (void)count; (void)buffer;
	fw.mem_writes++;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI bridge_mem_read(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *this, EFI_PCI_IO_PROTOCOL_WIDTH width,
					 UINT64 address, UINTN count, VOID *buffer)
{
	(void)this;
	if (width != EfiPciIoWidthUint32 || count != 1u || address < FW_BAR5) return EFI_INVALID_PARAMETER;
	fw.bridge_reads++;
	return mem_read(address - FW_BAR5, buffer);
}

static EFI_STATUS EFIAPI bridge_mem_write(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *this, EFI_PCI_IO_PROTOCOL_WIDTH width,
					  UINT64 address, UINTN count, VOID *buffer)
{
	(void)this; (void)width; (void)address; (void)count; (void)buffer;
	fw.mem_writes++;
	return EFI_SUCCESS;
}

/* The windows of the root bridge belong to the firmware: the caller does not free them. */
static EFI_STATUS EFIAPI bridge_configuration(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *this, VOID **resources)
{
	(void)this;
	if (fw.bridge_bytes == 0) return EFI_UNSUPPORTED;
	*resources = fw.bridge_list;
	return EFI_SUCCESS;
}

/* One QWORD memory descriptor at list + at, as edk2 PciIoGetBarAttributes fills it. Returns the next offset. */
static size_t qword_memory(unsigned char *list, size_t at, unsigned long long minimum, unsigned long long length)
{
	memset(list + at, 0, 46);
	list[at] = 0x8A;
	list[at + 1] = 0x2B;
	list[at + 6] = 32;
	put64(list + at + 14, minimum);
	put64(list + at + 22, length ? length - 1u : 0u);
	put64(list + at + 38, length);
	return at + 46u;
}

static size_t end_tag(unsigned char *list, size_t at)
{
	list[at] = 0x79;
	list[at + 1] = 0;
	return at + 2u;
}

/* The BARs of unit A (lspci of evidence/linux/2026-09-22-E21-linux-reference-4): two 64-bit prefetchable memory
 * BARs, an I/O BAR, and BAR5 as a 32-bit memory BAR, with the memory decode on. */
static void unit_a_bars(void)
{
	static const unsigned bars[6] = { 0xC000000Cu, 0, 0xD000000Cu, 0, 0x0000E001u, (unsigned)FW_BAR5 };
	unsigned i;
	fw.pci[4] = 0x06; fw.pci[5] = 0x04;
	for (i = 0; i < 6; i++) {
		fw.pci[16 + 4 * i] = (unsigned char)bars[i];
		fw.pci[17 + 4 * i] = (unsigned char)(bars[i] >> 8);
		fw.pci[18 + 4 * i] = (unsigned char)(bars[i] >> 16);
		fw.pci[19 + 4 * i] = (unsigned char)(bars[i] >> 24);
	}
}

/* One BarIndex of the model with one memory descriptor. */
static void bar_descriptor(unsigned index, unsigned long long minimum, unsigned long long length)
{
	fw.bar_bytes[index] = end_tag(fw.bar_list[index], qword_memory(fw.bar_list[index], 0, minimum, length));
}

/* A root bridge of segment 0 with one memory window. */
static void add_bridge(unsigned long long minimum, unsigned long long length)
{
	fw.bridge_bytes = end_tag(fw.bridge_list, qword_memory(fw.bridge_list, 0, minimum, length));
	add_protocol(&fw_handle_bridge, &guid_bridge, &fw_bridge);
}

static size_t add_structure(unsigned char *t, size_t at, const unsigned char *formatted, size_t bytes,
			    const char *const *strings, unsigned count)
{
	unsigned i;
	memcpy(t + at, formatted, bytes);
	at += bytes;
	for (i = 0; i < count; i++) {
		size_t n = strlen(strings[i]) + 1u;
		memcpy(t + at, strings[i], n);
		at += n;
	}
	if (count == 0) t[at++] = 0;
	t[at++] = 0;
	return at;
}

/* The SMBIOS table of a BC-250, with the strings that driver/common/board/board_identity.c accepts, behind a 3.0 entry
 * point. */
static void build_smbios(void)
{
	static const char *const bios[] = { "American Megatrends Inc.", "P5.00", "05/03/2022" };
	static const char *const board[] = { "ASRock", "AMD BC-250" };
	unsigned char b0[0x12] = { 0, 0x12, 0, 0, 1, 2, 0, 0, 3 };
	unsigned char b1[0x1B] = { 1, 0x1B, 1, 0 };
	unsigned char b2[0x08] = { 2, 0x08, 2, 0, 1, 2 };
	unsigned char end[4] = { 127, 4, 3, 0 };
	size_t at = 0;
	unsigned i;
	for (i = 0; i < 16; i++) b1[8 + i] = (unsigned char)(0x10 + i);
	at = add_structure(fw_smbios, at, b0, sizeof(b0), bios, 3);
	at = add_structure(fw_smbios, at, b1, sizeof(b1), NULL, 0);
	at = add_structure(fw_smbios, at, b2, sizeof(b2), board, 2);
	at = add_structure(fw_smbios, at, end, sizeof(end), NULL, 0);
	memset(fw_smbios_entry, 0, sizeof(fw_smbios_entry));
	memcpy(fw_smbios_entry, "_SM3_", 5);
	fw_smbios_entry[6] = 24;
	fw_smbios_entry[7] = 3;
	fw_smbios_entry[8] = 4;
	fw_smbios_entry[10] = 1;
	fw_smbios_entry[12] = (unsigned char)at;
	fw_smbios_entry[13] = (unsigned char)(at >> 8);
	put64(fw_smbios_entry + 16, (unsigned long long)(size_t)fw_smbios);
	fix_sum(fw_smbios_entry, 24, 5);
}

/* ---- one start of the stage ---------------------------------------------------------------------- */

static void add_file(const char *path, const char *dir, const char *name)
{
	struct fw_file *f = &fw.file[fw.files];
	snprintf(f->path, sizeof(f->path), "%s", path);
	f->data = efl_test_load(dir, name, &f->bytes);
	if (f->data == NULL) {
		failures++;
		printf("FAIL firmware model: %s/%s did not read\n", dir, name);
		return;
	}
	fw.files++;
}

static void free_firmware(void)
{
	unsigned i;
	for (i = 0; i < fw.files; i++) free(fw.file[i].data);
	for (i = 0; i < fw.tables; i++) free(fw.table[i].bytes);
	free(fw.xsdt);
	free(fw.dsdt);
	free(fw.variable_dbx);
	for (i = 0; i < fw.page_blocks; i++) _aligned_free(fw.page_block[i]);
	memset(&fw, 0, sizeof(fw));
	engine_model_reset();
}

/* A new firmware: a BC-250 with the stock ACPI tables of acpi_dir, and a Security2 protocol. */
static int new_firmware(const char *acpi_dir)
{
	static const char *const stock[] = { "FACP.aml", "APIC.aml", "SSDT1.aml", "SSDT2.aml" };
	static const char image_file[] = "\\EFI\\BC250\\bc250-stage.efi";
	size_t bytes, i, n = sizeof(image_file);
	unsigned char *data;

	free_firmware();
	fw_console.OutputString = fw_output;
	fw_bs.AllocatePool = fw_allocate;
	fw_bs.FreePool = fw_free;
	fw_bs.Stall = fw_stall;
	fw_bs.HandleProtocol = fw_handle_protocol;
	fw_bs.LocateProtocol = fw_locate_protocol;
	fw_bs.LocateHandleBuffer = fw_locate_handle_buffer;
	fw_bs.ReinstallProtocolInterface = fw_reinstall;
	fw_bs.LoadImage = fw_load_image;
	fw_bs.StartImage = fw_start_image;
	fw_bs.UnloadImage = fw_unload_image;
	fw_bs.CreateEvent = fw_create_event;
	fw_bs.CloseEvent = fw_close_event;
	fw_bs.CalculateCrc32 = fw_crc32;
	fw_bs.AllocatePages = (VOID *)fw_allocate_pages;
	fw_bs.FreePages = (VOID *)fw_free_pages;
	fw_bs.InstallProtocolInterface = fw_install_protocol;
	fw_bs.UninstallProtocolInterface = fw_uninstall_protocol;
	fw_bs.InstallConfigurationTable = fw_install_configuration;
	fw_rt.ConvertPointer = fw_convert_pointer;
	fw_rt.GetVariable = fw_get_variable;
	fw_rt.ResetSystem = fw_reset_system;
	memset(&fw_rt.Hdr, 0, sizeof(fw_rt.Hdr));
	fw_rt.Hdr.Signature = 0x5652455453424D53ull;	/* the table's own value is not read; the field has a size */
	fw_rt.Hdr.HeaderSize = (UINT32)sizeof(EFI_RUNTIME_SERVICES);
	(void)fw_crc32(&fw_rt, sizeof(fw_rt), &fw_rt.Hdr.CRC32);
	fw_st.ConOut = &fw_console;
	fw_st.BootServices = &fw_bs;
	fw_st.RuntimeServices = &fw_rt;
	fw_fs.OpenVolume = fs_open_volume;
	fw_security2.FileAuthentication = fw_firmware_check;
	fw_acpi_table.InstallAcpiTable = fw_install_table;
	fw_acpi_table.UninstallAcpiTable = fw_uninstall_table;
	fw_acpi_sdt.GetAcpiTable = fw_get_table;
	fw_pci.Pci.Read = pci_read;
	fw_pci.GetLocation = pci_location;
	fw_pci.GetBarAttributes = pci_bar_attributes;
	fw_pci.Mem.Read = pci_mem_read;
	fw_pci.Mem.Write = pci_mem_write;
	fw_bridge.Mem.Read = bridge_mem_read;
	fw_bridge.Mem.Write = bridge_mem_write;
	fw_bridge.Configuration = bridge_configuration;
	fw_bridge.SegmentNumber = 0;

	/* The file path of the stage image: one media file path node and the end node. */
	fw_image_path[0] = EFI_DP_TYPE_MEDIA;
	fw_image_path[1] = EFI_DP_MEDIA_FILEPATH;
	fw_image_path[2] = (unsigned char)(4u + 2u * n);
	fw_image_path[3] = 0;
	for (i = 0; i < n; i++) {
		fw_image_path[4u + 2u * i] = (unsigned char)image_file[i];
		fw_image_path[5u + 2u * i] = 0;
	}
	memcpy(fw_image_path + 4u + 2u * n, fw_device_path, 4);
	memset(&fw_loaded, 0, sizeof(fw_loaded));
	fw_loaded.DeviceHandle = &fw_handle_device;
	fw_loaded.FilePath = (EFI_DEVICE_PATH_PROTOCOL *)fw_image_path;

	add_protocol(&fw_handle_image, &guid_loaded, &fw_loaded);
	add_protocol(&fw_handle_device, &guid_device_path, fw_device_path);
	add_protocol(&fw_handle_device, &guid_fs, &fw_fs);
	add_protocol(&fw_handle_security, &guid_security2, &fw_security2);
	add_protocol(&fw_handle_security, &guid_acpi_table, &fw_acpi_table);
	add_protocol(&fw_handle_security, &guid_acpi_sdt, &fw_acpi_sdt);
	add_protocol(&fw_handle_gpu, &guid_pci, &fw_pci);

	/* The GPU function of a BC-250, as core-unlock/test/host_test.c builds it. */
	fw.pci[0] = 0x02; fw.pci[1] = 0x10; fw.pci[2] = 0xFE; fw.pci[3] = 0x13; fw.pci[44] = 0x22; fw.pci[45] = 0x10;
	build_smbios();

	/* The stock tables. The FADT of the model names the DSDT of the model. */
	fw.dsdt = efl_test_load(acpi_dir, "DSDT.aml", &bytes);
	if (fw.dsdt == NULL) return 0;
	for (i = 0; i < sizeof(stock) / sizeof(stock[0]); i++) {
		data = efl_test_load(acpi_dir, stock[i], &bytes);
		if (data == NULL) return 0;
		if (i == 0 && bytes >= 148u) {
			memset(data + 40, 0, 4);
			put64(data + 140, (unsigned long long)(size_t)fw.dsdt);
		}
		add_table(data, bytes, i == 0);
		free(data);
	}
	fw_config[0].VendorGuid = guid_acpi20;
	fw_config[0].VendorTable = fw.rsdp;
	fw_config[1].VendorGuid = guid_smbios3;
	fw_config[1].VendorTable = fw_smbios_entry;
	fw_st.NumberOfTableEntries = 2;
	fw_st.ConfigurationTable = fw_config;
	return 1;
}

/* Runs efi_main with these load options. *reset says that the stage restarted the board. */
static EFI_STATUS run_stage(const char *options, int *reset)
{
	EFI_STATUS status = EFI_ABORTED;
	char line[sizeof(fw_options) / sizeof(fw_options[0])];
	size_t i;
	/* The TPM module is off unless a test names it: only test_tpm sees it, so that the other tests count the same
	 * firmware calls as before the module. */
	snprintf(line, sizeof(line), strstr(options, "tpm=") ? "%s" : "%s tpm=0", options);
	for (i = 0; line[i] && i + 1u < sizeof(fw_options) / sizeof(fw_options[0]); i++) fw_options[i] = (unsigned char)line[i];
	fw_options[i] = 0;
	fw_loaded.LoadOptions = fw_options;
	fw_loaded.LoadOptionsSize = (UINT32)((i + 1u) * sizeof(CHAR16));
	fw_config[0].VendorTable = fw.rsdp;
	fw.log_bytes = 0;
	fw.log[0] = 0;
	*reset = 0;
	if (setjmp(fw.reset) == 0)
		status = efi_main(&fw_handle_image, &fw_st);
	else
		*reset = 1;
	return status;
}

static int said(const char *text)
{
	return strstr(fw.log, text) != NULL;
}

static VOID *security2_now(void)
{
	struct fw_protocol *p = find_protocol(NULL, &guid_security2);
	return p ? p->interface : NULL;
}

#define NEXT "\\EFI\\Microsoft\\Boot\\bootmgfw.efi"
#define BESIDE "\\EFI\\BC250\\"

/* ---- the tests ----------------------------------------------------------------------------------- */

static void test_acpi(const char *acpi_dir, const char *fixtures)
{
	unsigned char *bad;
	size_t bytes;
	int reset;

	/* A BC-250 with the stock tables: two tables are installed and the stock CPU table is removed. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	CHECK(run_stage("unlock=0 sb=0", &reset) == EFI_SUCCESS && !reset);
	CHECK(said("board BC-250") && said("idle states: table of") && said("method stubs: table of"));
	CHECK(fw.installs == 2 && fw.uninstalls == 1 && said("the stock CPU table is removed"));
	CHECK(has_table("CPUIDLE") && has_table("AMLSTUB") && !has_table("AMD CPU"));
	CHECK(fw.loads == 1 && fw.starts == 1 && fw.loads_from_buffer == 0);
	CHECK(fw.pool_live == 0);
	/* A second start on the same firmware finds nothing to do. */
	CHECK(run_stage("unlock=0 sb=0", &reset) == EFI_SUCCESS && fw.installs == 2 && fw.uninstalls == 1);
	CHECK(said("no SSDT with the header of the stock CPU table") && said("no called method is without a definition"));
	CHECK(fw.pool_live == 0);

	/* The off switches of the two tables. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	CHECK(run_stage("unlock=0 sb=0 acpi.stubs=0", &reset) == EFI_SUCCESS && fw.installs == 1 && has_table("CPUIDLE"));
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	CHECK(run_stage("unlock=0 sb=0 acpi=0", &reset) == EFI_SUCCESS && fw.installs == 0 && fw.uninstalls == 0);
	CHECK(said("acpi: off by the load option"));

	/* Another PCI function: not a BC-250, and nothing is installed. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	fw.pci[2] = 0xFF;
	CHECK(run_stage("unlock=0 sb=0", &reset) == EFI_SUCCESS && fw.installs == 0 && said("not a BC-250: nothing done"));
	CHECK(fw.starts == 1);

	/* A definition block with a wrong checksum in the root table: the set is not whole, and nothing is installed. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	bad = efl_test_load(acpi_dir, "SSDT2.aml", &bytes);
	if (bad != NULL) {
		bad[bytes - 1u] ^= 1u;
		add_table(bad, bytes, 0);
		free(bad);
		CHECK(run_stage("unlock=0 sb=0", &reset) == EFI_SUCCESS && fw.installs == 0 && fw.uninstalls == 0);
		CHECK(said("the table set is not whole") && said("a name collision cannot be excluded"));
		CHECK(has_table("AMD CPU") && fw.starts == 1);
	}

	/* No ACPI table protocol. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	find_protocol(NULL, &guid_acpi_table)->guid.Data1 ^= 1u;
	CHECK(run_stage("unlock=0 sb=0", &reset) == EFI_SUCCESS && fw.installs == 0 && said("no ACPI table protocol"));
	/* No SDT protocol: the new table is installed and the stock table stays. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	find_protocol(NULL, &guid_acpi_sdt)->guid.Data1 ^= 1u;
	CHECK(run_stage("unlock=0 sb=0", &reset) == EFI_SUCCESS && fw.installs == 2 && fw.uninstalls == 0);
	CHECK(said("no ACPI SDT protocol") && has_table("AMD CPU"));
}

static void test_secure_boot(const char *acpi_dir, const char *fixtures)
{
	int reset;

	/* Audit, the default: the image starts from its path, the hook sees the load, and the protocol of the
	 * firmware is back after the unwind. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	add_file(BESIDE "db.esl", fixtures, "db_root.esl");
	CHECK(run_stage("unlock=0 acpi=0", &reset) == EFI_SUCCESS && !reset);
	CHECK(said("mode audit") && said("the image check is in place"));
	CHECK(said("the next image, ") && said("allowed: a signature leads to a certificate of db"));
	CHECK(said("the firmware asked the image check 1 time(s)"));
	CHECK(fw.loads == 1 && fw.loads_from_buffer == 0 && fw.starts == 1 && fw.firmware_checks == 1);
	CHECK(fw.stage_check_at_start == 1 && fw.reinstalls == 2 && security2_now() == &fw_security2);
	CHECK(fw.pool_live == 0);
	/* Audit refuses nothing: an image that db does not allow starts, and the console says what it is. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed_other.efi");
	add_file(BESIDE "db.esl", fixtures, "db_root.esl");
	CHECK(run_stage("unlock=0 acpi=0", &reset) == EFI_SUCCESS && fw.starts == 1 && said("not allowed: the signature is valid"));
	/* Audit without any file. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	CHECK(run_stage("unlock=0 acpi=0", &reset) == EFI_SUCCESS && fw.starts == 1 && said("no db.esl beside the stage"));

	/* Enforce with sound inputs: the image is loaded from the bytes that the gate verified. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	add_file(BESIDE "db.esl", fixtures, "db_root.esl");
	add_file(BESIDE "dbx.esl", fixtures, "empty.esl");
	CHECK(run_stage("unlock=0 acpi=0 sb.mode=enforce", &reset) == EFI_SUCCESS && !reset);
	CHECK(said("mode enforce: an image that is not allowed does not start"));
	CHECK(fw.loads == 1 && fw.loads_from_buffer == 1 && fw.buffer_is_file == 1 && fw.starts == 1);
	CHECK(security2_now() == &fw_security2 && fw.pool_live == 0);

	/* Enforce, an image that db does not allow: no load, no start, EFI_ACCESS_DENIED. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed_other.efi");
	add_file(BESIDE "db.esl", fixtures, "db_root.esl");
	add_file(BESIDE "dbx.esl", fixtures, "empty.esl");
	CHECK(run_stage("unlock=0 acpi=0 sb.mode=enforce", &reset) == EFI_ACCESS_DENIED);
	CHECK(fw.loads == 0 && fw.starts == 0 && said("a module refused the next image"));
	CHECK(security2_now() == &fw_security2 && fw.pool_live == 0);

	/* Lab step s4b: enforce, the digest of the next image is in dbx. The stage refuses it and returns to its
	 * loader. With log=line and with log=1, the log file holds the refusal, the unwind and the return line, in
	 * that order, and the return line is the last line of the file when efi_main returns. */
	{
		static const char *const modes[] = { "unlock=0 acpi=0 sb.mode=enforce log=line",
						     "unlock=0 acpi=0 sb.mode=enforce log=1" };
		unsigned m;
		for (m = 0; m < 2u; m++) {
			long refused, back, returning;
			if (!new_firmware(acpi_dir)) return;
			add_file(NEXT, fixtures, "signed.efi");
			add_file(BESIDE "db.esl", fixtures, "db_root.esl");
			add_file(BESIDE "dbx.esl", fixtures, "dbx_digest.esl");
			CHECK(run_stage(modes[m], &reset) == EFI_ACCESS_DENIED && !reset);
			CHECK(fw.loads == 0 && fw.starts == 0 && security2_now() == &fw_security2 && fw.pool_live == 0);
			refused = file_at(LOG_FILE, "bc250-stage: a module refused the next image: not started\n");
			back = file_at(LOG_FILE, "secure-boot: the Security2 protocol of the firmware is back in its place\n");
			returning = file_at(LOG_FILE, "bc250-stage: returning to the loader, status 0x800000000000000F\n");
			CHECK(file_has(LOG_FILE, "forbidden: the digest of the image is in dbx"));
			CHECK(refused >= 0 && back > refused && returning > back);
			CHECK(file_ends_with(LOG_FILE, "bc250-stage: returning to the loader, status 0x800000000000000F"));
		}
	}
	/* Enforce, an image that dbx forbids by a certificate digest. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	add_file(BESIDE "db.esl", fixtures, "db_root.esl");
	add_file(BESIDE "dbx.esl", fixtures, "dbx_inter_hash.esl");
	CHECK(run_stage("unlock=0 acpi=0 sb.mode=enforce", &reset) == EFI_ACCESS_DENIED && fw.loads == 0);
	CHECK(said("forbidden: a certificate of its signature is in dbx"));
	/* Enforce, the next image is not there. */
	if (!new_firmware(acpi_dir)) return;
	add_file(BESIDE "db.esl", fixtures, "db_root.esl");
	add_file(BESIDE "dbx.esl", fixtures, "empty.esl");
	CHECK(run_stage("unlock=0 acpi=0 sb.mode=enforce", &reset) == EFI_ACCESS_DENIED && fw.loads == 0);

	/* Enforce with a defect in its inputs: every case refuses, and the hook is not installed. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	add_file(BESIDE "db.esl", fixtures, "db_root.esl");
	CHECK(run_stage("unlock=0 acpi=0 sb.mode=enforce", &reset) == EFI_ACCESS_DENIED);
	CHECK(fw.loads == 0 && fw.reinstalls == 0 && said("dbx.esl is not beside the stage") && !said("mode audit"));
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	add_file(BESIDE "dbx.esl", fixtures, "empty.esl");
	CHECK(run_stage("unlock=0 acpi=0 sb.mode=enforce", &reset) == EFI_ACCESS_DENIED && fw.loads == 0);
	CHECK(said("db.esl is not beside the stage") && !said("mode audit"));
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	add_file(BESIDE "db.esl", fixtures, "bad.esl");
	add_file(BESIDE "dbx.esl", fixtures, "empty.esl");
	CHECK(run_stage("unlock=0 acpi=0 sb.mode=enforce", &reset) == EFI_ACCESS_DENIED && fw.loads == 0);
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	add_file(BESIDE "db.esl", fixtures, "db_root.esl");
	add_file(BESIDE "dbx.esl", fixtures, "dbx_unknown.esl");
	CHECK(run_stage("unlock=0 acpi=0 sb.mode=enforce", &reset) == EFI_ACCESS_DENIED && fw.loads == 0);
	CHECK(said("dbx has entries of a type that this stage does not read"));

	/* The dbx variable of a firmware that has one is read and used. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	add_file(BESIDE "db.esl", fixtures, "db_root.esl");
	add_file(BESIDE "dbx.esl", fixtures, "empty.esl");
	fw.variable_dbx = efl_test_load(fixtures, "dbx_leaf.esl", &fw.variable_dbx_bytes);
	CHECK(run_stage("unlock=0 acpi=0 sb.mode=enforce", &reset) == EFI_ACCESS_DENIED && fw.loads == 0);
	CHECK(said("the dbx variable of the firmware: 1 certificates") && said("forbidden: a certificate"));
	/* A dbx variable that is not well formed is a defect of the inputs. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	add_file(BESIDE "db.esl", fixtures, "db_root.esl");
	add_file(BESIDE "dbx.esl", fixtures, "empty.esl");
	fw.variable_dbx = efl_test_load(fixtures, "bad.esl", &fw.variable_dbx_bytes);
	CHECK(run_stage("unlock=0 acpi=0 sb.mode=enforce", &reset) == EFI_ACCESS_DENIED && fw.loads == 0);
	CHECK(said("the dbx variable of the firmware did not read"));

	/* The firmware's own check answers EFI_SECURITY_VIOLATION: the loaded image is unloaded and not started. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	fw.firmware_check_status = EFI_SECURITY_VIOLATION;
	CHECK(run_stage("unlock=0 acpi=0", &reset) == EFI_SECURITY_VIOLATION);
	CHECK(fw.loads == 1 && fw.unloads == 1 && fw.starts == 0 && security2_now() == &fw_security2);

	/* A firmware without the Security2 protocol: the gate is the only check. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	find_protocol(NULL, &guid_security2)->guid.Data1 ^= 1u;
	CHECK(run_stage("unlock=0 acpi=0", &reset) == EFI_SUCCESS && fw.starts == 1 && fw.reinstalls == 0);
	CHECK(said("the firmware has no Security2 protocol"));

	/* The next image returns, and the firmware does not take its protocol back: the stage must not return. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	fw.refuse_put_back = 1;
	fw.start_status = EFI_ABORTED;
	(void)run_stage("unlock=0 acpi=0", &reset);
	CHECK(reset == 1 && fw.resets == 1 && fw.stalls == 1 && fw.starts == 1);
	CHECK(said("the firmware did not take its Security2 protocol back") && said("restart in 10 seconds"));

	/* The next image put a Security2 of its own in front of the stage's and returned. Its function can call
	 * into the stage, so the stage says so and does not return. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	fw.child_hooks = 1;
	fw.start_status = EFI_ABORTED;
	(void)run_stage("unlock=0 acpi=0", &reset);
	CHECK(reset == 1 && fw.resets == 1 && said("another image changed the Security2 protocol after the stage"));

	/* The module off: the firmware is not touched. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	CHECK(run_stage("unlock=0 acpi=0 sb=0", &reset) == EFI_SUCCESS && fw.reinstalls == 0 && fw.starts == 1);
	CHECK(fw.firmware_checks == 1 && said("secure-boot: off by the load option"));
}

/* ---- the state that Windows reads (sb.windows) ---------------------------------------------------- */

static const EFI_GUID guid_global = { 0x8BE4DF61, 0x93CA, 0x11D2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } };
static const EFI_GUID guid_database = { 0xD719B2CB, 0x3D3A, 0x4596, { 0xA3, 0xBC, 0xDA, 0xD0, 0x0E, 0x67, 0x65, 0x6F } };

static void test_sb_windows(const char *acpi_dir, const char *fixtures)
{
	static unsigned char pk[4] = { 0x11, 0x22, 0x33, 0x44 };
	static unsigned char kek[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
	static unsigned char db_bytes[8] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7 };
	static unsigned char dbx_bytes[10] = { 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9 };
	struct sb_windows_inputs in;
	struct stage st;
	unsigned char data[16];
	UINT32 attr = 0;
	UINTN size = 0;
	int reset;

	memset(&in, 0, sizeof(in));
	in.pk = pk;
	in.pk_bytes = sizeof(pk);
	in.kek = kek;
	in.kek_bytes = sizeof(kek);
	in.db = db_bytes;
	in.db_bytes = sizeof(db_bytes);
	in.dbx = dbx_bytes;
	in.dbx_bytes = sizeof(dbx_bytes);

	/* A full start with sb.windows=1: the log names the state, and the filter is gone when the stage returns. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	add_file(BESIDE "db.esl", fixtures, "db_root.esl");
	add_file(BESIDE "dbx.esl", fixtures, "empty.esl");
	add_file(BESIDE "pk.esl", fixtures, "db_root.esl");
	add_file(BESIDE "kek.esl", fixtures, "db_root.esl");
	CHECK(run_stage("unlock=0 acpi=0 sb.mode=enforce sb.windows=1 log=line", &reset) == EFI_SUCCESS && !reset);
	CHECK(said("Windows sees SecureBoot=1 from this stage (sb.windows=1), the firmware has no Secure Boot"));
	CHECK(said("the operating system is offered PK 0, KEK 0") || said("the operating system is offered PK"));
	CHECK(fw_rt.GetVariable == fw_get_variable && fw.events_open == 0 && fw.pool_live == 0);
	/* Without enforce the option is ignored, and the firmware is not touched. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	CHECK(run_stage("unlock=0 acpi=0 sb.windows=1", &reset) == EFI_SUCCESS);
	CHECK(said("sb.windows=1 without sb.mode=enforce: ignored") && fw_rt.GetVariable == fw_get_variable);

	/* The filter itself, between its install and its unwind. */
	if (!new_firmware(acpi_dir)) return;
	memset(&st, 0, sizeof(st));
	st.st = &fw_st;
	st.bs = &fw_bs;
	CHECK(sb_windows_install(&st, &in) == 0);
	CHECK(sb_windows_result_text(0)[0] != 0);
	CHECK(fw_rt.GetVariable != fw_get_variable && fw.events_open == 1 && fw.event_notify != NULL);
	/* SecureBoot = 1, SetupMode = 0, with the boots service and runtime attributes. */
	size = 1;
	CHECK(fw_rt.GetVariable(u"SecureBoot", (EFI_GUID *)&guid_global, &attr, &size, data) == EFI_SUCCESS);
	CHECK(size == 1 && data[0] == 1 && attr == 0x6);
	size = 1;
	CHECK(fw_rt.GetVariable(u"SetupMode", (EFI_GUID *)&guid_global, &attr, &size, data) == EFI_SUCCESS);
	CHECK(size == 1 && data[0] == 0 && attr == 0x6);
	/* The size first, as an operating system asks, and a buffer that is too small. */
	size = 0;
	CHECK(fw_rt.GetVariable(u"SecureBoot", (EFI_GUID *)&guid_global, &attr, &size, NULL) == EFI_BUFFER_TOO_SMALL);
	CHECK(size == 1);
	size = 2;
	data[0] = 0xFF;
	data[1] = 0xFF;
	CHECK(fw_rt.GetVariable(u"PK", (EFI_GUID *)&guid_global, &attr, &size, data) == EFI_BUFFER_TOO_SMALL);
	CHECK(size == sizeof(pk) && data[0] == 0xFF && attr == 0x27);
	/* PK and KEK live in the global namespace, db and dbx in the database one (edk2 8628578dacd3). */
	size = sizeof(data);
	CHECK(fw_rt.GetVariable(u"PK", (EFI_GUID *)&guid_global, &attr, &size, data) == EFI_SUCCESS);
	CHECK(size == sizeof(pk) && !memcmp(data, pk, sizeof(pk)) && attr == 0x27);
	size = sizeof(data);
	CHECK(fw_rt.GetVariable(u"KEK", (EFI_GUID *)&guid_global, &attr, &size, data) == EFI_SUCCESS);
	CHECK(size == sizeof(kek) && !memcmp(data, kek, sizeof(kek)) && attr == 0x27);
	/* A firmware answers EFI_NOT_FOUND when the name is asked in the wrong namespace, and so does the filter. */
	size = sizeof(data);
	CHECK(fw_rt.GetVariable(u"PK", (EFI_GUID *)&guid_database, &attr, &size, data) == EFI_NOT_FOUND);
	size = sizeof(data);
	CHECK(fw_rt.GetVariable(u"KEK", (EFI_GUID *)&guid_database, &attr, &size, data) == EFI_NOT_FOUND);
	size = sizeof(data);
	CHECK(fw_rt.GetVariable(u"db", (EFI_GUID *)&guid_database, &attr, &size, data) == EFI_SUCCESS);
	CHECK(size == sizeof(db_bytes) && !memcmp(data, db_bytes, sizeof(db_bytes)));
	size = sizeof(data);
	CHECK(fw_rt.GetVariable(u"dbx", (EFI_GUID *)&guid_database, &attr, &size, data) == EFI_SUCCESS);
	CHECK(size == sizeof(dbx_bytes) && !memcmp(data, dbx_bytes, sizeof(dbx_bytes)));
	/* Every other name goes to the firmware. */
	size = sizeof(data);
	CHECK(fw_rt.GetVariable(u"BootOrder", (EFI_GUID *)&guid_global, &attr, &size, data) == EFI_NOT_FOUND);
	/* A firmware that has SecureBoot answers with its own value: the stage does not override it. */
	fw.variable_secureboot = efl_test_load(fixtures, "empty.esl", &fw.variable_secureboot_bytes);
	if (fw.variable_secureboot != NULL) {
		fw.variable_secureboot_bytes = 1;
		fw.variable_secureboot[0] = 0;
		size = 1;
		CHECK(fw_rt.GetVariable(u"SecureBoot", (EFI_GUID *)&guid_global, &attr, &size, data) == EFI_SUCCESS);
		CHECK(size == 1 && data[0] == 0);
		free(fw.variable_secureboot);
		fw.variable_secureboot = NULL;
		fw.variable_secureboot_bytes = 0;
	}
	/* The address change: the notify converts every pointer and writes the slot again. */
	{
		unsigned before = fw.converts;
		CHECK(fw.event_notify != NULL);
		fw.event_notify((EFI_EVENT)&fw_rt, fw.event_context);
		/* The state, the runtime table and the five pointers of it: seven conversions. */
		CHECK(fw.converts - before >= 7);
	}
	size = 1;
	CHECK(fw_rt.GetVariable(u"SecureBoot", (EFI_GUID *)&guid_global, &attr, &size, data) == EFI_SUCCESS && data[0] == 1);
	size = sizeof(data);
	CHECK(fw_rt.GetVariable(u"db", (EFI_GUID *)&guid_database, &attr, &size, data) == EFI_SUCCESS &&
	      !memcmp(data, db_bytes, sizeof(db_bytes)));
	/* The unwind puts the firmware back and frees the runtime memory. */
	sb_windows_unwind(&st);
	CHECK(fw_rt.GetVariable == fw_get_variable && fw.events_open == 0 && fw.pool_live == 0);
	size = sizeof(data);
	CHECK(fw_rt.GetVariable(u"SecureBoot", (EFI_GUID *)&guid_global, &attr, &size, data) == EFI_NOT_FOUND);
	/* No memory for the runtime filter, and no event: both refuse and free what they took. */
	fw.allocate_fails = 1;
	CHECK(sb_windows_install(&st, &in) == 1 && fw_rt.GetVariable == fw_get_variable);
	fw.allocate_fails = 0;
	fw.create_event_fails = 1;
	CHECK(sb_windows_install(&st, &in) == 3 && fw_rt.GetVariable == fw_get_variable && fw.pool_live == 0);
	fw.create_event_fails = 0;
}

static void test_core(const char *acpi_dir, const char *fixtures)
{
	int reset;
	/* The load option next= names another image, and a load option that no module reads is counted. */
	if (!new_firmware(acpi_dir)) return;
	add_file("\\EFI\\BOOT\\BOOTX64.EFI", fixtures, "signed.efi");
	CHECK(run_stage("unlock=0 acpi=0 sb=0 next=\\EFI\\BOOT\\BOOTX64.EFI color=blue", &reset) == EFI_SUCCESS);
	CHECK(fw.loads == 1 && fw.starts == 1 && said("1 load option(s) not understood"));
	/* The next image is not there: the error of LoadImage goes back to the firmware. */
	if (!new_firmware(acpi_dir)) return;
	CHECK(run_stage("unlock=0 acpi=0 sb=0", &reset) == EFI_NOT_FOUND && fw.starts == 0 && said("did not load"));
	/* The status of an image that returns goes back to the firmware. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	fw.start_status = EFI_ABORTED;
	CHECK(run_stage("unlock=0 acpi=0 sb=0", &reset) == EFI_ABORTED && fw.pool_live == 0);
}

/* The count of this text in the file. */
static unsigned file_count(const char *path, const char *text)
{
	struct fw_file *file = find_file(path);
	size_t n = strlen(text), i;
	unsigned count = 0;
	if (file == NULL) return 0;
	for (i = 0; i + n <= file->bytes; i++)
		if (!memcmp(file->data + i, text, n)) count++;
	return count;
}

/* 1 when the log file is the console text of the last start, without its carriage returns. */
static int log_is_console(void)
{
	struct fw_file *file = find_file(LOG_FILE);
	size_t i, at = 0;
	if (file == NULL) return 0;
	for (i = 0; i < fw.log_bytes; i++) {
		if (fw.log[i] == '\r') continue;
		if (at >= file->bytes || file->data[at] != (unsigned char)fw.log[i]) return 0;
		at++;
	}
	return at == file->bytes;
}

static void test_log(const char *acpi_dir, const char *fixtures)
{
	struct fw_file *old;
	unsigned lines, i;
	int reset;

	/* The default: the log is on the volume before the next image starts, and it is the whole console at the
	 * end. The two core options are not in the count of options that nobody read. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	CHECK(run_stage("unlock=0 acpi=0 sb=0", &reset) == EFI_SUCCESS && fw.starts == 1);
	CHECK(fw.log_at_start == 1 && log_is_console() && fw.file_writes == 2 && fw.pool_live == 0);
	CHECK(file_has(LOG_FILE, "bc250-stage: BC-250 stage, version") && file_has(LOG_FILE, "the next image returned"));
	CHECK(!said("not understood"));
	/* A second start writes behind the first. */
	CHECK(run_stage("unlock=0 acpi=0 sb=0 log=1", &reset) == EFI_SUCCESS && !said("not understood"));
	CHECK(file_count(LOG_FILE, "BC-250 stage, version") == 2 && fw.file_deletes == 0);

	/* log=0: no file and no write. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	CHECK(run_stage("unlock=0 acpi=0 sb=0 log=0", &reset) == EFI_SUCCESS && fw.starts == 1);
	CHECK(find_file(LOG_FILE) == NULL && fw.file_writes == 0 && !said("not understood"));

	/* log=line: one write for each console line, so the file ends at the last line before a stop. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	CHECK(run_stage("unlock=0 acpi=0 sb=0 log=line", &reset) == EFI_SUCCESS && log_is_console());
	for (lines = 0, i = 0; i < fw.log_bytes; i++) lines += fw.log[i] == '\n';
	/* The lines before the option is read go in one write. */
	CHECK(lines > 4 && fw.file_writes >= lines - 2u && fw.file_writes <= lines && fw.log_at_start == 1);

	/* A volume that takes no write: one line says so, and the next image starts. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	fw.read_only = 1;
	CHECK(run_stage("unlock=0 acpi=0 sb=0 log=line", &reset) == EFI_SUCCESS && fw.starts == 1);
	CHECK(file_count(LOG_FILE, "x") == 0 && fw.file_writes == 0 && said("the log file did not write"));
	for (lines = 0, i = 0; i + 1u < fw.log_bytes; i++) lines += !strncmp(fw.log + i, "did not write", 13);
	CHECK(lines == 1);

	/* A log file above its bound starts again. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	old = &fw.file[fw.files++];
	snprintf(old->path, sizeof(old->path), "%s", LOG_FILE);
	old->bytes = 256u * 1024u + 1u;
	old->data = malloc(old->bytes);
	memset(old->data, 'x', old->bytes);
	CHECK(run_stage("unlock=0 acpi=0 sb=0", &reset) == EFI_SUCCESS && fw.file_deletes == 1);
	CHECK(log_is_console() && file_count(LOG_FILE, "xx") == 0 && find_file(NEXT) != NULL);

	/* A restart by a module: the reason is in the file before the board restarts. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	fw.refuse_put_back = 1;
	fw.start_status = EFI_ABORTED;
	(void)run_stage("unlock=0 acpi=0", &reset);
	CHECK(reset == 1 && file_has(LOG_FILE, "restart in 10 seconds") && log_is_console());

	/* A long console: 400 calls of the image check give more than 60 KiB, well above the 16 KiB that the core
	 * keeps in memory. The whole console must be in the file, with each way of writing. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	fw.extra_checks = 400;
	CHECK(run_stage("unlock=0 acpi=0 log=line", &reset) == EFI_SUCCESS && fw.starts == 1);
	CHECK(fw.log_bytes > 60000u && log_is_console() && fw.log_at_start == 1 && !said("did not fit in the log"));
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	fw.extra_checks = 400;
	CHECK(run_stage("unlock=0 acpi=0 log=1", &reset) == EFI_SUCCESS && fw.starts == 1);
	CHECK(fw.log_bytes > 60000u && log_is_console() && fw.log_at_start == 1 && !said("did not fit in the log"));
	/* With log=1 the file is written when the memory is full, not at each line. */
	CHECK(fw.file_writes >= 4u && fw.file_writes <= 12u);
	/* log=0 with the same console: no file, and no line about lost lines. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	fw.extra_checks = 400;
	CHECK(run_stage("unlock=0 acpi=0 log=0", &reset) == EFI_SUCCESS && find_file(LOG_FILE) == NULL);
	CHECK(fw.file_writes == 0 && !said("did not fit in the log"));
	/* A volume that stops taking writes in the middle: one line says so, and the start goes on. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	fw.extra_checks = 400;
	fw.write_budget = 3;
	CHECK(run_stage("unlock=0 acpi=0 log=1", &reset) == EFI_SUCCESS && fw.starts == 1 && fw.file_writes == 3);
	CHECK(said("the log file did not write") && !said("did not fit in the log"));

	/* A line for the console only is on the console and not in the file. The log state is the state of the
	 * last start, so this start is one with log=line. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	CHECK(run_stage("unlock=0 acpi=0 sb=0 log=line", &reset) == EFI_SUCCESS);
	{
		static const char image_file[] = "\\EFI\\BC250\\bc250-stage.efi";
		struct stage again;
		size_t i;
		memset(&again, 0, sizeof(again));
		again.st = &fw_st;
		again.bs = &fw_bs;
		for (i = 0; i < sizeof(image_file); i++) again.image_file[i] = (unsigned char)image_file[i];
		again.image_file_known = 1;
		(void)fs_open_volume(&fw_fs, &again.root);
		stage_say_console(&again, "core-unlock", "a line of long text");
		stage_say(&again, "core-unlock", "a line for the log");
		again.root->Close(again.root);
		CHECK(said("core-unlock: a line of long text") && said("core-unlock: a line for the log"));
		CHECK(file_has(LOG_FILE, "a line for the log") && !file_has(LOG_FILE, "a line of long text"));
	}

	/* A load that fails leaves the reason in the file. */
	if (!new_firmware(acpi_dir)) return;
	CHECK(run_stage("unlock=0 acpi=0 sb=0", &reset) == EFI_NOT_FOUND && file_has(LOG_FILE, "did not load"));
}

static void test_probe(const char *acpi_dir, const char *fixtures)
{
	int reset;
	/* probe=1 with the modules off: the presence of the four protocols, and no change of the firmware. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	CHECK(run_stage("unlock=0 acpi=0 sb=0 probe=1", &reset) == EFI_SUCCESS && fw.starts == 1);
	CHECK(said("acpi: probe: ACPI table protocol present") && said("acpi: probe: ACPI SDT protocol present"));
	CHECK(said("secure-boot: probe: Security2 protocol present, on exactly one handle"));
	CHECK(said("secure-boot: probe: Security protocol absent") && !said("not understood"));
	CHECK(fw.installs == 0 && fw.uninstalls == 0 && fw.reinstalls == 0 && fw.pool_live == 0);
	/* A firmware without them. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	find_protocol(NULL, &guid_acpi_table)->guid.Data1 ^= 1u;
	find_protocol(NULL, &guid_acpi_sdt)->guid.Data1 ^= 1u;
	find_protocol(NULL, &guid_security2)->guid.Data1 ^= 1u;
	CHECK(run_stage("unlock=0 acpi=0 sb=0 probe=1", &reset) == EFI_SUCCESS && fw.starts == 1);
	CHECK(said("ACPI table protocol absent") && said("ACPI SDT protocol absent") && said("no Security2 protocol"));
	/* Without probe=1 a module that is off says one line only, and a module that is on does not probe. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	CHECK(run_stage("unlock=0 acpi=0 sb=0", &reset) == EFI_SUCCESS && !said("probe:"));
	CHECK(run_stage("unlock=0 sb=0 probe=1 acpi.apply=0", &reset) == EFI_SUCCESS && !said("acpi: probe:"));

	/* acpi.apply=0: the plan and the built tables are reported, and the root tables stay as they are. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	CHECK(run_stage("unlock=0 sb=0 acpi.apply=0", &reset) == EFI_SUCCESS && fw.starts == 1);
	CHECK(said("acpi.apply=0: the tables are built and checked") && said("idle states: a table of"));
	CHECK(said("method stubs: a table of") && said("passed its check") && !said("not understood"));
	CHECK(fw.installs == 0 && fw.uninstalls == 0 && has_table("AMD CPU") && !has_table("CPUIDLE"));
	CHECK(fw.pool_live == 0);
}

/* The probe of core-unlock: unlock=0 probe=1 identifies the register BAR and reads the control register only. */
/* ---- the TPM module ------------------------------------------------------------------------------ */

typedef EFI_TCG2_PROTOCOL test_tcg2;
static const EFI_GUID guid_tcg2 = EFI_TCG2_PROTOCOL_GUID;
static const EFI_GUID guid_final_events = EFI_TCG2_FINAL_EVENTS_TABLE_GUID;
#define STAGE_FILE	"\\EFI\\BC250\\bc250-stage.efi"
#define TPM_LOG_BYTES	(64u * 1024u)	/* the size of the log area of tpm_module.c */

/* What the boot application saw, for the checks after the run. */
static struct {
	int ran, protocol, capability, log_matches, image_measured, stage_measured, extend_matches, record, record_pcrs;
	unsigned final_events;
	int final_in_nvs, after_exit_absent;
} seen;

static unsigned le32(const unsigned char *p)
{
	return p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* PCR 0 to 23 through SubmitCommand. 1 when all 24 read. */
static int read_pcrs(test_tcg2 *tcg2, unsigned char pcrs[24][32])
{
	unsigned char cmd[64], rsp[512];
	unsigned first, got, total = 0;
	for (first = 0; first < 24u; first += 8u) {
		size_t n = tl_tpm2_pcr_read(cmd, sizeof(cmd), first, 8);
		if (tcg2->SubmitCommand(tcg2, (UINT32)n, cmd, sizeof(rsp), rsp) != EFI_SUCCESS) return 0;
		size_t bytes = ((size_t)rsp[2] << 24) | ((size_t)rsp[3] << 16) | ((size_t)rsp[4] << 8) | rsp[5];	/* big-endian */
		if (bytes > sizeof(rsp) || tl_tpm2_pcr_read_answer(rsp, bytes, first, pcrs, &got) != 0)
			return 0;
		total += got;
	}
	return total == 24u;
}

/* 1 when the log has an event of this PCR and type whose digest is the Authenticode digest of the file. */
static int log_has_image(const unsigned char *log, unsigned pcr, unsigned type, const char *path)
{
	struct fw_file *file = find_file(path);
	struct efl_pe pe;
	struct tl_reader reader;
	struct tl_event event;
	size_t cursor = 0;
	if (file == NULL || !efl_pe_read(file->data, file->bytes, &pe)) return 0;
	memset(&reader, 0, sizeof(reader));
	while (tl_log_next(log, TPM_LOG_BYTES, &cursor, &reader, &event) == 1)
		if (event.pcr == pcr && event.type == type && event.digest != NULL && !memcmp(event.digest, pe.digest, 32)) return 1;
	return 0;
}

/* The work of a boot application: the calls that bootmgr makes, then ExitBootServices. */
static void tpm_application(void)
{
	struct fw_protocol *p = find_protocol(NULL, &guid_tcg2);
	test_tcg2 *tcg2;
	unsigned char capability[36], replay[24][32], pcrs[24][32], event[18 + 5];
	EFI_PHYSICAL_ADDRESS location = 0, last = 0;
	BOOLEAN truncated = 1;
	const unsigned char *log, *final;
	unsigned i;

	seen.ran = 1;
	if (p == NULL) return;
	seen.protocol = 1;
	tcg2 = p->interface;
	memset(capability, 0, sizeof(capability));
	capability[0] = sizeof(capability);
	seen.capability = tcg2->GetCapability(tcg2, capability) == EFI_SUCCESS && capability[16] == 1 &&
			  le32(capability + 8) == 2u && le32(capability + 12) == 2u && le32(capability + 24) == 0x42434D44u &&
			  le32(capability + 28) == 1u && le32(capability + 32) == 2u;
	if (tcg2->GetEventLog(tcg2, 2, &location, &last, &truncated) != EFI_SUCCESS || location == 0 || truncated) return;
	log = (const unsigned char *)(size_t)location;
	seen.log_matches = tl_log_replay(log, TPM_LOG_BYTES, replay) > 0 && read_pcrs(tcg2, pcrs) && !memcmp(replay, pcrs, sizeof(pcrs));
	seen.image_measured = log_has_image(log, 4, TL_EV_EFI_BOOT_SERVICES_APPLICATION, NEXT);
	seen.stage_measured = log_has_image(log, 0, TL_EV_S_CRTM_CONTENTS, STAGE_FILE);

	/* HashLogExtendEvent as a boot application uses it: PCR 8, EV_IPL, data "hello". */
	memset(event, 0, sizeof(event));
	event[0] = sizeof(event);
	event[4] = 14;
	event[8] = 1;
	event[10] = 8;
	event[14] = 0x0D;
	memcpy(event + 18, "hello", 5);
	seen.extend_matches = tcg2->HashLogExtendEvent(tcg2, 0, (EFI_PHYSICAL_ADDRESS)(size_t)"hello", 5, event) == EFI_SUCCESS &&
			      tl_log_replay(log, TPM_LOG_BYTES, replay) > 0 && read_pcrs(tcg2, pcrs) &&
			      !memcmp(replay, pcrs, sizeof(pcrs));

	/* ExitBootServices: the module measures PCR 5 and writes the handoff record. */
	if (fw.exit_notify != NULL) fw.exit_notify(&fw_bs, fw.exit_context);
	final = configuration_table(&guid_final_events);
	seen.final_in_nvs = final != NULL && page_type(final) == EfiACPIMemoryNVS;
	if (final != NULL) seen.final_events = le32(final + 8);	/* NumberOfEvents, low half */
	(void)tl_log_replay(log, TPM_LOG_BYTES, replay);
	for (i = 0; i < fw.page_blocks; i++)
		if (fw.page_type[i] == EfiReservedMemoryType) {
			struct tpm_handoff_view view;
			seen.record = tpm_handoff_read(fw.page_block[i], fw.page_bytes[i], efl_sha256, &view) == TPM_HANDOFF_OK &&
				      view.flags == (TPM_HANDOFF_STARTED | TPM_HANDOFF_PERMANENT_CHANGED) && view.sequence == 0 &&
				      view.permanent_bytes == 9u && view.permanent[8] == 1;	/* the model: SHA-256 only */
			/* The volatile state of the model is its PCRs: they must be the replay of the whole log. */
			seen.record_pcrs = view.volatile_bytes == sizeof(replay) && !memcmp(view.volatile_state, replay, sizeof(replay));
		}
	memset(capability, 0, sizeof(capability));
	capability[0] = sizeof(capability);
	seen.after_exit_absent = tcg2->GetCapability(tcg2, capability) == EFI_SUCCESS && capability[16] == 0 &&
				 tcg2->SubmitCommand(tcg2, 10, event, sizeof(capability), capability) == EFI_DEVICE_ERROR;
}

static void tpm_firmware(const char *acpi_dir, const char *fixtures)
{
	new_firmware(acpi_dir);
	add_file(STAGE_FILE, fixtures, "signed_other.efi");
	add_file(NEXT, fixtures, "signed.efi");
	fw.real_stall = 1;
	memset(&seen, 0, sizeof(seen));
}

static void test_tpm(const char *acpi_dir, const char *fixtures)
{
	static int firmware_tcg2;
	int reset;

	/* The whole path: a new TPM, the PCR_Allocate restart, the log, the protocol, the gate, ExitBootServices. */
	tpm_firmware(acpi_dir, fixtures);
	fw.on_start = tpm_application;
	CHECK(run_stage("unlock=0 acpi=0 sb=0 tpm=1", &reset) == EFI_SUCCESS && !reset);
	CHECK(said("PCR banks: SHA-256 only, after PCR_Allocate and a restart") && said("TCG2 protocol installed"));
	CHECK(engine_model.starts == 2 && said("next image measured into PCR 4"));
	CHECK(seen.ran && seen.protocol && seen.capability);
	CHECK(seen.log_matches && seen.image_measured && seen.stage_measured && seen.extend_matches);
	/* After GetEventLog: the event of HashLogExtendEvent and the two of ExitBootServices. */
	CHECK(seen.final_in_nvs && seen.final_events == 3u);
	CHECK(seen.record && seen.record_pcrs && seen.after_exit_absent);
	/* The bytes that started are the bytes that were measured. */
	CHECK(fw.loads == 1 && fw.loads_from_buffer == 1 && fw.buffer_is_file && fw.starts == 1);
	/* The image returned: the stage takes its protocol and its table away. */
	CHECK(find_protocol(NULL, &guid_tcg2) == NULL && configuration_table(&guid_final_events) == NULL);
	CHECK(fw.exit_notify == NULL && !engine_model_running() && fw.pool_live == 0 && fw.page_blocks == 0);
	CHECK(engine_model.wipes == 1 && said("tpm: model engine"));

	/* A new TPM with the SHA-256 bank only: no allocation and one start. */
	tpm_firmware(acpi_dir, fixtures);
	engine_model.new_sha256_only = 1;
	fw.on_start = tpm_application;
	CHECK(run_stage("unlock=0 acpi=0 sb=0 tpm=1", &reset) == EFI_SUCCESS);
	CHECK(engine_model.starts == 1 && !said("after PCR_Allocate") && seen.log_matches && seen.record);

	/* The firmware has its own TCG2 protocol: the stage adds none. */
	tpm_firmware(acpi_dir, fixtures);
	add_protocol(&fw_handle_security, &guid_tcg2, &firmware_tcg2);
	CHECK(run_stage("unlock=0 acpi=0 sb=0 tpm=1", &reset) == EFI_SUCCESS);
	CHECK(said("the firmware has a TCG2 protocol") && engine_model.inits == 0 && fw.loads_from_buffer == 0);

	/* The engine does not start: no protocol, and the image loads from its path. */
	tpm_firmware(acpi_dir, fixtures);
	engine_model.fail_init = 1;
	fw.on_start = tpm_application;
	CHECK(run_stage("unlock=0 acpi=0 sb=0 tpm=1", &reset) == EFI_SUCCESS);
	CHECK(said("the engine did not start: no TPM") && seen.ran && !seen.protocol && fw.loads_from_buffer == 0);
	CHECK(fw.page_blocks == 0 && fw_st.NumberOfTableEntries == 2);	/* nothing stays reserved */

	/* The stage file does not read: the TPM is not offered, because PCR 0 would not describe the stage. */
	tpm_firmware(acpi_dir, fixtures);
	fw.files = 0;
	free(fw.file[0].data);
	free(fw.file[1].data);
	add_file(NEXT, fixtures, "signed.efi");
	fw.on_start = tpm_application;
	CHECK(run_stage("unlock=0 acpi=0 sb=0 tpm=1", &reset) == EFI_SUCCESS);
	CHECK(said("the stage file did not read: no TPM") && !seen.protocol && !engine_model_running());
	CHECK(find_protocol(NULL, &guid_tcg2) == NULL && fw.exit_notify == NULL && fw.pool_live == 0);

	/* tpm=0: nothing. */
	tpm_firmware(acpi_dir, fixtures);
	CHECK(run_stage("unlock=0 acpi=0 sb=0 tpm=0", &reset) == EFI_SUCCESS);
	CHECK(engine_model.inits == 0 && fw.page_blocks == 0 && fw.loads_from_buffer == 0);
	fw.on_start = NULL;
	fw.real_stall = 0;
}

static void test_probe_bar(const char *acpi_dir, const char *fixtures)
{
	int reset;
	/* The decode is off in the plain model: the dump runs, nothing is read through a BAR. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	CHECK(run_stage("unlock=0 acpi=0 sb=0 probe=1", &reset) == EFI_SUCCESS && fw.starts == 1);
	CHECK(said("core-unlock: bar: function 1002:13FE") && said("the GPU function's memory decode is off"));
	CHECK(said("bar: BarIndex 5: status 0x8000000000000003") && said("probe: the register BAR is not identified"));
	CHECK(fw.mem_reads == 0 && fw.mem_writes == 0 && fw.pool_live == 0);

	/* The edk2 numbering: a 64-bit BAR is one index, so BAR5 of unit A is BarIndex 3. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	unit_a_bars();
	bar_descriptor(0, 0xC0000000ull, 0x10000000ull);
	bar_descriptor(1, 0xD0000000ull, 0x200000ull);
	bar_descriptor(3, FW_BAR5, 0x80000ull);
	fw.register_index = 3;
	fw.control_value = CU_BAR_CONTROL_VALUE;
	CHECK(run_stage("unlock=0 acpi=0 sb=0 probe=1", &reset) == EFI_SUCCESS && fw.starts == 1);
	CHECK(said("configuration BAR0 to BAR5: 0xC000000C 0x0 0xD000000C 0x0 0xE001 0xFE800000"));
	CHECK(said("bar: BarIndex 3 descriptor 0: minimum 0xFE800000, maximum 0x7FFFF, translation 0x0, length 0x80000"));
	CHECK(said("bar: BarIndex 3: 1 descriptors, end tag at byte 46"));
	CHECK(said("one PCI I/O BarIndex describes BAR5: BarIndex 3") && said("identified at 0xFE800000"));
	CHECK(said("probe: the register BAR is identified, no message sent"));
	CHECK(fw.mem_reads == 1 && fw.mem_writes == 0 && fw.bridge_reads == 0 && fw.pool_live == 0);

	/* The failure of kit r2: BarIndex 5 has a descriptor of length 0. Without a root bridge: refused. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	unit_a_bars();
	bar_descriptor(5, 0x300000ull, 0);
	fw.register_index = 5;
	fw.control_value = CU_BAR_CONTROL_VALUE;
	CHECK(run_stage("unlock=0 acpi=0 sb=0 probe=1", &reset) == EFI_SUCCESS && fw.starts == 1);
	CHECK(said("bar: BarIndex 5 descriptor 0: minimum 0x300000, maximum 0x0, translation 0x0, length 0x0"));
	CHECK(said("bar: 0 root bridges") && said("no single root bridge serves its address"));
	CHECK(fw.mem_reads == 0 && fw.mem_writes == 0 && fw.pool_live == 0);

	/* The same firmware with one root bridge whose window holds BAR5: the control reads through the bridge. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	unit_a_bars();
	bar_descriptor(5, 0x300000ull, 0);
	add_bridge(0xC0000000ull, 0x3F000000ull);
	fw.control_value = CU_BAR_CONTROL_VALUE;
	CHECK(run_stage("unlock=0 acpi=0 sb=0 probe=1", &reset) == EFI_SUCCESS && fw.starts == 1);
	CHECK(said("bar: root bridge 0 descriptor 0: minimum 0xC0000000") && said("one root bridge serves its address"));
	CHECK(said("reads 0x44, known 0x44: the control register has its known value"));
	CHECK(said("probe: the register BAR is identified, no message sent"));
	CHECK(fw.bridge_reads == 1 && fw.mem_writes == 0 && fw.pool_live == 0);

	/* The control register reads another value: refused. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	unit_a_bars();
	add_bridge(0xC0000000ull, 0x3F000000ull);
	fw.control_value = 0xFFFFFFFFu;
	CHECK(run_stage("unlock=0 acpi=0 sb=0 probe=1", &reset) == EFI_SUCCESS && fw.starts == 1);
	CHECK(said("does not have its known value") && said("probe: the register BAR is not identified"));
	CHECK(fw.bridge_reads == 1 && fw.mem_writes == 0);

	/* A descriptor at the address of BAR5 with another length: refused before any read. */
	if (!new_firmware(acpi_dir)) return;
	add_file(NEXT, fixtures, "signed.efi");
	unit_a_bars();
	bar_descriptor(3, FW_BAR5, 0x40000ull);
	add_bridge(0xC0000000ull, 0x3F000000ull);
	fw.register_index = 3;
	fw.control_value = CU_BAR_CONTROL_VALUE;
	CHECK(run_stage("unlock=0 acpi=0 sb=0 probe=1", &reset) == EFI_SUCCESS && said("has another length"));
	CHECK(fw.mem_reads == 0 && fw.mem_writes == 0 && fw.pool_live == 0);
}

unsigned firmware_tests(const char *acpi_dir, const char *fixture_dir, unsigned *check_count)
{
	size_t bytes = 0;
	unsigned char *probe = efl_test_load(fixture_dir, "signed.efi", &bytes);
	if (probe == NULL || !new_firmware(acpi_dir)) {
		printf("SKIP bindings: the firmware model needs the stock ACPI tables in %s and the fixtures in %s\n",
		       acpi_dir ? acpi_dir : "(no directory given)", fixture_dir ? fixture_dir : "(no directory given)");
	} else {
		test_core(acpi_dir, fixture_dir);
		test_log(acpi_dir, fixture_dir);
		test_probe(acpi_dir, fixture_dir);
		test_probe_bar(acpi_dir, fixture_dir);
		test_acpi(acpi_dir, fixture_dir);
		test_secure_boot(acpi_dir, fixture_dir);
		test_sb_windows(acpi_dir, fixture_dir);
		test_tpm(acpi_dir, fixture_dir);
	}
	free(probe);
	free_firmware();
	*check_count = checks;
	return failures;
}
