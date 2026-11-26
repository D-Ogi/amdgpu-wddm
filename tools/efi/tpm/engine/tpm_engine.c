/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The TPM engine (tpm_engine.h): libtpms with its NV state in arena memory.
 *
 * The NV callbacks of libtpms keep the permanent blob ("permall") in the arena, never in a file:
 * - nv_load gives a copy of the blob of the engine, or TPM_RETRY when there is none (libtpms then manufactures).
 * - nv_store replaces that blob and sets the dirty flag when the new blob differs.
 * The other names of libtpms ("volatilestate", "savestate") have no store here: the volatile state goes through
 * TPMLIB_GetState and TPMLIB_SetState only.
 *
 * The contract of TPMLIB_SetState (libtpms 165c303, src/tpm_tpm2_interface.c and src/tpm_library.c):
 * - It works only while the TPM is off (before TPMLIB_MainInit or after TPMLIB_Terminate), and after
 *   TPMLIB_ChooseTPMVersion: a change of the version choice clears the cached blobs.
 * - It checks the blob (it unmarshals it) and caches it. TPMLIB_MainInit takes the cached blobs: a cached permanent
 *   blob stops the manufacture, and _TPM_Init loads the cached volatile blob and restores the NV shadow, so that the
 *   TPM continues without TPM2_Startup.
 * - The volatile blob needs the permanent state first: SetState(VOLATILE) reads it with TPMLIB_GetState, which takes
 *   the cached permanent blob (or the nv_load callback). So the permanent blob goes first.
 * - A volatile blob that fails its check (SHA-1 checksum, TPM_RC_HASH) clears all cached blobs and sets the failure
 *   mode; TPMLIB_MainInit clears the failure mode again at its start.
 * - With a cached permanent blob, TPMLIB_MainInit ends with NvCommit, which calls nv_store once.
 *
 * Every call into libtpms or OpenSSL runs under a guard of the C runtime: abort() or a failed assertion inside them
 * returns here, and the engine is then out of service until the next image (a failure in the middle of a command
 * leaves libtpms in a state that nothing can trust).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include "libtpms/tpm_error.h"
#include "libtpms/tpm_library.h"
#include "libtpms/tpm_nvfilename.h"
#include "crt.h"
#include "tpm_engine.h"
#include "tpm_engine_internal.h"
#include "gcm_openssl.h"

/* libtpms 165c303, src/tpm_library.c: the clear of the blobs that TPMLIB_SetState cached. TPMLIB_SetState with NULL
 * does not clear; it caches an empty state, which TPMLIB_MainInit would take as an empty NV image. */
void ClearAllCachedState(void);

static struct tpm_engine_platform g_platform;
static int g_init_called;
static int g_initialized;
static int g_running;
static int g_broken;		/* a failure inside libtpms or OpenSSL: no further calls */
static int g_wiped;
static int g_dirty;
static unsigned char *g_permanent;	/* the newest permanent blob of libtpms, in the arena */
static size_t g_permanent_bytes;
static unsigned char *g_command;	/* a copy of the command: TPMLIB_Process takes a non-const buffer */
static size_t g_command_cap;
static unsigned char *g_response;	/* the response buffer of TPMLIB_Process, kept between commands */
static uint32_t g_response_cap;

int tpm_engine_entropy(unsigned char *out, size_t n)
{
	return g_platform.entropy && g_platform.entropy(g_platform.ctx, out, n) == 1;
}

static void free_permanent(void)
{
	free(g_permanent);
	g_permanent = NULL;
	g_permanent_bytes = 0;
}

static TPM_RESULT nv_init(void) { return TPM_SUCCESS; }

/* libtpms frees *data with free(): the copy comes from the same arena. */
static TPM_RESULT nv_load(unsigned char **data, uint32_t *length, uint32_t tpm_number, const char *name)
{
	(void)tpm_number;
	*data = NULL;
	*length = 0;
	if (strcmp(name, TPM_PERMANENT_ALL_NAME) != 0 || !g_permanent) return TPM_RETRY;
	*data = malloc(g_permanent_bytes);
	if (!*data) return TPM_FAIL;
	memcpy(*data, g_permanent, g_permanent_bytes);
	*length = (uint32_t)g_permanent_bytes;
	return TPM_SUCCESS;
}

static TPM_RESULT nv_store(const unsigned char *data, uint32_t length, uint32_t tpm_number, const char *name)
{
	unsigned char *copy;
	(void)tpm_number;
	if (strcmp(name, TPM_PERMANENT_ALL_NAME) != 0) return TPM_SUCCESS;
	if (g_permanent && g_permanent_bytes == length && memcmp(g_permanent, data, length) == 0) return TPM_SUCCESS;
	copy = malloc(length ? length : 1);
	if (!copy) return TPM_FAIL;
	memcpy(copy, data, length);
	free_permanent();
	g_permanent = copy;
	g_permanent_bytes = length;
	g_dirty = 1;
	return TPM_SUCCESS;
}

static TPM_RESULT nv_delete(uint32_t tpm_number, const char *name, TPM_BOOL must_exist)
{
	(void)tpm_number;
	if (strcmp(name, TPM_PERMANENT_ALL_NAME) != 0) return TPM_SUCCESS;
	if (!g_permanent) return must_exist ? TPM_FAIL : TPM_SUCCESS;
	free_permanent();
	g_dirty = 1;
	return TPM_SUCCESS;
}

static TPM_RESULT io_init(void) { return TPM_SUCCESS; }

/* The TCG2 protocol and the CRB page of Windows are locality 0. */
static TPM_RESULT io_locality(TPM_MODIFIER_INDICATOR *locality, uint32_t tpm_number)
{
	(void)tpm_number;
	*locality = 0;
	return TPM_SUCCESS;
}

/* The platform has no physical presence signal. */
static TPM_RESULT io_presence(TPM_BOOL *present, uint32_t tpm_number)
{
	(void)tpm_number;
	*present = FALSE;
	return TPM_SUCCESS;
}

static struct libtpms_callbacks g_callbacks = {
	sizeof(struct libtpms_callbacks), nv_init, nv_load, nv_store, nv_delete, io_init, io_locality, io_presence,
};

/* A failure under the guard: the engine is out of service. */
static void broken(crt_jmp_buf *jb)
{
	crt_guard_leave(jb);
	g_broken = 1;
	g_running = 0;
}

static int usable(void) { return g_initialized && !g_broken && !g_wiped; }

int tpm_engine_init(const struct tpm_engine_platform *p)
{
	crt_jmp_buf jb;
	unsigned char check[32];
	int ok;

	if (g_init_called || !p || !p->arena || !p->monotonic_ms || !p->entropy) return 0;
	if (!crt_init(p->arena, p->arena_bytes, p->monotonic_ms, p->ctx)) return 0;
	/* OpenSSL keeps its global state in this arena from the first call on: no second start in another arena */
	g_init_called = 1;
	g_platform = *p;
	if (crt_setjmp(&jb)) {
		broken(&jb);
		return 0;
	}
	crt_guard_enter(&jb);
	ok = OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG | OPENSSL_INIT_NO_ATEXIT, NULL) == 1 &&
	     RAND_bytes(check, sizeof(check)) == 1;
	crt_guard_leave(&jb);
	OPENSSL_cleanse(check, sizeof(check));
	if (!ok) {
		g_broken = 1;
		return 0;
	}
	g_initialized = 1;
	return 1;
}

int tpm_engine_gcm(int seal, const unsigned char key[32], const unsigned char nonce[12], const unsigned char *aad,
		   size_t aad_bytes, const unsigned char *in, size_t n, unsigned char *out, unsigned char tag[16])
{
	crt_jmp_buf jb;
	int ok;

	if (!usable()) return 0;
	/* OpenSSL allocates the cipher context from the arena: a failure there ends under the guard. */
	if (crt_setjmp(&jb)) {
		broken(&jb);
		return 0;
	}
	crt_guard_enter(&jb);
	ok = tpm_gcm_openssl(seal, key, nonce, aad, aad_bytes, in, n, out, tag);
	crt_guard_leave(&jb);
	return ok;
}

/* A start that did not work, still under its guard. */
static unsigned start_failed(crt_jmp_buf *jb, unsigned code)
{
	free_permanent();
	g_dirty = 0;
	crt_guard_leave(jb);
	return code;
}

unsigned tpm_engine_start(const unsigned char *permanent, size_t permanent_bytes,
			  const unsigned char *volatile_state, size_t volatile_bytes, unsigned max_buffer)
{
	crt_jmp_buf jb;
	TPM_RESULT r;
	uint32_t min_size = 0, max_size = 0, wanted;

	if (!usable() || g_running) return TPM_ENGINE_FAIL;
	if ((permanent && !permanent_bytes) || (!permanent && permanent_bytes) || (volatile_state && !volatile_bytes) ||
	    (!volatile_state && volatile_bytes) || (volatile_state && !permanent) || permanent_bytes > UINT32_MAX ||
	    volatile_bytes > UINT32_MAX)
		return TPM_ENGINE_BAD_PARAMETER;
	if (crt_setjmp(&jb)) {
		/* the arena may be damaged: forget the blob, do not free it */
		broken(&jb);
		g_permanent = NULL;
		g_permanent_bytes = 0;
		return TPM_ENGINE_FAIL;
	}
	crt_guard_enter(&jb);
	g_dirty = 0;
	if (permanent) {
		g_permanent = malloc(permanent_bytes);
		if (!g_permanent) return start_failed(&jb, TPM_ENGINE_FAIL);
		memcpy(g_permanent, permanent, permanent_bytes);
		g_permanent_bytes = permanent_bytes;
	}
	/* The version choice first: a change of it clears the cached state blobs. */
	r = TPMLIB_ChooseTPMVersion(TPMLIB_TPM_VERSION_2);
	if (r == TPM_SUCCESS) r = TPMLIB_RegisterCallbacks(&g_callbacks);
	if (r == TPM_SUCCESS) {
		(void)TPMLIB_SetBufferSize(0, &min_size, &max_size);
		wanted = max_buffer ? max_buffer : max_size;
		if (wanted < min_size || wanted > max_size || TPMLIB_SetBufferSize(wanted, NULL, NULL) != wanted)
			r = TPM_ENGINE_SIZE;
	}
	if (r == TPM_SUCCESS && permanent) r = TPMLIB_SetState(TPMLIB_STATE_PERMANENT, permanent, (uint32_t)permanent_bytes);
	if (r == TPM_SUCCESS && volatile_state)
		r = TPMLIB_SetState(TPMLIB_STATE_VOLATILE, volatile_state, (uint32_t)volatile_bytes);
	if (r == TPM_SUCCESS) {
		r = TPMLIB_MainInit();
		if (r != TPM_SUCCESS) TPMLIB_Terminate();
	}
	if (r != TPM_SUCCESS) {
		/* No cached blob may stay for the next start. */
		ClearAllCachedState();
		return start_failed(&jb, r);
	}
	crt_guard_leave(&jb);
	g_running = 1;
	return TPM_ENGINE_OK;
}

size_t tpm_engine_process(const unsigned char *cmd, size_t n, unsigned char *rsp, size_t cap)
{
	crt_jmp_buf jb;
	uint32_t out_bytes = 0;
	size_t result = 0;

	if (!usable() || !g_running || !cmd || !rsp || n < 10 || n > UINT32_MAX) return 0;
	if (crt_setjmp(&jb)) {
		broken(&jb);
		return 0;
	}
	crt_guard_enter(&jb);
	if (n > g_command_cap) {
		unsigned char *grown = realloc(g_command, n);
		if (!grown) {
			crt_guard_leave(&jb);
			return 0;
		}
		g_command = grown;
		g_command_cap = n;
	}
	memcpy(g_command, cmd, n);
	if (TPMLIB_Process(&g_response, &out_bytes, &g_response_cap, g_command, (uint32_t)n) == TPM_SUCCESS &&
	    g_response && out_bytes >= 10 && out_bytes <= cap) {
		memcpy(rsp, g_response, out_bytes);
		result = out_bytes;
	}
	crt_guard_leave(&jb);
	return result;
}

size_t tpm_engine_get_state(int kind, unsigned char *out, size_t cap)
{
	crt_jmp_buf jb;
	unsigned char *blob = NULL;
	uint32_t bytes = 0;
	TPM_RESULT r;
	size_t result = 0;

	if (!usable() || !g_running || (kind != TPM_ENGINE_PERMANENT && kind != TPM_ENGINE_VOLATILE)) return 0;
	if (!out && cap) return 0;
	if (crt_setjmp(&jb)) {
		broken(&jb);
		return 0;
	}
	crt_guard_enter(&jb);
	r = TPMLIB_GetState(kind == TPM_ENGINE_PERMANENT ? TPMLIB_STATE_PERMANENT : TPMLIB_STATE_VOLATILE, &blob, &bytes);
	if (r == TPM_SUCCESS && blob && bytes) {
		if (!out) {
			result = bytes;
		} else if (bytes <= cap) {
			memcpy(out, blob, bytes);
			result = bytes;
			if (kind == TPM_ENGINE_PERMANENT) {
				/* the caller has this blob now: it is the base of the next comparison in nv_store */
				unsigned char *fitted = realloc(blob, bytes);
				free_permanent();
				g_permanent = fitted ? fitted : blob;
				g_permanent_bytes = bytes;
				blob = NULL;
				g_dirty = 0;
			}
		}
	}
	if (blob) {
		OPENSSL_cleanse(blob, bytes);
		free(blob);
	}
	crt_guard_leave(&jb);
	return result;
}

int tpm_engine_permanent_dirty(void) { return g_dirty; }

void tpm_engine_stop(void)
{
	crt_jmp_buf jb;

	if (!g_initialized || g_wiped || !g_running) return;
	g_running = 0;
	if (crt_setjmp(&jb)) {
		broken(&jb);
		return;
	}
	crt_guard_enter(&jb);
	TPMLIB_Terminate();
	if (g_response) OPENSSL_cleanse(g_response, g_response_cap);
	free(g_response);
	g_response = NULL;
	g_response_cap = 0;
	if (g_command) OPENSSL_cleanse(g_command, g_command_cap);
	free(g_command);
	g_command = NULL;
	g_command_cap = 0;
	if (g_permanent) OPENSSL_cleanse(g_permanent, g_permanent_bytes);
	free_permanent();
	crt_guard_leave(&jb);
}

size_t tpm_engine_arena_peak(void)
{
	struct crt_arena_stats stats;
	crt_arena_stats(&stats);
	return stats.high_water;
}

void tpm_engine_wipe(void)
{
	if (!g_initialized || g_wiped) return;
	tpm_engine_stop();
	g_response = NULL;
	g_command = NULL;
	g_permanent = NULL;
	crt_arena_wipe();
	g_wiped = 1;
	g_running = 0;
}

void tpm_engine_set_log(void (*sink)(void *ctx, const char *text, size_t bytes), void *ctx)
{
	crt_set_log(sink, ctx);
}
