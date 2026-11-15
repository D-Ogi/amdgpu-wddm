/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The pre-OS TPM as a module of the BC-250 stage (docs/design/tpm-preos.md). This file is the UEFI binding. The
 * TPM itself is libtpms behind tools/efi/tpm/engine/tpm_engine.h. The event log, the TPM 2.0 commands, the
 * measurement order and the logic of EFI_TCG2_PROTOCOL are pure code in tl_log.c, tl_tpm2.c, tl_measure.c and
 * tl_tcg2.c, with host tests.
 *
 * The run:
 * 1. Only on a positively identified BC-250, and only when the firmware has no TCG2 protocol of its own.
 * 2. The engine starts from the state file of the stage volume (driver/tpm/statefile.h), or from a new state (a
 *    manufactured TPM) when neither copy is there, and gets TPM2_Startup(CLEAR). A copy that is there and does not
 *    open gives no TPM: a new TPM in its place would lose every sealed key in silence. The PCRs are set to the
 *    SHA-256 bank only; when the TPM has other banks, TPM2_PCR_Allocate and a restart of the engine do this.
 * 3. The event log and the final events table are made, and the events before the boot application are measured.
 * 4. EFI_TCG2_PROTOCOL is installed, and an event waits for ExitBootServices.
 * 5. The handoff range is reserved, and stage->tpm_handoff tells the ACPI repair where it is.
 * The gate measures the next image into PCR 4 and binds the start to those bytes. At ExitBootServices the module
 * measures the two PCR 5 events and writes the libtpms state into the handoff range for the Windows core.
 *
 * The stage only reads the state files. The Windows core writes them, from the handoff and at each NV change.
 *
 * The callbacks of ExitBootServices must not call the memory services of the firmware (UEFI 2.10, section 7.4.6).
 * The module therefore allocates every buffer of that path in its run.
 */
#include <stddef.h>
#include <string.h>
#include <cpuid.h>
#include "efi.h"
#include "efl_pe.h"
#include "efl_sha256.h"
#include "stage.h"
#include "handoff.h"
#include "statefile.h"
#include "tl_log.h"
#include "tl_measure.h"
#include "tl_tcg2.h"
#include "tl_tpm2.h"
#include "tpm_engine.h"

#define TPM_NAME		"tpm"
#define PAGE_BYTES		4096u
/* libtpms and OpenSSL. The host test of the engine measured a peak of 567 KiB (2026-11-14); the log line of the run
 * gives the peak on the board. */
#define ARENA_BYTES		(4u * 1024u * 1024u)
#define LOG_BYTES		(64u * 1024u)
#define FINAL_BYTES		(32u * 1024u)
#define BUFFER_BYTES		4096u			/* MaxCommandSize and MaxResponseSize */
#define STATE_BYTES		(TPM_HANDOFF_MAX_BYTES - TPM_HANDOFF_HEADER_BYTES)
#define VARIABLE_MAX_BYTES	(256u * 1024u)
#define STAGE_FILE_MAX_BYTES	(16u * 1024u * 1024u)

static const EFI_GUID tcg2_guid = EFI_TCG2_PROTOCOL_GUID;
static const EFI_GUID final_events_guid = EFI_TCG2_FINAL_EVENTS_TABLE_GUID;
/* EFI_GLOBAL_VARIABLE and EFI_IMAGE_SECURITY_DATABASE_GUID (UEFI 2.10, sections 3.3 and 32.6.1). */
static const EFI_GUID global_guid = { 0x8BE4DF61, 0x93CA, 0x11D2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } };
static const EFI_GUID database_guid = { 0xD719B2CB, 0x3D3A, 0x4596, { 0xA3, 0xBC, 0xDA, 0xD0, 0x0E, 0x67, 0x65, 0x6F } };
static const EFI_GUID device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;

#define MAX_BANKS		8u
#define PAGE_BLOCKS		5u	/* arena, log, final table, state buffer, handoff range */

/* ---- the state of the module ------------------------------------------------------------------------------ */

static struct {
	struct stage *stage;
	int present;			/* the TPM answers: cleared at ExitBootServices */
	int engine_initialized;		/* tpm_engine_init succeeded: the arena belongs to the engine until the wipe */
	int engine_running;		/* tpm_engine_start succeeded */
	int installed;			/* the protocol is on tcg2_handle */
	EFI_HANDLE tcg2_handle;
	EFI_EVENT exit_event;
	struct tl_log log;
	struct tl_measurer measurer;
	struct tl_capability capability;
	unsigned char command[BUFFER_BYTES], response[BUFFER_BYTES];
	unsigned char volume[16];	/* the GPT partition GUID of the stage volume, zeros when not known */
	unsigned long long ticks_per_ms;
	unsigned char *state;		/* STATE_BYTES for the two states at ExitBootServices */
	unsigned char *handoff;		/* the reserved range */
	unsigned long long sequence;	/* the sequence of the state file that the TPM started from, 0 for a new TPM */
	int handoff_written;
	int final_installed;		/* the final events table is in the system table */
	/* The pages of the module, given back when the TPM is not offered or the boot application returns. */
	struct { EFI_PHYSICAL_ADDRESS address; UINTN pages; } block[PAGE_BLOCKS];
	unsigned blocks;
} tpm;

static EFI_TCG2_PROTOCOL tcg2;

/* ---- the platform of the engine ---------------------------------------------------------------------------- */

static unsigned long long rdtsc(void)
{
	unsigned lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long long)hi << 32) | lo;
}

static unsigned long long monotonic_ms(void *context)
{
	(void)context;
	return rdtsc() / tpm.ticks_per_ms;
}

/* The time stamp counter of Zen 2 runs at a constant rate (CPUID 0x80000007 EDX bit 8, checked in the run). */
static int calibrate(struct stage *stage)
{
	unsigned a, b, c, d;
	unsigned long long start;
	if (!__get_cpuid(0x80000007u, &a, &b, &c, &d) || !(d & (1u << 8))) return 0;
	start = rdtsc();
	stage->bs->Stall(10000);
	tpm.ticks_per_ms = (rdtsc() - start) / 10u;
	return tpm.ticks_per_ms != 0;
}

static int has_rdseed(void)
{
	unsigned a, b, c, d;
	return __get_cpuid_count(7u, 0u, &a, &b, &c, &d) && (b & (1u << 18));
}

static int has_rdrand(void)
{
	unsigned a, b, c, d;
	return __get_cpuid(1u, &a, &b, &c, &d) && (c & (1u << 30));
}

/* One 64-bit word of RDSEED, else of RDRAND; each with the retry bound of the Intel DRNG guide (section 5.2). */
static int random_word(unsigned long long *word)
{
	unsigned char ok = 0;
	unsigned i;
	if (has_rdseed())
		for (i = 0; i < 100u && !ok; i++) __asm__ volatile("rdseed %0; setc %1" : "=r"(*word), "=qm"(ok));
	if (!ok && has_rdrand())
		for (i = 0; i < 10u && !ok; i++) __asm__ volatile("rdrand %0; setc %1" : "=r"(*word), "=qm"(ok));
	return ok;
}

static int entropy(void *context, unsigned char *out, size_t n)
{
	unsigned long long word;
	size_t i;
	(void)context;
	while (n > 0) {
		if (!random_word(&word)) return 0;
		for (i = 0; i < 8u && n > 0; i++, n--) *out++ = (unsigned char)(word >> (8u * i));
	}
	return 1;
}

/* The diagnostics of libtpms and OpenSSL: each piece of text as one console line, cut at the line end. */
static void engine_log(void *context, const char *text, size_t bytes)
{
	char line[160];
	size_t n = bytes < sizeof(line) - 1u ? bytes : sizeof(line) - 1u;
	memcpy(line, text, n);
	while (n > 0 && (line[n - 1u] == '\n' || line[n - 1u] == '\r')) n--;
	line[n] = 0;
	if (n > 0) stage_say((struct stage *)context, TPM_NAME, line);
}

/* ---- commands that the module sends itself --------------------------------------------------------------- */

/* Sends tpm.command (n bytes) and returns the response code. */
static unsigned send(size_t n)
{
	size_t got;
	if (n == 0) return TL_TPM2_MALFORMED;
	got = tpm_engine_process(tpm.command, n, tpm.response, sizeof(tpm.response));
	return tl_tpm2_rc(tpm.response, got);
}

static int extend(void *context, unsigned pcr, const unsigned char digest[TL_SHA256_BYTES])
{
	(void)context;
	if (!tpm.present) return 0;
	return send(tl_tpm2_pcr_extend(tpm.command, sizeof(tpm.command), pcr, digest)) == 0;
}

/* Sets the SHA-256 bank as the only bank. A change takes effect at the next TPM reset (TPM 2.0 Part 3, section
 * 22.5): the engine stops and starts again from its permanent state. 1 on success. */
static int sha256_only(struct stage *stage)
{
	int only = 0, full = 0, success = 0;
	unsigned banks[MAX_BANKS], others[MAX_BANKS], count = 0, n = 0, i;
	size_t bytes, got;
	unsigned rc;

	got = tpm_engine_process(tpm.command, tl_tpm2_get_pcrs(tpm.command, sizeof(tpm.command)), tpm.response,
				 sizeof(tpm.response));
	rc = tl_tpm2_get_pcrs_answer(tpm.response, got, &only, &full);
	if (rc == 0) rc = tl_tpm2_get_pcrs_banks(tpm.response, got, banks, MAX_BANKS, &count);
	if (rc != 0) {
		stage_sayf(stage, TPM_NAME, "GetCapability(PCRS) failed, rc 0x%x", rc);
		return 0;
	}
	if (only && full) return 1;

	/* Only the banks that the TPM implements: an algorithm that it does not know fails the command. */
	for (i = 0; i < count; i++)
		if (banks[i] != TL_ALG_SHA256) others[n++] = banks[i];
	got = tpm_engine_process(tpm.command, tl_tpm2_pcr_allocate_sha256(tpm.command, sizeof(tpm.command), others, n),
				 tpm.response, sizeof(tpm.response));
	rc = tl_tpm2_pcr_allocate_answer(tpm.response, got, &success);
	if (rc != 0 || !success) {
		stage_sayf(stage, TPM_NAME, "PCR_Allocate failed, rc 0x%x", rc);
		return 0;
	}
	rc = send(tl_tpm2_shutdown(tpm.command, sizeof(tpm.command), TL_TPM_SU_CLEAR));
	if (rc != 0) {
		stage_sayf(stage, TPM_NAME, "Shutdown before the restart failed, rc 0x%x", rc);
		return 0;
	}
	bytes = tpm_engine_get_state(TPM_ENGINE_PERMANENT, tpm.state, STATE_BYTES);
	tpm_engine_stop();
	if (bytes == 0 || tpm_engine_start(tpm.state, bytes, NULL, 0, BUFFER_BYTES) != 0) {
		stage_say(stage, TPM_NAME, "the engine did not start again after PCR_Allocate");
		return 0;
	}
	rc = send(tl_tpm2_startup(tpm.command, sizeof(tpm.command), TL_TPM_SU_CLEAR));
	if (rc != 0) {
		stage_sayf(stage, TPM_NAME, "Startup after PCR_Allocate failed, rc 0x%x", rc);
		return 0;
	}
	got = tpm_engine_process(tpm.command, tl_tpm2_get_pcrs(tpm.command, sizeof(tpm.command)), tpm.response,
				 sizeof(tpm.response));
	if (tl_tpm2_get_pcrs_answer(tpm.response, got, &only, &full) != 0 || !only || !full) {
		stage_say(stage, TPM_NAME, "the SHA-256 bank is not the only bank after PCR_Allocate");
		return 0;
	}
	stage_say(stage, TPM_NAME, "PCR banks: SHA-256 only, after PCR_Allocate and a restart");
	return 1;
}

static int manufacturer(struct stage *stage)
{
	size_t got = tpm_engine_process(tpm.command, tl_tpm2_get_property(tpm.command, sizeof(tpm.command),
					TL_TPM_PT_MANUFACTURER), tpm.response, sizeof(tpm.response));
	unsigned rc = tl_tpm2_get_property_answer(tpm.response, got, TL_TPM_PT_MANUFACTURER, &tpm.capability.manufacturer);
	if (rc != 0) stage_sayf(stage, TPM_NAME, "GetCapability(TPM_PT_MANUFACTURER) failed, rc 0x%x", rc);
	return rc == 0;
}

/* ---- EFI_TCG2_PROTOCOL --------------------------------------------------------------------------------- */

static EFI_STATUS efi_status(enum tl_status status)
{
	switch (status) {
	case TL_OK: return EFI_SUCCESS;
	case TL_INVALID_PARAMETER: return EFI_INVALID_PARAMETER;
	case TL_BUFFER_TOO_SMALL: return EFI_BUFFER_TOO_SMALL;
	case TL_VOLUME_FULL: return EFI_VOLUME_FULL;
	case TL_UNSUPPORTED: return EFI_UNSUPPORTED;
	default: return EFI_DEVICE_ERROR;
	}
}

static EFI_STATUS EFIAPI get_capability(EFI_TCG2_PROTOCOL *This, VOID *ProtocolCapability)
{
	if (This != &tcg2 || ProtocolCapability == NULL) return EFI_INVALID_PARAMETER;
	tpm.capability.present = tpm.present;
	return efi_status(tl_tcg2_capability(&tpm.capability, ProtocolCapability));
}

static EFI_STATUS EFIAPI get_event_log(EFI_TCG2_PROTOCOL *This, UINT32 EventLogFormat, EFI_PHYSICAL_ADDRESS *EventLogLocation,
				       EFI_PHYSICAL_ADDRESS *EventLogLastEntry, BOOLEAN *EventLogTruncated)
{
	if (This != &tcg2 || EventLogFormat != TL_LOG_FORMAT_TCG_2) return EFI_INVALID_PARAMETER;
	if (!tpm.present) {
		/* Section 6.5: no TPM, no log. */
		if (EventLogLocation != NULL) *EventLogLocation = 0;
		if (EventLogLastEntry != NULL) *EventLogLastEntry = 0;
		if (EventLogTruncated != NULL) *EventLogTruncated = 0;
		return EFI_SUCCESS;
	}
	if (EventLogLocation != NULL) *EventLogLocation = (EFI_PHYSICAL_ADDRESS)(UINTN)tpm.log.area;
	if (EventLogLastEntry != NULL) *EventLogLastEntry = (EFI_PHYSICAL_ADDRESS)(UINTN)(tpm.log.area + tl_log_last(&tpm.log));
	if (EventLogTruncated != NULL) *EventLogTruncated = (BOOLEAN)(tpm.log.truncated != 0);
	tl_log_final_start(&tpm.log);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI hash_log_extend_event(EFI_TCG2_PROTOCOL *This, UINT64 Flags, EFI_PHYSICAL_ADDRESS DataToHash,
					       UINT64 DataToHashLen, VOID *EfiTcgEvent)
{
	if (This != &tcg2) return EFI_INVALID_PARAMETER;
	return efi_status(tl_tcg2_hash_log_extend(&tpm.measurer, tpm.present, Flags, (const unsigned char *)(UINTN)DataToHash,
						  DataToHashLen, EfiTcgEvent));
}

static EFI_STATUS EFIAPI submit_command(EFI_TCG2_PROTOCOL *This, UINT32 InputParameterBlockSize, UINT8 *InputParameterBlock,
					UINT32 OutputParameterBlockSize, UINT8 *OutputParameterBlock)
{
	size_t got;
	if (This != &tcg2 || InputParameterBlock == NULL || OutputParameterBlock == NULL || InputParameterBlockSize == 0 ||
	    OutputParameterBlockSize == 0)
		return EFI_INVALID_PARAMETER;
	if (!tpm.present) return EFI_DEVICE_ERROR;
	if (InputParameterBlockSize > BUFFER_BYTES) return EFI_INVALID_PARAMETER;
	got = tpm_engine_process(InputParameterBlock, InputParameterBlockSize, OutputParameterBlock, OutputParameterBlockSize);
	/* The engine returns 0 when the answer does not fit (tpm_engine.h). */
	return got == 0 ? EFI_BUFFER_TOO_SMALL : EFI_SUCCESS;
}

static EFI_STATUS EFIAPI get_active_pcr_banks(EFI_TCG2_PROTOCOL *This, UINT32 *ActivePcrBanks)
{
	if (This != &tcg2 || ActivePcrBanks == NULL) return EFI_INVALID_PARAMETER;
	*ActivePcrBanks = TL_HASH_ALG_SHA256;
	return EFI_SUCCESS;
}

/* The bank set is fixed: SHA-256 only. A request for that set is a no-op (section 6.9), every other is refused. */
static EFI_STATUS EFIAPI set_active_pcr_banks(EFI_TCG2_PROTOCOL *This, UINT32 ActivePcrBanks)
{
	if (This != &tcg2) return EFI_INVALID_PARAMETER;
	return ActivePcrBanks == TL_HASH_ALG_SHA256 ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI get_result_of_set_active_pcr_banks(EFI_TCG2_PROTOCOL *This, UINT32 *OperationPresent,
							    UINT32 *Response)
{
	if (This != &tcg2 || OperationPresent == NULL || Response == NULL) return EFI_INVALID_PARAMETER;
	*OperationPresent = 0;
	*Response = 0;
	return EFI_SUCCESS;
}

static EFI_TCG2_PROTOCOL tcg2 = {
	get_capability, get_event_log, hash_log_extend_event, submit_command,
	get_active_pcr_banks, set_active_pcr_banks, get_result_of_set_active_pcr_banks
};

/* ---- ExitBootServices ------------------------------------------------------------------------------------ */

/* No memory service of the firmware here: every buffer exists since the run. */
static VOID EFIAPI exit_boot_services(EFI_EVENT Event, VOID *Context)
{
	size_t permanent, volatile_bytes;
	unsigned flags = TPM_HANDOFF_STARTED;
	(void)Event;
	(void)Context;
	if (!tpm.present) return;
	tl_measure_exit_boot_services(&tpm.measurer);
	/* A new TPM, or NV that changed in the stage (the clock, the lockout counter): the core stores it. */
	if (tpm.sequence == 0 || tpm_engine_permanent_dirty()) flags |= TPM_HANDOFF_PERMANENT_CHANGED;
	permanent = tpm_engine_get_state(TPM_ENGINE_PERMANENT, tpm.state, STATE_BYTES);
	volatile_bytes = permanent ? tpm_engine_get_state(TPM_ENGINE_VOLATILE, tpm.state + permanent, STATE_BYTES - permanent) : 0;
	if (permanent && volatile_bytes)
		tpm.handoff_written = tpm_handoff_write(tpm.handoff, TPM_HANDOFF_MAX_BYTES, tpm.volume, tpm.sequence, flags, tpm.state,
							(unsigned)permanent, tpm.state + permanent, (unsigned)volatile_bytes,
							efl_sha256) != 0;
	/* Without a record the range keeps zeros: the core finds no magic and the device has no TPM. */
	tpm.present = 0;
}

/* ---- the run ------------------------------------------------------------------------------------------- */

/* below_4g: the pages end below 4 GiB (AllocateMaxAddress). */
static void *pages(struct stage *stage, EFI_MEMORY_TYPE type, size_t bytes, int below_4g)
{
	EFI_PHYSICAL_ADDRESS page = below_4g ? 0xFFFFFFFFull : 0;
	UINTN count = (bytes + PAGE_BYTES - 1u) / PAGE_BYTES;
	EFI_STATUS status;
	if (tpm.blocks == PAGE_BLOCKS) return NULL;
	status = ((EFI_ALLOCATE_PAGES)stage->bs->AllocatePages)(below_4g ? AllocateMaxAddress : AllocateAnyPages, type,
								    count, &page);
	if (EFI_ERROR(status) || page == 0) {
		stage_sayf(stage, TPM_NAME, "AllocatePages of %u bytes failed, status 0x%llx", (unsigned)bytes,
			   (unsigned long long)status);
		return NULL;
	}
	tpm.block[tpm.blocks].address = page;
	tpm.block[tpm.blocks++].pages = count;
	memset((void *)(UINTN)page, 0, bytes);
	return (void *)(UINTN)page;
}

static void free_pages(struct stage *stage)
{
	while (tpm.blocks > 0) {
		tpm.blocks--;
		((EFI_FREE_PAGES)stage->bs->FreePages)(tpm.block[tpm.blocks].address, tpm.block[tpm.blocks].pages);
	}
	tpm.state = NULL;
	tpm.handoff = NULL;
}

/* The GPT partition GUID of the stage volume: the hard drive node of its device path (UEFI 2.10, section
 * 10.3.5.1), signature type 2. */
static void find_volume(struct stage *stage)
{
	const unsigned char *node = NULL;
	unsigned length;
	unsigned guard;
	if (EFI_ERROR(stage->bs->HandleProtocol(stage->loaded->DeviceHandle, &device_path_guid, (VOID **)&node)) || node == NULL)
		return;
	for (guard = 0; guard < 64u && node[0] != EFI_DP_TYPE_END; guard++, node += length) {
		length = node[2] | ((unsigned)node[3] << 8);
		if (length < 4u) return;
		if (node[0] == EFI_DP_TYPE_MEDIA && node[1] == 0x01u && length >= 42u && node[41] == 2u) {
			memcpy(tpm.volume, node + 24, 16);
			return;
		}
	}
}

static int read_variable(struct stage *stage, struct tl_variable *v, const CHAR16 *name, const EFI_GUID *guid)
{
	EFI_GUID vendor = *guid;
	UINTN size = 0;
	EFI_STATUS status;
	unsigned char *data;
	v->name = name;
	memcpy(v->guid, guid, 16);
	v->data = NULL;
	v->bytes = 0;
	status = stage->st->RuntimeServices->GetVariable((CHAR16 *)name, &vendor, NULL, &size, NULL);
	if (status == EFI_NOT_FOUND) return 1;	/* measured with no data, as edk2 does */
	if (status != EFI_BUFFER_TOO_SMALL || size == 0 || size > VARIABLE_MAX_BYTES) return 0;
	data = stage_alloc(stage, (size_t)size);
	if (data == NULL) return 0;
	status = stage->st->RuntimeServices->GetVariable((CHAR16 *)name, &vendor, NULL, &size, data);
	if (EFI_ERROR(status)) {
		stage_free(stage, data);
		return 0;
	}
	v->data = data;
	v->bytes = (size_t)size;
	return 1;
}

/* The events before the boot application. 0 when the TPM must not be offered: an unmeasured stage, or a failed
 * extend, would give PCR values that do not describe this start. */
static int measure_preboot(struct stage *stage)
{
	static const CHAR16 *const names[TL_SECURE_VARIABLES] = { u"SecureBoot", u"PK", u"KEK", u"db", u"dbx" };
	struct tl_preboot p;
	struct efl_pe pe;
	unsigned char *file = NULL, *scratch;
	size_t file_bytes = 0, scratch_bytes = 0, i;
	unsigned failed;
	int ok = 1;

	if (!stage->image_file_known ||
	    stage_read_file(stage, stage->image_file, STAGE_FILE_MAX_BYTES, &file, &file_bytes) != STAGE_READ_OK) {
		stage_say(stage, TPM_NAME, "the stage file did not read: no TPM");
		return 0;
	}
	ok = efl_pe_read(file, file_bytes, &pe);
	stage_free(stage, file);
	if (!ok) {
		stage_say(stage, TPM_NAME, "the stage file is not a sound PE image: no TPM");
		return 0;
	}
	p.version = STAGE_VERSION;
	memcpy(p.stage_digest, pe.digest, sizeof(p.stage_digest));
	for (i = 0; i < TL_SECURE_VARIABLES; i++) {
		if (!read_variable(stage, &p.secure[i], names[i], i < 3u ? &global_guid : &database_guid)) {
			stage_sayf(stage, TPM_NAME, "Secure Boot variable %u did not read: no TPM", (unsigned)i);
			ok = 0;
		}
		if (p.secure[i].bytes > scratch_bytes) scratch_bytes = p.secure[i].bytes;
	}
	scratch_bytes += 64u + 2u * 16u;	/* UEFI_VARIABLE_DATA: GUID, two UINT64, the name of up to 15 characters */
	scratch = ok ? stage_alloc(stage, scratch_bytes) : NULL;
	if (ok && scratch == NULL) ok = 0;
	if (ok) {
		failed = tl_measure_preboot(&tpm.measurer, &p, scratch, scratch_bytes);
		if (failed) {
			stage_sayf(stage, TPM_NAME, "%u events before the boot application did not extend: no TPM", failed);
			ok = 0;
		}
		stage_free(stage, scratch);
	}
	for (i = 0; i < TL_SECURE_VARIABLES; i++) stage_free(stage, (void *)p.secure[i].data);
	return ok;
}

/* ---- the state files ---------------------------------------------------------------------------------- */

struct state_copy {
	enum stage_read read;		/* STAGE_READ_ABSENT: no file */
	enum tpm_statefile_result result;
	unsigned long long sequence;
	size_t bytes;
};

static const char *copy_text(const struct state_copy *c)
{
	static const char *const results[] = { "sound", "not a state file", "a damaged frame", "another key or a changed byte",
					       "too large" };
	if (c->read == STAGE_READ_ABSENT) return "absent";
	if (c->read == STAGE_READ_FAILED) return "unread";
	return results[c->result];
}

/* Reads one copy and opens it into out. */
static void read_copy(struct stage *stage, const char *name, const unsigned char key[32], unsigned char *out,
		      size_t capacity, struct state_copy *c)
{
	unsigned short path[STAGE_PATH_CHARS];
	unsigned char *file = NULL;
	size_t bytes = 0;

	c->result = TPM_STATEFILE_NONE;
	c->sequence = 0;
	c->bytes = 0;
	c->read = stage_sibling(stage, name, path, STAGE_PATH_CHARS)
			  ? stage_read_file(stage, path, TPM_STATEFILE_MAX_BYTES, &file, &bytes)
			  : STAGE_READ_ABSENT;
	if (c->read != STAGE_READ_OK) return;
	c->result = tpm_statefile_read(file, bytes, key, tpm_engine_gcm, out, capacity, &c->bytes, &c->sequence);
	stage_free(stage, file);
}

/* The permanent state to start from, into tpm.state. Returns 1 with *bytes 0 for a new TPM (neither copy is
 * there), 1 with the blob, or 0: a copy is there and none opens, so the module offers no TPM. */
static int load_state(struct stage *stage, size_t *bytes)
{
	struct state_copy a, b;
	unsigned char key[32];
	unsigned char *rsmb;
	size_t rsmb_bytes = 0;
	int have_key = 0, chosen;

	*bytes = 0;
	tpm.sequence = 0;
	rsmb = stage_smbios_rsmb(stage, &rsmb_bytes);
	if (rsmb != NULL) {
		have_key = tpm_statefile_board_key(rsmb, rsmb_bytes, efl_sha256, key);
		stage_free(stage, rsmb);
	}
	if (!have_key) memset(key, 0, sizeof(key));	/* no copy opens: absent files still give a new TPM */
	/* A is read into the state buffer, B into its second half: the buffer holds two permanent states. */
	read_copy(stage, "tpm\\state-a.bin", key, tpm.state, STATE_BYTES / 2u, &a);
	read_copy(stage, "tpm\\state-b.bin", key, tpm.state + STATE_BYTES / 2u, STATE_BYTES / 2u, &b);
	memset(key, 0, sizeof(key));
	if (a.read == STAGE_READ_ABSENT && b.read == STAGE_READ_ABSENT) {
		stage_say(stage, TPM_NAME, "no state file: a new TPM is manufactured");
		return 1;
	}
	chosen = tpm_statefile_choose(a.read == STAGE_READ_OK ? a.result : TPM_STATEFILE_NONE, a.sequence,
				      b.read == STAGE_READ_OK ? b.result : TPM_STATEFILE_NONE, b.sequence);
	if (chosen < 0) {
		stage_sayf(stage, TPM_NAME, "state file A is %s, state file B is %s%s: no TPM", copy_text(&a), copy_text(&b),
			   have_key ? "" : ", and the SMBIOS table gives no board key");
		memset(tpm.state, 0, STATE_BYTES);
		return 0;
	}
	if (chosen == 1) memmove(tpm.state, tpm.state + STATE_BYTES / 2u, b.bytes);
	*bytes = chosen == 0 ? a.bytes : b.bytes;
	tpm.sequence = chosen == 0 ? a.sequence : b.sequence;
	memset(tpm.state + *bytes, 0, STATE_BYTES - *bytes);
	stage_sayf(stage, TPM_NAME, "state file %s, sequence %llu, %u bytes: the TPM continues (A is %s, B is %s)",
		   chosen == 0 ? "A" : "B", tpm.sequence, (unsigned)*bytes, copy_text(&a), copy_text(&b));
	return 1;
}

static void stop(struct stage *stage)
{
	if (tpm.installed) {
		stage->bs->UninstallProtocolInterface(tpm.tcg2_handle, &tcg2_guid, &tcg2);
		tpm.installed = 0;
	}
	if (tpm.final_installed) {
		stage->bs->InstallConfigurationTable(&final_events_guid, NULL);
		tpm.final_installed = 0;
	}
	if (tpm.exit_event != NULL) {
		stage->bs->CloseEvent(tpm.exit_event);
		tpm.exit_event = NULL;
	}
	/* The arena holds the seeds, the keys and the OpenSSL state even after a stop: zeros over it before the pages
	 * go back. The engine refuses to start afterwards, which is right, because the module ends here. */
	if (tpm.engine_initialized) tpm_engine_wipe();
	tpm.engine_initialized = 0;
	tpm.engine_running = 0;
	tpm.present = 0;
	free_pages(stage);
	stage->tpm_handoff = 0;
	stage->tpm_handoff_bytes = 0;
}

static void run(struct stage *stage)
{
	struct tpm_engine_platform platform;
	void *firmware = NULL, *arena;
	unsigned char *area, *final;
	size_t permanent_bytes = 0;
	unsigned rc;
	EFI_STATUS status;

	memset(&tpm, 0, sizeof(tpm));
	tpm.stage = stage;
	if (!stage_board_bc250(stage)) {
		stage_say(stage, TPM_NAME, "not a BC-250: no TPM");
		return;
	}
	if (!EFI_ERROR(stage->bs->LocateProtocol(&tcg2_guid, NULL, &firmware)) && firmware != NULL) {
		stage_say(stage, TPM_NAME, "the firmware has a TCG2 protocol: the stage adds none");
		return;
	}
	if (!calibrate(stage) || (!has_rdseed() && !has_rdrand())) {
		stage_say(stage, TPM_NAME, "no invariant time stamp counter or no RDSEED/RDRAND: no TPM");
		return;
	}
	find_volume(stage);

	arena = pages(stage, EfiBootServicesData, ARENA_BYTES, 0);
	area = pages(stage, EfiBootServicesData, LOG_BYTES, 0);
	/* The final events table stays after ExitBootServices (section 6.6), as in edk2. */
	final = pages(stage, EfiACPIMemoryNVS, FINAL_BYTES, 0);
	tpm.state = pages(stage, EfiBootServicesData, STATE_BYTES, 0);
	/* Below 4 GiB: Name (BCHO) gives the address as an AML integer, which is 32 bits in a table of revision 1. */
	tpm.handoff = pages(stage, EfiReservedMemoryType, TPM_HANDOFF_MAX_BYTES, 1);
	if (arena == NULL || area == NULL || final == NULL || tpm.state == NULL || tpm.handoff == NULL ||
	    !tl_log_init(&tpm.log, area, LOG_BYTES, final, FINAL_BYTES)) {
		stop(stage);
		return;
	}

	platform.arena = arena;
	platform.arena_bytes = ARENA_BYTES;
	platform.monotonic_ms = monotonic_ms;
	platform.entropy = entropy;
	platform.ctx = NULL;
	tpm_engine_set_log(engine_log, stage);
	tpm.engine_initialized = tpm_engine_init(&platform);
	if (!tpm.engine_initialized) {
		stage_say(stage, TPM_NAME, "the engine did not start: no TPM");
		stop(stage);
		return;
	}
	/* The state file opens with the cipher of the engine, so it is read after the init. */
	if (!load_state(stage, &permanent_bytes)) {
		stop(stage);
		return;
	}
	rc = tpm_engine_start(permanent_bytes ? tpm.state : NULL, permanent_bytes, NULL, 0, BUFFER_BYTES);
	memset(tpm.state, 0, permanent_bytes);	/* the engine has its own copy */
	if (rc != 0) {
		stage_sayf(stage, TPM_NAME, "the engine did not start, rc 0x%x: no TPM", rc);
		stop(stage);
		return;
	}
	tpm.engine_running = 1;
	tpm.present = 1;
	rc = send(tl_tpm2_startup(tpm.command, sizeof(tpm.command), TL_TPM_SU_CLEAR));
	if (rc != 0) {
		stage_sayf(stage, TPM_NAME, "Startup failed, rc 0x%x: no TPM", rc);
		stop(stage);
		return;
	}
	if (!sha256_only(stage) || !manufacturer(stage)) {
		stop(stage);
		return;
	}
	tpm.capability.max_command = BUFFER_BYTES;
	tpm.capability.max_response = BUFFER_BYTES;
	tpm.measurer.extend = extend;
	tpm.measurer.context = NULL;
	tpm.measurer.log = &tpm.log;
	if (!measure_preboot(stage)) {
		stop(stage);
		return;
	}

	status = stage->bs->CreateEvent(EVT_SIGNAL_EXIT_BOOT_SERVICES, 16 /* TPL_NOTIFY */, exit_boot_services, NULL,
					&tpm.exit_event);
	if (EFI_ERROR(status)) {
		stage_sayf(stage, TPM_NAME, "CreateEvent(ExitBootServices) failed, status 0x%llx: no TPM",
			   (unsigned long long)status);
		tpm.exit_event = NULL;
		stop(stage);
		return;
	}
	status = stage->bs->InstallConfigurationTable(&final_events_guid, final);
	tpm.final_installed = !EFI_ERROR(status);
	if (!EFI_ERROR(status)) status = stage->bs->InstallProtocolInterface(&tpm.tcg2_handle, &tcg2_guid, EFI_NATIVE_INTERFACE, &tcg2);
	if (EFI_ERROR(status)) {
		stage_sayf(stage, TPM_NAME, "the protocol did not install, status 0x%llx: no TPM", (unsigned long long)status);
		stop(stage);
		return;
	}
	tpm.installed = 1;
	stage->tpm_handoff = (unsigned long long)(UINTN)tpm.handoff;
	stage->tpm_handoff_bytes = TPM_HANDOFF_MAX_BYTES;
	stage_sayf(stage, TPM_NAME, "TCG2 protocol installed: %s, SHA-256 bank, %u log bytes, engine arena peak %u KiB",
		   tpm.sequence ? "the stored TPM" : "a new TPM", (unsigned)tpm.log.used,
		   (unsigned)(tpm_engine_arena_peak() / 1024u));
}

/* ---- the gate, the unwind and the probe ------------------------------------------------------------------ */

static enum stage_gate gate(struct stage *stage, const unsigned char *image, size_t bytes)
{
	struct efl_pe pe;
	unsigned char scratch[64];
	if (!tpm.present) return STAGE_GATE_PASS;
	if (image == NULL || !efl_pe_read(image, bytes, &pe)) {
		/* Nothing is measured: PCR 4 then does not describe a start, and a sealed key does not unseal. */
		stage_say(stage, TPM_NAME, "the next image is not a readable PE image: not measured");
		return STAGE_GATE_PASS;
	}
	/* The device path is not part of the digest; the event carries none. */
	if (tl_measure_image(&tpm.measurer, pe.digest, 0, bytes, NULL, 0, scratch, sizeof(scratch)) == TL_NOT_EXTENDED) {
		stage_say(stage, TPM_NAME, "the measurement of the next image failed: not started");
		return STAGE_GATE_DENY;
	}
	stage_sayf(stage, TPM_NAME, "next image measured into PCR 4, %u bytes", (unsigned)bytes);
	return STAGE_GATE_BIND;
}

/* The boot application returned or did not start: the stage returns to the firmware, which must not keep a
 * protocol whose code goes away with the stage. */
static void unwind(struct stage *stage)
{
	static const char returning[] = "Returning from EFI Application from Boot Option";
	if (tpm.present) tl_measure_data(&tpm.measurer, 4, TL_EV_EFI_ACTION, returning, sizeof(returning) - 1u, returning,
					 sizeof(returning) - 1u);
	stop(stage);
}

static void probe(struct stage *stage)
{
	void *firmware = NULL;
	int has = !EFI_ERROR(stage->bs->LocateProtocol(&tcg2_guid, NULL, &firmware)) && firmware != NULL;
	stage_sayf(stage, TPM_NAME, "firmware TCG2 protocol: %s; RDSEED %s, RDRAND %s", has ? "yes" : "no",
		   has_rdseed() ? "yes" : "no", has_rdrand() ? "yes" : "no");
}

const struct stage_module tpm_module = { TPM_NAME, "tpm", 1, run, probe, gate, NULL, unwind };
