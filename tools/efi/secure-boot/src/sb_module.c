/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The Secure Boot check as a module of the BC-250 stage (docs/design/efi-resident-stage.md, "The Secure Boot
 * module"). This file is the UEFI binding. The verdict on an image comes from sb_verify.c, and the rule for the
 * inputs of the enforce mode from sb_policy.c.
 *
 * The module checks an image at two places:
 * - The gate: the core gives it the bytes of the next image before the start.
 * - The hook: the module puts its own EFI_SECURITY2_ARCH_PROTOCOL (PI Specification, volume 2) in the place of
 *   the firmware's, with ReinstallProtocolInterface. The firmware core calls the protocol for each image that
 *   LoadImage loads. The module's function gives the verdict and then calls the function of the firmware. The
 *   module does not write into the structure of the firmware, which can be in read-only memory.
 *
 * sb.mode=audit (the default) reports each verdict and refuses nothing. sb.mode=enforce refuses an image that is
 * not allowed, and it refuses every image when its inputs have a defect. The module reads the db and dbx variables
 * of the firmware when they are there, and it calls SetVariable nowhere.
 *
 * sb.windows=1, only with sb.mode=enforce, gives the operating system the state that the firmware lacks: a filter in
 * front of GetVariable answers SecureBoot=1, SetupMode=0 and the kit's PK, KEK, db and dbx (sb_windows.c). Every
 * other name, and every variable that the firmware has, goes to the firmware's own function. The state lives for
 * this start only and in memory only: no NVRAM write, and no SetVariable call exists in the stage.
 */
#include <stddef.h>
#include <string.h>
#include "efi.h"
#include "efl_text.h"
#include "stage.h"
#include "sb_policy.h"
#include "sb_siglist.h"
#include "sb_verify.h"
#include "sb_windows.h"

#define SB_NAME			"secure-boot"
#define SB_DATABASE_MAX_BYTES	(1024u * 1024u)
#define SB_UNWIND_WAIT_US	10000000u

static const EFI_GUID security2_guid = EFI_SECURITY2_ARCH_PROTOCOL_GUID;
/* EFI_IMAGE_SECURITY_DATABASE_GUID (edk2 MdePkg/Include/Guid/ImageAuthentication.h, commit 8628578dacd3). */
static const EFI_GUID database_guid = { 0xD719B2CB, 0x3D3A, 0x4596, { 0xA3, 0xBC, 0xDA, 0xD0, 0x0E, 0x67, 0x65, 0x6F } };

enum { MODE_AUDIT = 0, MODE_ENFORCE = 1 };
static const char *const mode_names[] = { "audit", "enforce" };

static struct stage *sb_stage;
static int mode;
static const char *defect;		/* enforce mode: the reason why every image is refused, or NULL */
static unsigned char *db, *dbx;
static struct sb_databases databases;
static unsigned hook_calls;

/* The hook. security2_handle is not NULL while the module's protocol is installed. */
static EFI_HANDLE security2_handle;
static EFI_SECURITY2_ARCH_PROTOCOL *firmware_security2;
static EFI_SECURITY2_ARCH_PROTOCOL stage_security2;

static void say_verdict(struct stage *stage, const char *what, size_t bytes, const struct sb_result *result)
{
	static const char hex[] = "0123456789abcdef";
	char digest[2u * sizeof(result->digest) + 1u];
	size_t i;
	for (i = 0; i < sizeof(result->digest); i++) {
		digest[2u * i] = hex[result->digest[i] >> 4];
		digest[2u * i + 1u] = hex[result->digest[i] & 15u];
	}
	digest[2u * sizeof(result->digest)] = 0;
	stage_sayf(stage, SB_NAME, "%s, %u bytes: %s", what, (unsigned)bytes, sb_verdict_text(result->verdict));
	if (result->verdict != SB_NOT_AN_IMAGE && result->verdict != SB_BAD_DATABASE)
		stage_sayf(stage, SB_NAME, "  Authenticode SHA-256 %s", digest);
}

/* ---- the databases ------------------------------------------------------------------------------- */

static enum sb_source checked(struct stage *stage, const char *what, unsigned char **data, size_t *bytes)
{
	struct sb_siglist_count count;
	if (!sb_siglist_count(*data, *bytes, &count)) {
		stage_sayf(stage, SB_NAME, "%s is not a well-formed signature database", what);
		stage_free(stage, *data);
		*data = NULL;
		*bytes = 0;
		return SB_SOURCE_BROKEN;
	}
	stage_sayf(stage, SB_NAME, "%s: %u certificates, %u image digests, %u certificate digests", what, count.x509,
		   count.sha256, count.x509_sha256);
	if (count.other) stage_sayf(stage, SB_NAME, "%s: %u entries of a type that this stage does not read", what, count.other);
	return SB_SOURCE_GOOD;
}

/* One database file beside the stage. */
static enum sb_source read_file(struct stage *stage, const char *name, unsigned char **data, size_t *bytes)
{
	unsigned short path[STAGE_PATH_CHARS];
	enum stage_read read;
	*data = NULL;
	*bytes = 0;
	if (!stage_sibling(stage, name, path, STAGE_PATH_CHARS)) return SB_SOURCE_ABSENT;
	read = stage_read_file(stage, path, SB_DATABASE_MAX_BYTES, data, bytes);
	if (read == STAGE_READ_ABSENT) return SB_SOURCE_ABSENT;
	if (read != STAGE_READ_OK) {
		stage_sayf(stage, SB_NAME, "%s did not read", name);
		return SB_SOURCE_BROKEN;
	}
	return checked(stage, name, data, bytes);
}

/* One database variable of the firmware. GetVariable only: nothing is written. */
static enum sb_source read_variable(struct stage *stage, const char *what, const CHAR16 *name, unsigned char **data,
				    size_t *bytes)
{
	EFI_GUID vendor = database_guid;
	CHAR16 copy[8];
	UINTN size = 0;
	EFI_STATUS status;
	size_t i;
	*data = NULL;
	*bytes = 0;
	for (i = 0; i + 1u < sizeof(copy) / sizeof(copy[0]) && name[i]; i++) copy[i] = name[i];
	copy[i] = 0;
	status = stage->st->RuntimeServices->GetVariable(copy, &vendor, NULL, &size, NULL);
	if (status == EFI_NOT_FOUND) return SB_SOURCE_ABSENT;
	if (status != EFI_BUFFER_TOO_SMALL || size == 0 || size > SB_DATABASE_MAX_BYTES) {
		stage_sayf(stage, SB_NAME, "%s did not read, status 0x%llx", what, (unsigned long long)status);
		return SB_SOURCE_BROKEN;
	}
	*data = stage_alloc(stage, (size_t)size);
	if (*data == NULL) return SB_SOURCE_BROKEN;
	status = stage->st->RuntimeServices->GetVariable(copy, &vendor, NULL, &size, *data);
	if (EFI_ERROR(status)) {
		stage_sayf(stage, SB_NAME, "%s did not read, status 0x%llx", what, (unsigned long long)status);
		stage_free(stage, *data);
		*data = NULL;
		return SB_SOURCE_BROKEN;
	}
	*bytes = (size_t)size;
	return checked(stage, what, data, bytes);
}

/* The file and the variable as one database: the lists of the one after the lists of the other. Frees both
 * parts. NULL with 0 bytes when both are empty. 0 when there is no memory for the whole. */
static int join(struct stage *stage, unsigned char *first, size_t first_bytes, unsigned char *second,
		size_t second_bytes, unsigned char **whole, size_t *whole_bytes)
{
	*whole = NULL;
	*whole_bytes = first_bytes + second_bytes;
	if (*whole_bytes != 0) {
		*whole = stage_alloc(stage, *whole_bytes);
		if (*whole != NULL) {
			if (first_bytes) memcpy(*whole, first, first_bytes);
			if (second_bytes) memcpy(*whole + first_bytes, second, second_bytes);
		}
	}
	stage_free(stage, first);
	stage_free(stage, second);
	if (*whole_bytes != 0 && *whole == NULL) {
		*whole_bytes = 0;
		return 0;
	}
	return 1;
}

static void read_databases(struct stage *stage)
{
	unsigned char *file, *variable;
	size_t file_bytes, variable_bytes;
	struct sb_siglist_count count;
	struct sb_inputs inputs;
	int whole;

	memset(&inputs, 0, sizeof(inputs));
	inputs.db_file = read_file(stage, "db.esl", &file, &file_bytes);
	inputs.db_firmware = read_variable(stage, "the db variable of the firmware", u"db", &variable, &variable_bytes);
	whole = join(stage, file, file_bytes, variable, variable_bytes, &db, &databases.db_bytes);
	inputs.dbx_file = read_file(stage, "dbx.esl", &file, &file_bytes);
	inputs.dbx_firmware = read_variable(stage, "the dbx variable of the firmware", u"dbx", &variable, &variable_bytes);
	whole = join(stage, file, file_bytes, variable, variable_bytes, &dbx, &databases.dbx_bytes) && whole;
	databases.db = db;
	databases.dbx = dbx;
	if (sb_siglist_count(dbx, databases.dbx_bytes, &count)) inputs.dbx_unread = count.other;

	if (inputs.db_file == SB_SOURCE_ABSENT) stage_say(stage, SB_NAME, "no db.esl beside the stage");
	if (inputs.dbx_file == SB_SOURCE_ABSENT) stage_say(stage, SB_NAME, "no dbx.esl beside the stage");
	defect = whole ? sb_enforce_defect(&inputs) : "no memory for the databases";
}

/* ---- the hook ------------------------------------------------------------------------------------ */

static EFI_STATUS EFIAPI check_file(const EFI_SECURITY2_ARCH_PROTOCOL *this, const EFI_DEVICE_PATH_PROTOCOL *file,
				    VOID *buffer, UINTN bytes, BOOLEAN boot_policy)
{
	struct sb_result result;
	(void)this;
	/* Without a buffer the call is the question "may this device path be connected", not an image check. */
	if (buffer == NULL) return firmware_security2->FileAuthentication(firmware_security2, file, buffer, bytes, boot_policy);
	hook_calls++;
	sb_verify_image(buffer, bytes, &databases, &result);
	say_verdict(sb_stage, "image at LoadImage", bytes, &result);
	if (mode == MODE_ENFORCE && !sb_verdict_allows(result.verdict)) {
		stage_say(sb_stage, SB_NAME, "refused");
		return EFI_ACCESS_DENIED;	/* the firmware core does not load the image */
	}
	return firmware_security2->FileAuthentication(firmware_security2, file, buffer, bytes, boot_policy);
}

/* The one handle that carries the protocol, or NULL. */
static EFI_HANDLE security2_owner(struct stage *stage, EFI_SECURITY2_ARCH_PROTOCOL *interface)
{
	EFI_HANDLE *handles = NULL, owner = NULL;
	UINTN count = 0, i, owners = 0;
	if (EFI_ERROR(stage->bs->LocateHandleBuffer(ByProtocol, &security2_guid, NULL, &count, &handles))) return NULL;
	for (i = 0; i < count; i++) {
		VOID *found = NULL;
		if (!EFI_ERROR(stage->bs->HandleProtocol(handles[i], &security2_guid, &found)) && found == interface) {
			owner = handles[i];
			owners++;
		}
	}
	if (handles != NULL) stage->bs->FreePool(handles);
	return owners == 1u ? owner : NULL;
}

static void hook(struct stage *stage)
{
	EFI_SECURITY2_ARCH_PROTOCOL *found = NULL;
	EFI_HANDLE owner;
	EFI_STATUS status;

	/* The line comes first: when the firmware stops in the next call, the console says where. */
	stage_say(stage, SB_NAME, "putting the image check in front of the Security2 protocol of the firmware");
	if (EFI_ERROR(stage->bs->LocateProtocol(&security2_guid, NULL, (VOID **)&found)) || found == NULL ||
	    found->FileAuthentication == NULL) {
		stage_say(stage, SB_NAME, "the firmware has no Security2 protocol: only the image that the stage starts is checked");
		return;
	}
	owner = security2_owner(stage, found);
	if (owner == NULL) {
		stage_say(stage, SB_NAME, "the Security2 protocol is not on exactly one handle: only the image that the stage "
					  "starts is checked");
		return;
	}
	firmware_security2 = found;
	stage_security2.FileAuthentication = check_file;
	status = stage->bs->ReinstallProtocolInterface(owner, &security2_guid, firmware_security2, &stage_security2);
	if (EFI_ERROR(status)) {
		stage_sayf(stage, SB_NAME, "the firmware did not take the image check, status 0x%llx: only the image that the "
					   "stage starts is checked", (unsigned long long)status);
		firmware_security2 = NULL;
		return;
	}
	security2_handle = owner;
	stage_say(stage, SB_NAME, "the image check is in place");
}

/* Puts the protocol of the firmware back. The module's structure is in the image of the stage, and the firmware
 * frees that image when the stage returns. So when the firmware does not take its own protocol back, the stage
 * must not return: it restarts the board. */
static void unhook(struct stage *stage)
{
	VOID *installed = NULL;
	EFI_STATUS status;
	if (security2_handle == NULL) return;
	/* An image that the stage started can put a protocol of its own in front of the stage's and return without
	 * an unwind. Its function then calls into the stage. The stage does not return in silence over that: the
	 * reinstall below fails, because the installed structure is not the one of the stage. */
	if (EFI_ERROR(stage->bs->HandleProtocol(security2_handle, &security2_guid, &installed)) ||
	    installed != &stage_security2 || stage_security2.FileAuthentication != check_file)
		stage_say(stage, SB_NAME, "another image changed the Security2 protocol after the stage and did not put it back");
	status = stage->bs->ReinstallProtocolInterface(security2_handle, &security2_guid, &stage_security2, firmware_security2);
	security2_handle = NULL;
	if (!EFI_ERROR(status)) {
		/* The line says that the unwind ended. A stop after it is not in the stage's check. */
		stage_say(stage, SB_NAME, "the Security2 protocol of the firmware is back in its place");
		return;
	}
	stage_sayf(stage, SB_NAME, "the firmware did not take its Security2 protocol back, status 0x%llx",
		   (unsigned long long)status);
	stage_say(stage, SB_NAME, "the stage cannot return with its check in the firmware: restart in 10 seconds");
	stage_log_flush(stage);
	stage->bs->Stall(SB_UNWIND_WAIT_US);
	stage->st->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
	for (;;) {}
}

/* ---- the state that Windows reads (sb.windows) --------------------------------------------------- */

/* One file of the kit, read for the filter that serves it to the operating system. The bytes are copied into
 * runtime memory by sb_windows_install, so the module frees them after the install. */
static enum sb_source read_served(struct stage *stage, const char *name, unsigned char **data, size_t *bytes)
{
	unsigned short path[STAGE_PATH_CHARS];
	enum stage_read read;
	*data = NULL;
	*bytes = 0;
	if (!stage_sibling(stage, name, path, STAGE_PATH_CHARS)) return SB_SOURCE_ABSENT;
	read = stage_read_file(stage, path, SB_DATABASE_MAX_BYTES, data, bytes);
	if (read == STAGE_READ_ABSENT) return SB_SOURCE_ABSENT;
	if (read != STAGE_READ_OK) {
		stage_sayf(stage, SB_NAME, "%s did not read: the operating system is not given it", name);
		*data = NULL;
		*bytes = 0;
		return SB_SOURCE_BROKEN;
	}
	return SB_SOURCE_GOOD;
}

static void windows_install(struct stage *stage)
{
	struct sb_windows_inputs inputs;
	unsigned char *pk, *kek, *db, *dbx;
	size_t pk_bytes, kek_bytes, db_bytes, dbx_bytes;
	int result;

	memset(&inputs, 0, sizeof(inputs));
	(void)read_served(stage, "pk.esl", &pk, &pk_bytes);
	(void)read_served(stage, "kek.esl", &kek, &kek_bytes);
	(void)read_served(stage, "db.esl", &db, &db_bytes);
	(void)read_served(stage, "dbx.esl", &dbx, &dbx_bytes);
	stage_sayf(stage, SB_NAME, "the operating system is offered PK %u, KEK %u, db %u and dbx %u bytes",
		   (unsigned)pk_bytes, (unsigned)kek_bytes, (unsigned)db_bytes, (unsigned)dbx_bytes);
	inputs.pk = pk;
	inputs.pk_bytes = pk_bytes;
	inputs.kek = kek;
	inputs.kek_bytes = kek_bytes;
	inputs.db = db;
	inputs.db_bytes = db_bytes;
	inputs.dbx = dbx;
	inputs.dbx_bytes = dbx_bytes;
	result = sb_windows_install(stage, &inputs);
	stage_free(stage, pk);
	stage_free(stage, kek);
	stage_free(stage, db);
	stage_free(stage, dbx);
	if (result != 0) {
		stage_sayf(stage, SB_NAME, "sb.windows=1: the state is not installed: %s", sb_windows_result_text(result));
		return;
	}
	stage_say(stage, SB_NAME, "Windows sees SecureBoot=1 from this stage (sb.windows=1), the firmware has no Secure Boot");
}

/* ---- the module ---------------------------------------------------------------------------------- */

static void sb_module_run(struct stage *stage)
{
	int windows;
	sb_stage = stage;
	mode = (int)stage_option_choice(&stage->options, "sb.mode", mode_names, 2u, MODE_AUDIT);
	windows = stage_option_flag(&stage->options, "sb.windows", 0);
	read_databases(stage);
	if (mode == MODE_ENFORCE && defect != NULL) {
		/* Fail closed: the operator asked for a refusal, and the inputs cannot give a sound verdict. */
		stage_sayf(stage, SB_NAME, "mode enforce: %s", defect);
		stage_say(stage, SB_NAME, "mode enforce with a defect in its inputs: the next image is refused");
		return;
	}
	stage_sayf(stage, SB_NAME, "mode %s: %s", mode_names[mode],
		   mode == MODE_ENFORCE ? "an image that is not allowed does not start"
					: "each image is checked and reported, and none is refused");
	hook(stage);
	if (!windows) return;
	if (mode != MODE_ENFORCE) {
		stage_say(stage, SB_NAME, "sb.windows=1 without sb.mode=enforce: ignored, the state is not installed");
		return;
	}
	windows_install(stage);
}

static enum stage_gate sb_module_gate(struct stage *stage, const unsigned char *image, size_t bytes)
{
	struct sb_result result;
	hook_calls = 0;
	if (mode == MODE_ENFORCE && defect != NULL) {
		stage_sayf(stage, SB_NAME, "the next image is refused: %s", defect);
		return STAGE_GATE_DENY;
	}
	if (image == NULL) {
		stage_say(stage, SB_NAME, "the next image did not read, so it is not checked");
		return mode == MODE_ENFORCE ? STAGE_GATE_DENY : STAGE_GATE_PASS;
	}
	sb_verify_image(image, bytes, &databases, &result);
	say_verdict(stage, "the next image", bytes, &result);
	if (mode != MODE_ENFORCE) return STAGE_GATE_PASS;
	return sb_verdict_allows(result.verdict) ? STAGE_GATE_BIND : STAGE_GATE_DENY;
}

static void sb_module_loaded(struct stage *stage, EFI_STATUS status)
{
	(void)status;
	/* The self-check of the hook: the load that the core just did must have come through it. */
	if (security2_handle != NULL)
		stage_sayf(stage, SB_NAME, "the firmware asked the image check %u time(s) for this load%s", hook_calls,
			   hook_calls ? "" : ": this firmware core does not call it, and the gate is the only check");
}

static void sb_module_unwind(struct stage *stage)
{
	sb_windows_unwind(stage);
	unhook(stage);
	firmware_security2 = NULL;
	stage_free(stage, db);
	stage_free(stage, dbx);
	db = dbx = NULL;
	defect = NULL;
	memset(&databases, 0, sizeof(databases));
}

/* probe=1 with sb=0: the two protocols that the hook needs, read and not changed. */
static void sb_module_probe(struct stage *stage)
{
	static const EFI_GUID security_guid = EFI_SECURITY_ARCH_PROTOCOL_GUID;
	EFI_SECURITY2_ARCH_PROTOCOL *found = NULL;
	VOID *older = NULL;
	if (EFI_ERROR(stage->bs->LocateProtocol(&security2_guid, NULL, (VOID **)&found)) || found == NULL)
		stage_say(stage, SB_NAME, "probe: the firmware has no Security2 protocol");
	else
		stage_sayf(stage, SB_NAME, "probe: Security2 protocol present, %s",
			   found->FileAuthentication == NULL ? "without a function"
			   : security2_owner(stage, found) != NULL ? "on exactly one handle"
								   : "not on exactly one handle");
	stage_sayf(stage, SB_NAME, "probe: Security protocol %s",
		   EFI_ERROR(stage->bs->LocateProtocol(&security_guid, NULL, &older)) || older == NULL ? "absent" : "present");
}

const struct stage_module sb_module = { SB_NAME, "sb", 1, sb_module_run, sb_module_probe, sb_module_gate,
					sb_module_loaded, sb_module_unwind };
