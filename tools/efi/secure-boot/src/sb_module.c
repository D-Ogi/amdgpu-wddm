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
 * not allowed. The module gives no Secure Boot variable to the operating system, and it calls SetVariable nowhere.
 */
#include <stddef.h>
#include <string.h>
#include "efi.h"
#include "efl_text.h"
#include "stage.h"
#include "sb_siglist.h"
#include "sb_verify.h"

#define SB_NAME			"secure-boot"
#define SB_DATABASE_MAX_BYTES	(1024u * 1024u)
static const EFI_GUID security2_guid = EFI_SECURITY2_ARCH_PROTOCOL_GUID;
enum { MODE_AUDIT = 0, MODE_ENFORCE = 1 };
static const char *const mode_names[] = { "audit", "enforce" };

static struct stage *sb_stage;
static int mode;
static unsigned char *db, *dbx;
static struct sb_databases databases;
static EFI_SECURITY2_ARCH_PROTOCOL *security2;
static EFI_SECURITY2_FILE_AUTHENTICATION firmware_check;
static unsigned hook_calls;

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

/* ---- the module ---------------------------------------------------------------------------------- */

static void sb_module_run(struct stage *stage)
{
	int have_db, have_dbx;
	sb_stage = stage;
	mode = (int)stage_option_choice(&stage->options, "sb.mode", mode_names, 2u, MODE_AUDIT);

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
}

static enum stage_gate sb_module_gate(struct stage *stage, const unsigned char *image, size_t bytes)
{
	struct sb_result result;
	hook_calls = 0;
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
	if (security2 != NULL)
		stage_sayf(stage, SB_NAME, "the firmware asked the image check %u time(s) for this load%s", hook_calls,
			   hook_calls ? "" : ": this firmware core does not call Security2, the gate is the only check");
}

static void sb_module_unwind(struct stage *stage)
{
	if (security2 != NULL && security2->FileAuthentication == check_file) security2->FileAuthentication = firmware_check;
	security2 = NULL;
	stage_free(stage, db);
	stage_free(stage, dbx);
	db = dbx = NULL;
	memset(&databases, 0, sizeof(databases));
}

const struct stage_module sb_module = { SB_NAME, "sb", sb_module_run, sb_module_gate, sb_module_loaded,
					sb_module_unwind };
