/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The Secure Boot check as a module of the BC-250 stage (docs/design/efi-resident-stage.md, "The Secure Boot
 * module"). This file is the UEFI binding. The verdict on an image comes from sb_verify.c.
 *
 * The module checks an image at two places:
 * - The gate: the core gives it the bytes of the Windows Boot Manager before the start.
 * - The hook: it replaces the FileAuthentication function of the firmware's EFI_SECURITY2_ARCH_PROTOCOL (PI
 *   Specification, volume 2), which the firmware core calls for each image that LoadImage loads. The firmware's
 *   own function still runs after the check.
 *
 * sb.mode=audit (the default) reports each verdict and refuses nothing. sb.mode=enforce refuses an image that is
 * not allowed. sb.reported=1 adds the variables that say "Secure Boot is on" to the operating system, from RAM,
 * through the runtime driver bc250-sbvar.efi. The module calls SetVariable nowhere.
 */
#include <stddef.h>
#include <string.h>
#include "efi.h"
#include "efl_text.h"
#include "stage.h"
#include "sb_siglist.h"
#include "sb_vars.h"
#include "sb_verify.h"

#define SB_NAME			"secure-boot"
#define SB_DATABASE_MAX_BYTES	(1024u * 1024u)
#define SB_DRIVER_MAX_BYTES	(1024u * 1024u)
#define SB_DRIVER_FILE		"bc250-sbvar.efi"

static const EFI_GUID security2_guid = EFI_SECURITY2_ARCH_PROTOCOL_GUID;
static const EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
/* EFI_GLOBAL_VARIABLE and EFI_IMAGE_SECURITY_DATABASE_GUID (edk2 MdePkg/Include/Guid, commit 8628578dacd3). */
static const EFI_GUID global_guid = { 0x8BE4DF61, 0x93CA, 0x11D2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } };
static const EFI_GUID database_guid = { 0xD719B2CB, 0x3D3A, 0x4596, { 0xA3, 0xBC, 0xDA, 0xD0, 0x0E, 0x67, 0x65, 0x6F } };
/* The vendor GUID of this project's own variable, made for it on 2026-11-13. */
static const EFI_GUID origin_guid = { 0x2E9685E0, 0xCA1D, 0x4D95, { 0x80, 0x7F, 0x6C, 0xC9, 0xD9, 0x86, 0x51, 0x43 } };
static const char origin_text[] =
	"Secure Boot variables served from RAM by bc250-stage. Not firmware Secure Boot. Keys: files on the ESP.";

enum { MODE_AUDIT = 0, MODE_ENFORCE = 1 };
static const char *const mode_names[] = { "audit", "enforce" };

static struct stage *sb_stage;
static int mode;
static unsigned char *db, *dbx;
static struct sb_databases databases;
static EFI_SECURITY2_ARCH_PROTOCOL *security2;
static EFI_SECURITY2_FILE_AUTHENTICATION firmware_check;
static unsigned hook_calls;
static struct sb_var_store *store;	/* filled when the reported mode is ready to start */
static int gate_allowed;

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
	if (result->verdict != SB_NOT_AN_IMAGE) stage_sayf(stage, SB_NAME, "  Authenticode SHA-256 %s", digest);
}

/* ---- the databases ------------------------------------------------------------------------------- */

/* Reads one database file beside the stage. 1 when it is there and well formed, 0 when it is absent, -1 when it
 * is there and cannot be used. */
static int read_database(struct stage *stage, const char *name, unsigned char **data, size_t *bytes)
{
	unsigned short path[STAGE_PATH_CHARS];
	struct sb_siglist_count count;
	enum stage_read read;
	*data = NULL;
	*bytes = 0;
	if (!stage_sibling(stage, name, path, STAGE_PATH_CHARS)) return 0;
	read = stage_read_file(stage, path, SB_DATABASE_MAX_BYTES, data, bytes);
	if (read == STAGE_READ_ABSENT) return 0;
	if (read != STAGE_READ_OK) {
		stage_sayf(stage, SB_NAME, "%s did not read", name);
		return -1;
	}
	if (!sb_siglist_count(*data, *bytes, &count)) {
		stage_sayf(stage, SB_NAME, "%s is not a well-formed signature list", name);
		stage_free(stage, *data);
		*data = NULL;
		*bytes = 0;
		return -1;
	}
	stage_sayf(stage, SB_NAME, "%s: %u certificates, %u SHA-256 digests", name, count.x509, count.sha256);
	if (count.other)
		stage_sayf(stage, SB_NAME, "%s: %u entries of a type that this stage does not read", name, count.other);
	return 1;
}

/* ---- the hook ------------------------------------------------------------------------------------ */

static EFI_STATUS EFIAPI check_file(const EFI_SECURITY2_ARCH_PROTOCOL *this, const EFI_DEVICE_PATH_PROTOCOL *file,
				    VOID *buffer, UINTN bytes, BOOLEAN boot_policy)
{
	struct sb_result result;
	/* Without a buffer the call is the question "may this device path be connected", not an image check. */
	if (buffer == NULL) return firmware_check(this, file, buffer, bytes, boot_policy);
	hook_calls++;
	sb_verify_image(buffer, bytes, &databases, &result);
	say_verdict(sb_stage, "image at LoadImage", bytes, &result);
	if (mode == MODE_ENFORCE && !sb_verdict_allows(result.verdict)) {
		stage_say(sb_stage, SB_NAME, "refused");
		return EFI_ACCESS_DENIED;	/* the firmware core does not load the image */
	}
	return firmware_check(this, file, buffer, bytes, boot_policy);
}

static void hook(struct stage *stage)
{
	if (EFI_ERROR(stage->bs->LocateProtocol(&security2_guid, NULL, (VOID **)&security2)) || security2 == NULL ||
	    security2->FileAuthentication == NULL) {
		security2 = NULL;
		stage_say(stage, SB_NAME, "the firmware has no Security2 protocol: only the image that the stage starts is checked");
		return;
	}
	firmware_check = security2->FileAuthentication;
	security2->FileAuthentication = check_file;
}

/* ---- the reported mode --------------------------------------------------------------------------- */

static int firmware_has(struct stage *stage, const CHAR16 *name, const EFI_GUID *guid)
{
	EFI_GUID vendor = *guid;
	CHAR16 copy[SB_VARS_NAME_CHARS];
	UINTN size = 0;
	EFI_STATUS status;
	size_t i;
	for (i = 0; i + 1u < SB_VARS_NAME_CHARS && name[i]; i++) copy[i] = name[i];
	copy[i] = 0;
	status = stage->st->RuntimeServices->GetVariable(copy, &vendor, NULL, &size, NULL);
	return status != EFI_NOT_FOUND;
}

static int add_file(struct stage *stage, const char *file, const CHAR16 *name, const EFI_GUID *guid, int required)
{
	unsigned char *data;
	size_t bytes;
	int have = read_database(stage, file, &data, &bytes), added;
	if (have != 1) {
		if (have == 0 && required) stage_sayf(stage, SB_NAME, "%s is not beside the stage", file);
		return have == 0 && !required;
	}
	added = sb_vars_add(store, guid, name, SB_VAR_BOOTSERVICE_ACCESS | SB_VAR_RUNTIME_ACCESS, data, bytes);
	stage_free(stage, data);
	if (!added) stage_sayf(stage, SB_NAME, "%s does not fit into the variable store", file);
	return added;
}

/* Builds the variable store. On any failure there is no store, and the reported mode does not start. */
static void prepare_reported(struct stage *stage)
{
	static const struct { const CHAR16 *name; const EFI_GUID *guid; } names[] = {
		{ u"SecureBoot", &global_guid }, { u"SetupMode", &global_guid }, { u"PK", &global_guid },
		{ u"KEK", &global_guid }, { u"db", &database_guid }, { u"dbx", &database_guid }
	};
	const UINT32 attributes = SB_VAR_BOOTSERVICE_ACCESS | SB_VAR_RUNTIME_ACCESS;
	const UINT8 on = 1, off = 0;
	size_t i;
	int good;

	if (mode != MODE_ENFORCE) {
		stage_say(stage, SB_NAME, "sb.reported=1 needs sb.mode=enforce with a usable db: not reported");
		return;
	}
	if (!stage_board_bc250(stage)) {
		stage_say(stage, SB_NAME, "sb.reported=1 is for the BC-250 firmware only: not reported");
		return;
	}
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (firmware_has(stage, names[i].name, names[i].guid)) {
			stage_say(stage, SB_NAME, "the firmware has Secure Boot variables of its own: not reported");
			return;
		}
	store = stage_alloc(stage, sizeof(*store));
	if (store == NULL) {
		stage_say(stage, SB_NAME, "no memory for the variable store: not reported");
		return;
	}
	sb_vars_init(store);
	good = sb_vars_add(store, &global_guid, u"SecureBoot", attributes, &on, 1u) &&
	       sb_vars_add(store, &global_guid, u"SetupMode", attributes, &off, 1u) &&
	       add_file(stage, "PK.esl", u"PK", &global_guid, 1) && add_file(stage, "KEK.esl", u"KEK", &global_guid, 1) &&
	       sb_vars_add(store, &database_guid, u"db", attributes, databases.db, databases.db_bytes) &&
	       (databases.dbx == NULL ||
		sb_vars_add(store, &database_guid, u"dbx", attributes, databases.dbx, databases.dbx_bytes)) &&
	       sb_vars_add(store, &origin_guid, u"Bc250StageSecureBoot", attributes, origin_text, sizeof(origin_text));
	if (!good) {
		stage_say(stage, SB_NAME, "the variable store is not complete: not reported");
		stage_free(stage, store);
		store = NULL;
	}
}

/* Loads and starts the runtime driver with the store. The driver is an image like any other: it must pass the
 * same check. */
static void start_reported(struct stage *stage)
{
	unsigned short path[STAGE_PATH_CHARS];
	EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
	EFI_HANDLE driver = NULL;
	struct sb_result result;
	unsigned char *image = NULL;
	size_t bytes = 0;
	EFI_STATUS status;

	if (!stage_sibling(stage, SB_DRIVER_FILE, path, STAGE_PATH_CHARS) ||
	    stage_read_file(stage, path, SB_DRIVER_MAX_BYTES, &image, &bytes) != STAGE_READ_OK) {
		stage_say(stage, SB_NAME, SB_DRIVER_FILE " is not beside the stage or did not read: not reported");
		return;
	}
	sb_verify_image(image, bytes, &databases, &result);
	say_verdict(stage, SB_DRIVER_FILE, bytes, &result);
	if (!sb_verdict_allows(result.verdict)) {
		stage_say(stage, SB_NAME, "the runtime driver is not allowed by db: not reported");
		stage_free(stage, image);
		return;
	}
	status = stage->bs->LoadImage(0, stage->image, NULL, image, bytes, &driver);
	stage_free(stage, image);
	if (EFI_ERROR(status) || driver == NULL ||
	    EFI_ERROR(stage->bs->HandleProtocol(driver, &loaded_image_guid, (VOID **)&loaded)) || loaded == NULL) {
		stage_sayf(stage, SB_NAME, "the runtime driver did not load, status 0x%llx: not reported",
			   (unsigned long long)status);
		if (driver != NULL) stage->bs->UnloadImage(driver);
		return;
	}
	loaded->LoadOptions = store;
	loaded->LoadOptionsSize = (UINT32)sizeof(*store);
	status = stage->bs->StartImage(driver, NULL, NULL);
	if (EFI_ERROR(status)) {
		stage_sayf(stage, SB_NAME, "the runtime driver refused to start, status 0x%llx: not reported",
			   (unsigned long long)status);
		return;
	}
	stage_say(stage, SB_NAME, "reported mode: SecureBoot=1 and SetupMode=0 are served from RAM until the next restart");
	stage_say(stage, SB_NAME, "reported mode: Windows refuses its test-signing mode while this is on");
}

/* ---- the module ---------------------------------------------------------------------------------- */

static void sb_module_run(struct stage *stage)
{
	int have_db, have_dbx, reported;
	sb_stage = stage;
	mode = (int)stage_option_choice(&stage->options, "sb.mode", mode_names, 2u, MODE_AUDIT);
	reported = stage_option_flag(&stage->options, "sb.reported", 0);

	have_db = read_database(stage, "db.esl", &db, &databases.db_bytes);
	have_dbx = read_database(stage, "dbx.esl", &dbx, &databases.dbx_bytes);
	databases.db = db;
	databases.dbx = dbx;
	if (have_db == 0) stage_say(stage, SB_NAME, "no db.esl beside the stage: no image can be allowed");
	if (have_dbx == 0) stage_say(stage, SB_NAME, "no dbx.esl beside the stage: no image is forbidden");
	/* The stage fails open on a defect of its own inputs: without a usable db, or with a dbx that is there and
	 * does not read, enforce would refuse or allow for the wrong reason. */
	if (mode == MODE_ENFORCE && (have_db != 1 || have_dbx < 0)) {
		stage_say(stage, SB_NAME, "sb.mode=enforce needs a usable db.esl and no broken dbx.esl: the mode is audit");
		mode = MODE_AUDIT;
	}
	stage_sayf(stage, SB_NAME, "mode %s: %s", mode_names[mode],
		   mode == MODE_ENFORCE ? "an image that is not allowed does not start"
					: "each image is checked and reported, and none is refused");
	hook(stage);
	if (reported) prepare_reported(stage);
}

static enum stage_gate sb_module_gate(struct stage *stage, const unsigned char *image, size_t bytes)
{
	struct sb_result result;
	gate_allowed = 0;
	hook_calls = 0;
	if (image == NULL) {
		stage_say(stage, SB_NAME, "the next image did not read, so it is not checked");
		return mode == MODE_ENFORCE ? STAGE_GATE_DENY : STAGE_GATE_PASS;
	}
	sb_verify_image(image, bytes, &databases, &result);
	say_verdict(stage, "the next image", bytes, &result);
	gate_allowed = sb_verdict_allows(result.verdict);
	if (mode != MODE_ENFORCE) return STAGE_GATE_PASS;
	return gate_allowed ? STAGE_GATE_BIND : STAGE_GATE_DENY;
}

static void sb_module_loaded(struct stage *stage, EFI_STATUS status)
{
	/* The self-check of the hook: the load that the core just did must have come through it. */
	if (security2 != NULL)
		stage_sayf(stage, SB_NAME, "the firmware asked the image check %u time(s) for this load%s", hook_calls,
			   hook_calls ? "" : ": this firmware core does not call Security2, the gate is the only check");
	if (store != NULL && !EFI_ERROR(status) && gate_allowed) start_reported(stage);
}

static void sb_module_unwind(struct stage *stage)
{
	if (security2 != NULL && security2->FileAuthentication == check_file) security2->FileAuthentication = firmware_check;
	security2 = NULL;
	stage_free(stage, store);
	stage_free(stage, db);
	stage_free(stage, dbx);
	store = NULL;
	db = dbx = NULL;
	memset(&databases, 0, sizeof(databases));
}

const struct stage_module sb_module = { SB_NAME, "sb", sb_module_run, sb_module_gate, sb_module_loaded,
					sb_module_unwind };
