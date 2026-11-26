/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * A model of tpm_engine.h for the host tests of the TPM module binding (stage/test/firmware_test.c). It is not a TPM.
 * It answers the commands that tpm_module.c sends itself, in the byte layout of TPM 2.0 Part 3, with real SHA-256
 * extends, so that a test can replay the event log of the module against PCR_Read:
 * - Startup, Shutdown, PCR_Extend (SHA-256 bank), PCR_Read (SHA-256, up to 8 PCRs), PCR_Allocate,
 *   GetCapability(PCRS) and GetCapability(TPM_PROPERTIES, TPM_PT_MANUFACTURER).
 * - tpm_engine_gcm: an authenticated cipher in the contract of AES-256-GCM (a SHA-256 keystream and a SHA-256 tag),
 *   so that a test can write state files that the module opens, and damage them. The real GCM has its known-answer
 *   test in the engine test.
 * A new model TPM has two banks, SHA-1 and SHA-256, as libtpms has, so that the module must allocate. The
 * allocation takes effect at the next start, as in TPM 2.0 Part 3, section 22.5.
 */
#include <string.h>
#include "efl_sha256.h"
#include "tpm_engine.h"
#include "engine_model.h"

#define PCRS		24u
#define DIGEST		32u
#define ST_NO_SESSIONS	0x8001u
#define ST_SESSIONS	0x8002u
#define RC_INITIALIZE	0x100u
#define RC_FAILURE	0x101u
#define RC_COMMAND_CODE	0x143u
#define PERMANENT_MAGIC	"MODELTPM"

struct engine_model_state engine_model;

static unsigned char pcr[PCRS][DIGEST];
static int running, started, sha256_only, pending_sha256_only, initialized;

static unsigned long get(const unsigned char *p, unsigned n)
{
	unsigned long v = 0;
	unsigned i;
	for (i = 0; i < n; i++) v = (v << 8) | p[i];
	return v;
}

static void put(unsigned char *p, unsigned long v, unsigned n)
{
	unsigned i;
	for (i = 0; i < n; i++) p[i] = (unsigned char)(v >> (8u * (n - 1u - i)));
}

/* The header of a response of n bytes. */
static size_t answer(unsigned char *rsp, unsigned tag, size_t n, unsigned rc)
{
	put(rsp, tag, 2);
	put(rsp + 2, (unsigned long)n, 4);
	put(rsp + 6, rc, 4);
	return n;
}

int tpm_engine_init(const struct tpm_engine_platform *p)
{
	unsigned char probe[16];
	engine_model.inits++;
	if (engine_model.fail_init || p == NULL || p->arena == NULL || p->arena_bytes == 0 || p->monotonic_ms == NULL ||
	    p->entropy == NULL)
		return 0;
	/* The platform functions must work: the module gives real ones. */
	if (!p->entropy(p->ctx, probe, sizeof(probe))) return 0;
	(void)p->monotonic_ms(p->ctx);
	initialized = 1;
	return 1;
}

unsigned tpm_engine_start(const unsigned char *permanent, size_t permanent_bytes, const unsigned char *volatile_state,
			  size_t volatile_bytes, unsigned max_buffer)
{
	(void)volatile_state;
	(void)volatile_bytes;
	engine_model.starts++;
	if (running || max_buffer < 1024u) return RC_FAILURE;
	engine_model.from_state = permanent != NULL;
	if (permanent == NULL) {
		sha256_only = engine_model.new_sha256_only;
	} else {
		if (permanent_bytes != sizeof(PERMANENT_MAGIC) || memcmp(permanent, PERMANENT_MAGIC, 8)) return RC_FAILURE;
		sha256_only = permanent[8];
	}
	pending_sha256_only = sha256_only;
	running = 1;
	started = 0;
	return 0;
}

static size_t get_pcrs(unsigned char *rsp)
{
	size_t at = 11;
	rsp[10] = 0;				/* moreData */
	put(rsp + at, 5, 4);			/* TPM_CAP_PCRS */
	put(rsp + at + 4, sha256_only ? 1u : 2u, 4);
	at += 8;
	if (!sha256_only) {
		put(rsp + at, 0x0004u, 2);	/* SHA-1, all 24 PCRs */
		rsp[at + 2] = 3;
		memset(rsp + at + 3, 0xFF, 3);
		at += 6;
	}
	put(rsp + at, 0x000Bu, 2);
	rsp[at + 2] = 3;
	memset(rsp + at + 3, 0xFF, 3);
	at += 6;
	return answer(rsp, ST_NO_SESSIONS, at, 0);
}

static size_t pcr_read(const unsigned char *cmd, size_t n, unsigned char *rsp)
{
	unsigned char select[3];
	size_t at;
	unsigned i, count = 0;
	if (n < 20 || get(cmd + 10, 4) != 1 || get(cmd + 14, 2) != 0x000Bu || cmd[16] != 3) return answer(rsp, ST_NO_SESSIONS, 10, RC_FAILURE);
	memcpy(select, cmd + 17, 3);
	/* At most 8 digests, the lowest PCRs first; the selection of the answer names those. */
	for (i = 0; i < PCRS; i++)
		if (select[i / 8u] & (1u << (i % 8u))) {
			if (count == 8u) select[i / 8u] &= (unsigned char)~(1u << (i % 8u));
			else count++;
		}
	put(rsp + 10, engine_model.extends, 4);	/* pcrUpdateCounter */
	put(rsp + 14, 1, 4);
	put(rsp + 18, 0x000Bu, 2);
	rsp[20] = 3;
	memcpy(rsp + 21, select, 3);
	put(rsp + 24, count, 4);
	at = 28;
	for (i = 0; i < PCRS; i++)
		if (select[i / 8u] & (1u << (i % 8u))) {
			put(rsp + at, DIGEST, 2);
			memcpy(rsp + at + 2, pcr[i], DIGEST);
			at += 2u + DIGEST;
		}
	return answer(rsp, ST_NO_SESSIONS, at, 0);
}

static size_t pcr_extend(const unsigned char *cmd, size_t n, unsigned char *rsp)
{
	struct efl_sha256 s;
	unsigned index;
	size_t auth;
	if (n < 18) return answer(rsp, ST_SESSIONS, 10, RC_FAILURE);
	index = (unsigned)get(cmd + 10, 4);
	auth = get(cmd + 14, 4);
	if (index >= PCRS || n != 18u + auth + 4u + 2u + DIGEST || get(cmd + 18 + auth, 4) != 1 ||
	    get(cmd + 22 + auth, 2) != 0x000Bu)
		return answer(rsp, ST_SESSIONS, 10, RC_FAILURE);
	efl_sha256_init(&s);
	efl_sha256_update(&s, pcr[index], DIGEST);
	efl_sha256_update(&s, cmd + 24 + auth, DIGEST);
	efl_sha256_final(&s, pcr[index]);
	engine_model.extends++;
	/* parameterSize 0, then the response session: nonce (empty), attributes, hmac (empty). */
	put(rsp + 10, 0, 4);
	memset(rsp + 14, 0, 5);
	return answer(rsp, ST_SESSIONS, 19, 0);
}

size_t tpm_engine_process(const unsigned char *cmd, size_t n, unsigned char *rsp, size_t cap)
{
	unsigned long code;
	unsigned i;
	if (!running || cmd == NULL || n < 10 || get(cmd + 2, 4) != n || cap < 512u) return 0;
	engine_model.commands++;
	code = get(cmd + 6, 4);
	if (code != 0x144u && !started) return answer(rsp, ST_NO_SESSIONS, 10, RC_INITIALIZE);
	switch (code) {
	case 0x144u:	/* Startup */
		if (started) return answer(rsp, ST_NO_SESSIONS, 10, RC_INITIALIZE);
		started = 1;
		memset(pcr, 0, sizeof(pcr));
		for (i = 17; i <= 22u; i++) memset(pcr[i], 0xFF, DIGEST);
		return answer(rsp, ST_NO_SESSIONS, 10, 0);
	case 0x145u:	/* Shutdown */
		return answer(rsp, ST_NO_SESSIONS, 10, 0);
	case 0x182u:
		return pcr_extend(cmd, n, rsp);
	case 0x17Eu:
		return pcr_read(cmd, n, rsp);
	case 0x12Bu:	/* PCR_Allocate: the model checks the bank list names only its own banks */
		if (n < 31 || get(cmd + 10, 4) != 0x4000000Cu) return answer(rsp, ST_SESSIONS, 10, RC_FAILURE);
		for (i = 0; i < get(cmd + 27, 4); i++) {
			unsigned long alg = get(cmd + 31 + 6u * i, 2);
			if (alg != 0x000Bu && (alg != 0x0004u || sha256_only)) return answer(rsp, ST_SESSIONS, 10, 0x2C3u);
		}
		pending_sha256_only = 1;
		put(rsp + 10, 13, 4);
		rsp[14] = 1;			/* allocationSuccess */
		memset(rsp + 15, 0, 12);
		memset(rsp + 27, 0, 5);
		return answer(rsp, ST_SESSIONS, 32, 0);
	case 0x17Au:	/* GetCapability */
		if (n != 22) return answer(rsp, ST_NO_SESSIONS, 10, RC_FAILURE);
		if (get(cmd + 10, 4) == 5) return get_pcrs(rsp);
		if (get(cmd + 10, 4) == 6 && get(cmd + 14, 4) == 0x105u) {
			rsp[10] = 0;
			put(rsp + 11, 6, 4);
			put(rsp + 15, 1, 4);
			put(rsp + 19, 0x105u, 4);
			memcpy(rsp + 23, "BCMD", 4);
			return answer(rsp, ST_NO_SESSIONS, 27, 0);
		}
		return answer(rsp, ST_NO_SESSIONS, 10, RC_FAILURE);
	default:
		return answer(rsp, ST_NO_SESSIONS, 10, RC_COMMAND_CODE);
	}
}

size_t tpm_engine_get_state(int kind, unsigned char *out, size_t cap)
{
	if (!running) return 0;
	if (kind == TPM_ENGINE_PERMANENT) {
		if (cap < sizeof(PERMANENT_MAGIC)) return 0;
		memcpy(out, PERMANENT_MAGIC, 8);
		out[8] = (unsigned char)pending_sha256_only;
		return sizeof(PERMANENT_MAGIC);
	}
	if (kind == TPM_ENGINE_VOLATILE) {
		if (cap < sizeof(pcr)) return 0;
		memcpy(out, pcr, sizeof(pcr));
		return sizeof(pcr);
	}
	return 0;
}

int tpm_engine_permanent_dirty(void)
{
	return 1;
}

void tpm_engine_stop(void)
{
	engine_model.stops += running;
	running = 0;
	started = 0;
}

size_t tpm_engine_arena_peak(void)
{
	return 4096u;
}

void tpm_engine_wipe(void)
{
	tpm_engine_stop();
	initialized = 0;
	engine_model.wipes++;
}

void tpm_engine_set_log(void (*sink)(void *ctx, const char *text, size_t bytes), void *ctx)
{
	if (sink != NULL) sink(ctx, "model engine\n", 13);	/* the binding must take a piece with a line end */
}

void engine_model_reset(void)
{
	memset(&engine_model, 0, sizeof(engine_model));
	running = 0;
	started = 0;
	initialized = 0;
}

int engine_model_running(void)
{
	return running;
}

int tpm_engine_gcm(int seal, const unsigned char key[32], const unsigned char nonce[12], const unsigned char *aad,
		   size_t aad_bytes, const unsigned char *in, size_t n, unsigned char *out, unsigned char tag[16])
{
	unsigned char t[32], block[32], ctr[44];
	struct efl_sha256 s;
	size_t i;

	engine_model.gcm_calls++;
	if (!initialized) return 0;	/* as the engine: the cipher lives in the arena */
	efl_sha256_init(&s);
	efl_sha256_update(&s, key, 32);
	efl_sha256_update(&s, nonce, 12);
	efl_sha256_update(&s, aad, aad_bytes);
	if (!seal) {
		efl_sha256_update(&s, in, n);
		efl_sha256_final(&s, t);
		if (memcmp(t, tag, 16) != 0) return 0;
	}
	for (i = 0; i < n; i++) {
		if (i % 32 == 0) {
			memcpy(ctr, key, 32);
			memcpy(ctr + 32, nonce, 12);
			ctr[31] ^= (unsigned char)(i / 32);
			efl_sha256(ctr, sizeof(ctr), block);
		}
		out[i] = (unsigned char)(in[i] ^ block[i % 32]);
	}
	if (seal) {
		efl_sha256_update(&s, out, n);
		efl_sha256_final(&s, t);
		memcpy(tag, t, 16);
	}
	return 1;
}

void engine_model_init_cipher(void)
{
	initialized = 1;
}