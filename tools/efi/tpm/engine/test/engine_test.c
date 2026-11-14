/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The host test of the TPM engine (design: docs/design/tpm-preos.md, "The test plan"). It drives the objects of
 * tpm_engine.lib through the life of the pre-OS TPM: a manufacture, a sealed secret, a PCR extend, the two state
 * blobs of the handoff, a resume without TPM2_Startup that still unseals and still has the PCR, a refused damaged
 * volatile blob, and a TPM reset that clears the PCR.
 *
 * The sealed object after the resume: the test loads it twice. The handle that was loaded before the handoff is
 * still there, because loaded objects are part of the volatile state. And a new TPM2_CreatePrimary with the same
 * template gives the same storage key, because a primary key is a function of the hierarchy seed (in the permanent
 * state) and the template (TPM 2.0 Library Part 1, "Primary Keys"); the test checks that the public area is the same
 * byte for byte and loads the stored private part under it. So no TPM2_EvictControl is necessary, and the test does
 * not add a persistent object to the NV state.
 *
 * --mutate=NAME breaks one check on purpose; build.ps1 runs each mutation and requires a failure:
 *   pcr-after-resume  extends PCR 4 once more before the state is taken, so the PCR after the resume differs
 *   volatile-intact   gives the volatile blob without damage to the negative test, so the refusal does not come
 *   gcm-tag           gives the open of the GCM test case a changed tag, so the open refuses where it must succeed
 */
#include <stdio.h>
#include <string.h>
#include "test_host.h"
#include "tpm_cmd.h"

static const char *g_mutation;
static int g_failures;
static unsigned char g_rsp[TPM_CMD_MAX];
static unsigned char g_permanent[512 * 1024];
static unsigned char g_volatile[256 * 1024];
static unsigned char g_damaged[256 * 1024];

/* The digest that the test extends into PCR 4, and the PCR value that follows from a zero PCR:
 * SHA-256(32 zero bytes || digest), computed outside this project (Python hashlib). */
static const unsigned char g_extend[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
					   17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
static const unsigned char g_expected_pcr4[32] = {
	0x0b, 0x8f, 0x4c, 0x5b, 0x6a, 0xdc, 0x4c, 0x08, 0x7a, 0xb9, 0xf4, 0x3a, 0xae, 0xb6, 0x00, 0x70,
	0x84, 0xc2, 0x64, 0xad, 0xca, 0xa3, 0xcb, 0x07, 0x17, 0x6b, 0x79, 0x23, 0x42, 0x85, 0x04, 0x12};
static const unsigned char g_secret[] = "BC-250 pre-OS TPM: a secret sealed before the handoff";

static int mutated(const char *name) { return g_mutation && strcmp(g_mutation, name) == 0; }

static void check(int ok, const char *what)
{
	if (ok) {
		host_print("  ok   %s", what);
	} else {
		host_print("  FAIL %s", what);
		g_failures++;
	}
}

/* ---- the steps and their measurements */

static unsigned long long g_step_start;

static void step_begin(const char *name)
{
	host_print("%s", name);
	host_stack_paint();
	g_step_start = host_us();
}

static void step_end(void)
{
	unsigned long long us = host_us() - g_step_start;
	host_print("  time %llu.%03llu ms, stack %zu KiB, arena peak %zu KiB", us / 1000, us % 1000,
		   (host_stack_used() + 1023) / 1024, (tpm_engine_arena_peak() + 1023) / 1024);
}

/* ---- the TPM commands */

static size_t run(struct tpm_cmd *c)
{
	size_t n = tpm_cmd_end(c);
	return n ? tpm_engine_process(c->b, n, g_rsp, sizeof(g_rsp)) : 0;
}

static unsigned startup_clear(void)
{
	struct tpm_cmd c;
	struct tpm_rsp r;
	size_t n;
	tpm_cmd_begin(&c, TPM_ST_NO_SESSIONS, TPM_CC_Startup);
	tpm_cmd_u16(&c, TPM_SU_CLEAR);
	n = run(&c);
	return tpm_rsp_begin(&r, g_rsp, n);
}

static unsigned get_random(unsigned char *out, size_t want)
{
	struct tpm_cmd c;
	struct tpm_rsp r;
	const unsigned char *bytes;
	size_t n, got;
	unsigned rc;
	tpm_cmd_begin(&c, TPM_ST_NO_SESSIONS, TPM_CC_GetRandom);
	tpm_cmd_u16(&c, (unsigned)want);
	n = run(&c);
	rc = tpm_rsp_begin(&r, g_rsp, n);
	if (rc) return rc;
	bytes = tpm_rsp_b2b(&r, &got, NULL);
	if (!bytes || got != want) return 0xFFFFFFFEu;
	memcpy(out, bytes, got);
	return 0;
}

/* An RSA 2048 storage key in the owner hierarchy: restricted, decrypt, AES-128-CFB for its children. */
static unsigned create_primary(unsigned *handle, unsigned char *public_area, size_t *public_bytes)
{
	struct tpm_cmd c;
	struct tpm_rsp r;
	const unsigned char *raw;
	size_t n, bytes;
	unsigned rc;

	tpm_cmd_begin(&c, TPM_ST_SESSIONS, TPM_CC_CreatePrimary);
	tpm_cmd_u32(&c, TPM_RH_OWNER);
	tpm_cmd_password_session(&c);
	tpm_cmd_u16(&c, 4);		/* inSensitive: an empty userAuth and empty data */
	tpm_cmd_u16(&c, 0);
	tpm_cmd_u16(&c, 0);
	tpm_cmd_u16(&c, 26);		/* inPublic */
	tpm_cmd_u16(&c, 0x0001);	/* TPM_ALG_RSA */
	tpm_cmd_u16(&c, 0x000B);	/* nameAlg SHA-256 */
	/* fixedTPM, fixedParent, sensitiveDataOrigin, userWithAuth, noDA, restricted, decrypt */
	tpm_cmd_u32(&c, 0x00030472);
	tpm_cmd_u16(&c, 0);		/* authPolicy */
	tpm_cmd_u16(&c, 0x0006);	/* symmetric AES */
	tpm_cmd_u16(&c, 128);
	tpm_cmd_u16(&c, 0x0043);	/* CFB */
	tpm_cmd_u16(&c, 0x0010);	/* scheme TPM_ALG_NULL */
	tpm_cmd_u16(&c, 2048);
	tpm_cmd_u32(&c, 0);		/* the default exponent */
	tpm_cmd_u16(&c, 0);		/* unique */
	tpm_cmd_u16(&c, 0);		/* outsideInfo */
	tpm_cmd_u32(&c, 0);		/* creationPCR: no selection */
	n = run(&c);
	rc = tpm_rsp_begin(&r, g_rsp, n);
	if (rc) return rc;
	*handle = tpm_rsp_u32(&r);
	(void)tpm_rsp_u32(&r);		/* parameterSize */
	if (!tpm_rsp_b2b(&r, &bytes, &raw) || bytes + 2 > *public_bytes) return 0xFFFFFFFEu;
	memcpy(public_area, raw, bytes + 2);
	*public_bytes = bytes + 2;
	return 0;
}

/* A keyed-hash object with the secret as its data and an empty authorization: the sealed secret. */
static unsigned create_sealed(unsigned parent, unsigned char *private_area, size_t *private_bytes,
			      unsigned char *public_area, size_t *public_bytes)
{
	struct tpm_cmd c;
	struct tpm_rsp r;
	const unsigned char *raw;
	size_t n, bytes;
	unsigned rc;

	tpm_cmd_begin(&c, TPM_ST_SESSIONS, TPM_CC_Create);
	tpm_cmd_u32(&c, parent);
	tpm_cmd_password_session(&c);
	tpm_cmd_u16(&c, (unsigned)(4 + sizeof(g_secret)));	/* inSensitive */
	tpm_cmd_u16(&c, 0);
	tpm_cmd_b2b(&c, g_secret, sizeof(g_secret));
	tpm_cmd_u16(&c, 14);		/* inPublic */
	tpm_cmd_u16(&c, 0x0008);	/* TPM_ALG_KEYEDHASH */
	tpm_cmd_u16(&c, 0x000B);
	tpm_cmd_u32(&c, 0x00000452);	/* fixedTPM, fixedParent, userWithAuth, noDA */
	tpm_cmd_u16(&c, 0);
	tpm_cmd_u16(&c, 0x0010);	/* scheme TPM_ALG_NULL */
	tpm_cmd_u16(&c, 0);		/* unique */
	tpm_cmd_u16(&c, 0);
	tpm_cmd_u32(&c, 0);
	n = run(&c);
	rc = tpm_rsp_begin(&r, g_rsp, n);
	if (rc) return rc;
	(void)tpm_rsp_u32(&r);
	if (!tpm_rsp_b2b(&r, &bytes, &raw) || bytes + 2 > *private_bytes) return 0xFFFFFFFEu;
	memcpy(private_area, raw, bytes + 2);
	*private_bytes = bytes + 2;
	if (!tpm_rsp_b2b(&r, &bytes, &raw) || bytes + 2 > *public_bytes) return 0xFFFFFFFEu;
	memcpy(public_area, raw, bytes + 2);
	*public_bytes = bytes + 2;
	return 0;
}

static unsigned load(unsigned parent, const unsigned char *private_area, size_t private_bytes,
		     const unsigned char *public_area, size_t public_bytes, unsigned *handle)
{
	struct tpm_cmd c;
	struct tpm_rsp r;
	size_t n;
	unsigned rc;
	tpm_cmd_begin(&c, TPM_ST_SESSIONS, TPM_CC_Load);
	tpm_cmd_u32(&c, parent);
	tpm_cmd_password_session(&c);
	tpm_cmd_bytes(&c, private_area, private_bytes);
	tpm_cmd_bytes(&c, public_area, public_bytes);
	n = run(&c);
	rc = tpm_rsp_begin(&r, g_rsp, n);
	if (rc) return rc;
	*handle = tpm_rsp_u32(&r);
	return r.bad ? 0xFFFFFFFEu : 0;
}

/* 0 when the object unseals to the secret. */
static unsigned unseal_matches(unsigned handle)
{
	struct tpm_cmd c;
	struct tpm_rsp r;
	const unsigned char *data;
	size_t n, bytes;
	unsigned rc;
	tpm_cmd_begin(&c, TPM_ST_SESSIONS, TPM_CC_Unseal);
	tpm_cmd_u32(&c, handle);
	tpm_cmd_password_session(&c);
	n = run(&c);
	rc = tpm_rsp_begin(&r, g_rsp, n);
	if (rc) return rc;
	(void)tpm_rsp_u32(&r);
	data = tpm_rsp_b2b(&r, &bytes, NULL);
	if (!data || bytes != sizeof(g_secret) || memcmp(data, g_secret, bytes) != 0) return 0xFFFFFFFEu;
	return 0;
}

static unsigned flush_context(unsigned handle)
{
	struct tpm_cmd c;
	struct tpm_rsp r;
	size_t n;
	tpm_cmd_begin(&c, TPM_ST_NO_SESSIONS, TPM_CC_FlushContext);
	tpm_cmd_u32(&c, handle);
	n = run(&c);
	return tpm_rsp_begin(&r, g_rsp, n);
}

static unsigned pcr_extend(unsigned pcr, const unsigned char digest[32])
{
	struct tpm_cmd c;
	struct tpm_rsp r;
	size_t n;
	tpm_cmd_begin(&c, TPM_ST_SESSIONS, TPM_CC_PCR_Extend);
	tpm_cmd_u32(&c, pcr);
	tpm_cmd_password_session(&c);
	tpm_cmd_u32(&c, 1);		/* TPML_DIGEST_VALUES: one digest */
	tpm_cmd_u16(&c, 0x000B);
	tpm_cmd_bytes(&c, digest, 32);
	n = run(&c);
	return tpm_rsp_begin(&r, g_rsp, n);
}

/* The SHA-256 value of one PCR (0-7). */
static unsigned pcr_read(unsigned pcr, unsigned char value[32])
{
	struct tpm_cmd c;
	struct tpm_rsp r;
	const unsigned char *digest;
	size_t n, bytes;
	unsigned rc, count, i;
	tpm_cmd_begin(&c, TPM_ST_NO_SESSIONS, TPM_CC_PCR_Read);
	tpm_cmd_u32(&c, 1);		/* TPML_PCR_SELECTION: one bank */
	tpm_cmd_u16(&c, 0x000B);
	tpm_cmd_u8(&c, 3);
	tpm_cmd_u8(&c, 1u << pcr);
	tpm_cmd_u8(&c, 0);
	tpm_cmd_u8(&c, 0);
	n = run(&c);
	rc = tpm_rsp_begin(&r, g_rsp, n);
	if (rc) return rc;
	(void)tpm_rsp_u32(&r);		/* pcrUpdateCounter */
	count = tpm_rsp_u32(&r);	/* pcrSelectionOut */
	for (i = 0; i < count && !r.bad; i++) {
		unsigned select_bytes;
		(void)tpm_rsp_u16(&r);
		select_bytes = tpm_rsp_u16(&r) >> 8;	/* sizeofSelect, then the first select byte */
		tpm_rsp_skip(&r, select_bytes ? select_bytes - 1 : 0);
	}
	if (tpm_rsp_u32(&r) != 1) return 0xFFFFFFFEu;	/* pcrValues: one digest */
	digest = tpm_rsp_b2b(&r, &bytes, NULL);
	if (!digest || bytes != 32) return 0xFFFFFFFEu;
	memcpy(value, digest, 32);
	return 0;
}

static int all_zero(const unsigned char *p, size_t n)
{
	while (n--)
		if (*p++) return 0;
	return 1;
}

/* ---- the test */

int engine_test_run(const char *mutation)
{
	static unsigned char primary[1024], primary_again[1024], sealed_private[1024], sealed_public[512];
	unsigned char random[32], pcr4[32], pcr4_after[32];
	size_t primary_bytes = sizeof(primary), primary_again_bytes, private_bytes = sizeof(sealed_private);
	size_t public_bytes = sizeof(sealed_public), permanent_bytes = 0, volatile_bytes = 0;
	unsigned primary_handle = 0, sealed_handle = 0, again_handle = 0, again_sealed = 0, rc;

	g_mutation = mutation;
	host_print("engine host test%s%s", mutation ? ", mutation " : "", mutation ? mutation : "");

	step_begin("init (C runtime, OpenSSL, DRBG seeded from BCryptGenRandom)");
	check(tpm_engine_init(host_platform()) == 1, "tpm_engine_init returns 1");
	check(tpm_engine_init(host_platform()) == 0, "a second tpm_engine_init is refused");
	step_end();

	step_begin("AES-256-GCM of the state file (GCM specification, test case 16)");
	{
		/* McGrew and Viega, "The Galois/Counter Mode of Operation", test case 16: 256-bit key, 60 bytes of
		 * plaintext, 20 bytes of additional data. The values agree with .NET AesGcm on the development PC. */
		static const unsigned char key[32] = {
			0xfe, 0xff, 0xe9, 0x92, 0x86, 0x65, 0x73, 0x1c, 0x6d, 0x6a, 0x8f, 0x94, 0x67, 0x30, 0x83, 0x08,
			0xfe, 0xff, 0xe9, 0x92, 0x86, 0x65, 0x73, 0x1c, 0x6d, 0x6a, 0x8f, 0x94, 0x67, 0x30, 0x83, 0x08};
		static const unsigned char iv[12] = {0xca, 0xfe, 0xba, 0xbe, 0xfa, 0xce, 0xdb, 0xad, 0xde, 0xca, 0xf8, 0x88};
		static const unsigned char aad[20] = {0xfe, 0xed, 0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef, 0xfe, 0xed,
						      0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef, 0xab, 0xad, 0xda, 0xd2};
		static const unsigned char plain[60] = {
			0xd9, 0x31, 0x32, 0x25, 0xf8, 0x84, 0x06, 0xe5, 0xa5, 0x59, 0x09, 0xc5, 0xaf, 0xf5, 0x26, 0x9a,
			0x86, 0xa7, 0xa9, 0x53, 0x15, 0x34, 0xf7, 0xda, 0x2e, 0x4c, 0x30, 0x3d, 0x8a, 0x31, 0x8a, 0x72,
			0x1c, 0x3c, 0x0c, 0x95, 0x95, 0x68, 0x09, 0x53, 0x2f, 0xcf, 0x0e, 0x24, 0x49, 0xa6, 0xb5, 0x25,
			0xb1, 0x6a, 0xed, 0xf5, 0xaa, 0x0d, 0xe6, 0x57, 0xba, 0x63, 0x7b, 0x39};
		static const unsigned char cipher[60] = {
			0x52, 0x2d, 0xc1, 0xf0, 0x99, 0x56, 0x7d, 0x07, 0xf4, 0x7f, 0x37, 0xa3, 0x2a, 0x84, 0x42, 0x7d,
			0x64, 0x3a, 0x8c, 0xdc, 0xbf, 0xe5, 0xc0, 0xc9, 0x75, 0x98, 0xa2, 0xbd, 0x25, 0x55, 0xd1, 0xaa,
			0x8c, 0xb0, 0x8e, 0x48, 0x59, 0x0d, 0xbb, 0x3d, 0xa7, 0xb0, 0x8b, 0x10, 0x56, 0x82, 0x88, 0x38,
			0xc5, 0xf6, 0x1e, 0x63, 0x93, 0xba, 0x7a, 0x0a, 0xbc, 0xc9, 0xf6, 0x62};
		static const unsigned char tag[16] = {0x76, 0xfc, 0x6e, 0xce, 0x0f, 0x4e, 0x17, 0x68,
						      0xcd, 0xdf, 0x88, 0x53, 0xbb, 0x2d, 0x55, 0x1b};
		unsigned char out[60], t[16];

		check(tpm_engine_gcm(1, key, iv, aad, sizeof(aad), plain, sizeof(plain), out, t) == 1 &&
		      memcmp(out, cipher, sizeof(cipher)) == 0 && memcmp(t, tag, sizeof(tag)) == 0,
		      "the seal gives the ciphertext and the tag of the test case");
		memcpy(t, tag, sizeof(t));
		if (mutated("gcm-tag")) t[15] ^= 1;	/* the open must then refuse */
		memcpy(out, cipher, sizeof(out));
		check(tpm_engine_gcm(0, key, iv, aad, sizeof(aad), out, sizeof(out), out, t) == 1 &&
		      memcmp(out, plain, sizeof(plain)) == 0, "the open in place gives the plaintext");
		t[15] ^= 1;
		check(tpm_engine_gcm(0, key, iv, aad, sizeof(aad), cipher, sizeof(cipher), out, t) == 0,
		      "the open refuses a changed tag");
		memcpy(t, tag, sizeof(t));
		check(tpm_engine_gcm(0, key, iv, aad, sizeof(aad) - 1u, cipher, sizeof(cipher), out, t) == 0,
		      "the open refuses changed additional data");
	}
	step_end();

	step_begin("start (manufacture)");
	rc = tpm_engine_start(NULL, 0, NULL, 0, 0);
	host_print("  start returns 0x%x, dirty %d", rc, tpm_engine_permanent_dirty());
	check(rc == 0, "start without state manufactures a TPM");
	check(tpm_engine_permanent_dirty() == 1, "a manufacture makes the permanent state dirty");
	step_end();

	step_begin("TPM2_Startup(CLEAR)");
	check(startup_clear() == TPM_RC_SUCCESS, "TPM2_Startup(CLEAR) succeeds");
	step_end();

	step_begin("TPM2_GetRandom(32)");
	check(get_random(random, sizeof(random)) == 0 && !all_zero(random, sizeof(random)), "32 random bytes, not all zero");
	step_end();

	step_begin("TPM2_CreatePrimary (RSA 2048 storage key, owner hierarchy)");
	rc = create_primary(&primary_handle, primary, &primary_bytes);
	host_print("  rc 0x%x, handle 0x%08x, public area %zu bytes", rc, primary_handle, primary_bytes);
	check(rc == 0 && (primary_handle >> 24) == 0x80, "the storage key is a transient object");
	step_end();

	step_begin("TPM2_Create (sealed data) + TPM2_Load + TPM2_Unseal");
	rc = create_sealed(primary_handle, sealed_private, &private_bytes, sealed_public, &public_bytes);
	check(rc == 0, "TPM2_Create of the sealed object");
	rc = load(primary_handle, sealed_private, private_bytes, sealed_public, public_bytes, &sealed_handle);
	check(rc == 0, "TPM2_Load of the sealed object");
	check(unseal_matches(sealed_handle) == 0, "TPM2_Unseal gives the secret");
	step_end();

	step_begin("TPM2_PCR_Extend(SHA-256, PCR 4) + TPM2_PCR_Read");
	check(pcr_read(4, pcr4) == 0 && all_zero(pcr4, 32), "PCR 4 is zero after TPM2_Startup(CLEAR)");
	check(pcr_extend(4, g_extend) == 0, "TPM2_PCR_Extend succeeds");
	check(pcr_read(4, pcr4) == 0, "TPM2_PCR_Read succeeds");
	check(memcmp(pcr4, g_expected_pcr4, 32) == 0, "PCR 4 is SHA-256(zero PCR || digest), as computed outside");
	if (mutated("pcr-after-resume")) (void)pcr_extend(4, g_extend);
	step_end();

	step_begin("get_state(PERMANENT) + get_state(VOLATILE)");
	permanent_bytes = tpm_engine_get_state(TPM_ENGINE_PERMANENT, NULL, 0);
	check(permanent_bytes > 0 && permanent_bytes <= sizeof(g_permanent), "the permanent state has a size");
	check(tpm_engine_get_state(TPM_ENGINE_PERMANENT, g_permanent, permanent_bytes - 1) == 0,
	      "a buffer one byte too small gets nothing");
	check(tpm_engine_get_state(TPM_ENGINE_PERMANENT, g_permanent, sizeof(g_permanent)) == permanent_bytes,
	      "the permanent state is copied");
	check(tpm_engine_permanent_dirty() == 0, "the copy clears the dirty flag");
	volatile_bytes = tpm_engine_get_state(TPM_ENGINE_VOLATILE, g_volatile, sizeof(g_volatile));
	check(volatile_bytes > 0, "the volatile state is copied");
	host_print("  permanent %zu bytes, volatile %zu bytes", permanent_bytes, volatile_bytes);
	step_end();

	step_begin("stop");
	tpm_engine_stop();
	check(tpm_engine_process(g_rsp, 12, g_rsp, sizeof(g_rsp)) == 0, "a stopped TPM runs no command");
	step_end();

	step_begin("start(volatile without permanent) is refused");
	check(tpm_engine_start(NULL, 0, g_volatile, volatile_bytes, 0) == TPM_ENGINE_BAD_PARAMETER, "TPM_ENGINE_BAD_PARAMETER");
	step_end();

	step_begin("start(permanent, volatile): resume without TPM2_Startup");
	rc = tpm_engine_start(g_permanent, permanent_bytes, g_volatile, volatile_bytes, 0);
	host_print("  start returns 0x%x, dirty %d", rc, tpm_engine_permanent_dirty());
	check(rc == 0, "the resume starts");
	check(pcr_read(4, pcr4_after) == 0, "TPM2_PCR_Read without TPM2_Startup succeeds");
	check(memcmp(pcr4_after, pcr4, 32) == 0, "PCR 4 has the value of before the stop");
	check(unseal_matches(sealed_handle) == 0, "the sealed object that was loaded before the stop unseals");
	check(startup_clear() == TPM_RC_INITIALIZE, "TPM2_Startup now gives TPM_RC_INITIALIZE: the TPM runs already");
	step_end();

	step_begin("TPM2_CreatePrimary again + TPM2_Load of the stored object + TPM2_Unseal");
	/* libtpms has 3 object slots (MAX_LOADED_OBJECTS): the two objects of before the stop go first */
	check(flush_context(sealed_handle) == 0 && flush_context(primary_handle) == 0,
	      "TPM2_FlushContext of the two objects that the resume kept");
	primary_again_bytes = sizeof(primary_again);
	rc = create_primary(&again_handle, primary_again, &primary_again_bytes);
	check(rc == 0, "TPM2_CreatePrimary with the same template");
	check(primary_again_bytes == primary_bytes && memcmp(primary_again, primary, primary_bytes) == 0,
	      "the same storage key (the public area is the same byte for byte)");
	rc = load(again_handle, sealed_private, private_bytes, sealed_public, public_bytes, &again_sealed);
	if (rc) host_print("  TPM2_Load rc 0x%x", rc);
	check(rc == 0, "TPM2_Load of the stored private and public parts under it");
	check(unseal_matches(again_sealed) == 0, "TPM2_Unseal gives the secret");
	step_end();
	tpm_engine_stop();

	step_begin("start(permanent, damaged volatile) is refused");
	memcpy(g_damaged, g_volatile, volatile_bytes);
	if (!mutated("volatile-intact")) g_damaged[volatile_bytes / 2] ^= 0x01;
	rc = tpm_engine_start(g_permanent, permanent_bytes, g_damaged, volatile_bytes, 0);
	host_print("  start returns 0x%x", rc);
	check(rc != 0, "a volatile blob with one changed bit is refused");
	if (rc == 0) tpm_engine_stop();
	rc = tpm_engine_start(g_permanent, permanent_bytes, g_volatile, volatile_bytes, 0);
	check(rc == 0 && pcr_read(4, pcr4_after) == 0 && memcmp(pcr4_after, pcr4, 32) == 0,
	      "after the refusal the sound blobs start, with the same PCR 4");
	tpm_engine_stop();
	step_end();

	step_begin("start(permanent) + TPM2_Startup(CLEAR): a TPM reset");
	rc = tpm_engine_start(g_permanent, permanent_bytes, NULL, 0, 0);
	check(rc == 0, "start with the permanent state only");
	check(startup_clear() == TPM_RC_SUCCESS, "TPM2_Startup(CLEAR) succeeds");
	check(pcr_read(4, pcr4_after) == 0 && all_zero(pcr4_after, 32), "PCR 4 is zero again");
	primary_again_bytes = sizeof(primary_again);
	rc = create_primary(&again_handle, primary_again, &primary_again_bytes);
	check(rc == 0 && primary_again_bytes == primary_bytes && memcmp(primary_again, primary, primary_bytes) == 0,
	      "the seeds are in the permanent state: the same storage key after the reset");
	tpm_engine_stop();
	step_end();

	step_begin("wipe");
	tpm_engine_wipe();
	check(tpm_engine_start(NULL, 0, NULL, 0, 0) == TPM_ENGINE_FAIL, "after the wipe the engine does not start");
	step_end();

	host_print("arena peak %zu bytes (%zu KiB)", tpm_engine_arena_peak(), (tpm_engine_arena_peak() + 1023) / 1024);
	host_print("%s: %d check(s) failed", g_failures ? "FAIL" : "PASS", g_failures);
	return g_failures ? 1 : 0;
}
