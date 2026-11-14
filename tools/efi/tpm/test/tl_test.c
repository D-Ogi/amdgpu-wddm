/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host tests of the pure code of the pre-OS TPM: the event log, the TPM 2.0 commands, the measurement order and
 * the TCG2 protocol logic. A model TPM stands for the engine: it keeps 24 SHA-256 PCRs and extends them as a TPM
 * does, so a replay of the log must give its values.
 *
 * The positive control is a real log: the measured-boot log of a Windows PC with a firmware TPM, read by the same
 * reader and replayed (tools/efi/tpm/tools/tpm_pcrs.exe compared that replay with the live PCRs of that TPM). Its
 * path comes from the runner; without it the check says SKIP.
 */
#include "efl_test.h"
#include "efl_pe.h"
#include "efl_sha256.h"
#include "tl_log.h"
#include "tl_measure.h"
#include "tl_tcg2.h"
#include "tl_tpm2.h"
#include "handoff.h"
#include "statefile.h"

struct model_tpm {
	unsigned char pcr[24][TL_SHA256_BYTES];
	unsigned extends;
	unsigned order[64][2];	/* pcr, call index */
	int fail_at;		/* the extend with this index fails; -1 never */
};

static int model_extend(void *context, unsigned pcr, const unsigned char digest[TL_SHA256_BYTES])
{
	struct model_tpm *t = context;
	struct efl_sha256 s;
	if (t->fail_at >= 0 && (unsigned)t->fail_at == t->extends) {
		t->extends++;
		return 0;
	}
	if (pcr >= 24u) return 0;
	if (t->extends < 64u) t->order[t->extends][0] = pcr;
	t->extends++;
	efl_sha256_init(&s);
	efl_sha256_update(&s, t->pcr[pcr], TL_SHA256_BYTES);
	efl_sha256_update(&s, digest, TL_SHA256_BYTES);
	efl_sha256_final(&s, t->pcr[pcr]);
	return 1;
}

static void model_init(struct model_tpm *t)
{
	memset(t, 0, sizeof(*t));
	memset(t->pcr[17], 0xFF, 6u * TL_SHA256_BYTES);	/* as after TPM2_Startup */
	t->fail_at = -1;
}

static unsigned char area[65536], final_area[16384], scratch[4096];

static void test_log_format(void)
{
	struct tl_log log;
	struct tl_reader reader;
	struct tl_event e;
	unsigned char digest[TL_SHA256_BYTES], pcrs[24][TL_SHA256_BYTES];
	size_t cursor = 0;

	CHECK(tl_log_init(&log, area, sizeof(area), final_area, sizeof(final_area)) == 1);
	CHECK(log.used == TL_SPEC_ID_ENTRY_BYTES && TL_SPEC_ID_ENTRY_BYTES == 65u);
	/* The Spec ID entry, byte by byte where the specification fixes the value. */
	CHECK(!memcmp(area + 32, "Spec ID Event03", 16) && area[32 + 15] == 0);
	CHECK(area[4] == 3 && area[28] == TL_SPEC_ID_EVENT_BYTES && area[32 + 21] == 2 && area[32 + 23] == 2);
	CHECK(area[32 + 24] == 1 && area[32 + 28] == 0x0B && area[32 + 29] == 0 && area[32 + 30] == 32);
	CHECK(area[log.used] == 0xFF && area[sizeof(area) - 1] == 0xFF);
	/* The final table header: version 1, no events. */
	CHECK(final_area[0] == 1 && final_area[8] == 0 && log.final_used == TL_FINAL_HEADER_BYTES);

	memset(&reader, 0, sizeof(reader));
	CHECK(tl_log_next(area, log.used, &cursor, &reader, &e) == 1 && reader.algs == 1 && reader.id[0] == 0x0B &&
	      reader.size[0] == 32);
	CHECK(tl_log_next(area, sizeof(area), &cursor, &reader, &e) == 0);	/* 0xFF: the end */

	/* An event before GetEventLog is only in the log; one after it is in the final table too. */
	efl_sha256("a", 1, digest);
	CHECK(tl_log_add(&log, 4, TL_EV_EFI_ACTION, digest, "a", 1) == 1);
	CHECK(log.final_used == TL_FINAL_HEADER_BYTES && final_area[8] == 0);
	tl_log_final_start(&log);
	CHECK(tl_log_add(&log, 5, TL_EV_EFI_ACTION, digest, "a", 1) == 1);
	CHECK(final_area[8] == 1 && log.final_used == TL_FINAL_HEADER_BYTES + TL_EVENT2_HEADER_BYTES + 1u);
	CHECK(tl_log_last(&log) == TL_SPEC_ID_ENTRY_BYTES + TL_EVENT2_HEADER_BYTES + 1u);

	/* The final table reads with the reader of the main log. */
	cursor = TL_FINAL_HEADER_BYTES;
	CHECK(tl_log_next(final_area, log.final_used, &cursor, &reader, &e) == 1 && e.pcr == 5 && e.bytes == 1 &&
	      e.digest != NULL && !memcmp(e.digest, digest, 32));

	/* The replay: PCR 4 and 5 each = SHA-256(zeros || digest). */
	CHECK(tl_log_replay(area, log.used, pcrs) == 3);
	{
		unsigned char zero[TL_SHA256_BYTES], expect[TL_SHA256_BYTES];
		struct efl_sha256 s;
		memset(zero, 0, sizeof(zero));
		efl_sha256_init(&s);
		efl_sha256_update(&s, zero, 32);
		efl_sha256_update(&s, digest, 32);
		efl_sha256_final(&s, expect);
		CHECK(!memcmp(pcrs[4], expect, 32) && !memcmp(pcrs[5], expect, 32) && !memcmp(pcrs[0], zero, 32));
		CHECK(pcrs[17][0] == 0xFF && pcrs[22][31] == 0xFF && pcrs[23][0] == 0);
	}

	/* A malformed entry: a declared size beyond the bytes. */
	{
		unsigned char copy[256];
		memcpy(copy, area, log.used);
		copy[TL_SPEC_ID_ENTRY_BYTES + 14 + 32] = 0xF0;
		CHECK(tl_log_replay(copy, log.used, pcrs) == -1);
		memcpy(copy, area, log.used);
		copy[32] = 'X';		/* not a Spec ID entry */
		CHECK(tl_log_replay(copy, log.used, pcrs) == -1);
	}
}

static void test_log_full(void)
{
	static unsigned char small[TL_SPEC_ID_ENTRY_BYTES + TL_EVENT2_HEADER_BYTES + 4u], small_final[TL_FINAL_HEADER_BYTES + 10u];
	struct tl_log log;
	unsigned char digest[TL_SHA256_BYTES] = {0};

	CHECK(tl_log_init(&log, small, sizeof(small), small_final, sizeof(small_final)) == 1);
	CHECK(tl_log_add(&log, 1, TL_EV_SEPARATOR, digest, "abcd", 4) == 1);
	CHECK(tl_log_add(&log, 1, TL_EV_SEPARATOR, digest, "abcd", 4) == 0 && log.truncated);
	/* After the first miss nothing more goes in, also not a smaller event: the log stays a prefix. */
	CHECK(tl_log_add(&log, 1, TL_EV_SEPARATOR, digest, NULL, 0) == 0);
	tl_log_final_start(&log);
	CHECK(tl_log_add(&log, 1, TL_EV_SEPARATOR, digest, "abcd", 4) == 0 && log.final_truncated);
	CHECK(tl_log_init(&log, small, TL_SPEC_ID_ENTRY_BYTES - 1u, small_final, sizeof(small_final)) == 0);
}

static void test_real_log(const char *path)
{
	unsigned char pcrs[24][TL_SHA256_BYTES];
	size_t bytes = 0;
	unsigned char *log = efl_test_load("", path, &bytes);
	if (log == NULL) {
		printf("SKIP the real TCG log: no file %s\n", path != NULL ? path : "(none given)");
		return;
	}
	/* 43 events, of which the reader must read every one: the count tpm_pcrs.exe printed for the same file. */
	CHECK(tl_log_replay(log, bytes, pcrs) == 43);
	free(log);
}

static void test_tpm2(void)
{
	static const unsigned char startup[12] = {0x80, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x01, 0x44, 0x00, 0x00};
	unsigned char cmd[128], rsp[256], pcrs[24][TL_SHA256_BYTES], digest[32];
	unsigned got = 9;
	int only, full, ok;
	size_t n, i;

	/* The same bytes as the startup command of driver/tpm/core/tpmcore.c. */
	CHECK(tl_tpm2_startup(cmd, sizeof(cmd), TL_TPM_SU_CLEAR) == 12 && !memcmp(cmd, startup, 12));
	CHECK(tl_tpm2_startup(cmd, 11, TL_TPM_SU_CLEAR) == 0);
	CHECK(tl_tpm2_shutdown(cmd, sizeof(cmd), TL_TPM_SU_CLEAR) == 12 && cmd[9] == 0x45);

	for (i = 0; i < 32; i++) digest[i] = (unsigned char)i;
	n = tl_tpm2_pcr_extend(cmd, sizeof(cmd), 4, digest);
	/* header 10, pcr handle 4, auth size 4, session 9, count 4, alg 2, digest 32 */
	CHECK(n == 65 && cmd[0] == 0x80 && cmd[1] == 0x02 && cmd[5] == 65 && cmd[9] == 0x82 && cmd[13] == 4);
	CHECK(cmd[17] == 9 && cmd[18] == 0x40 && cmd[21] == 0x09 && cmd[30] == 1 && cmd[32] == 0x0B && cmd[33] == 0);
	CHECK(tl_tpm2_pcr_extend(cmd, sizeof(cmd), 24, digest) == 0);

	n = tl_tpm2_pcr_read(cmd, sizeof(cmd), 8, 8);
	CHECK(n == 20 && cmd[9] == 0x7E && cmd[13] == 1 && cmd[15] == 0x0B && cmd[16] == 3 && cmd[17] == 0 &&
	      cmd[18] == 0xFF && cmd[19] == 0);
	CHECK(tl_tpm2_pcr_read(cmd, sizeof(cmd), 20, 8) == 0 && tl_tpm2_pcr_read(cmd, sizeof(cmd), 0, 9) == 0);

	/* An answer with two PCRs, 8 and 9. */
	memset(rsp, 0, sizeof(rsp));
	n = 0;
	rsp[0] = 0x80; rsp[1] = 0x01; n = 10;
	n += 4;						/* pcrUpdateCounter */
	rsp[n + 3] = 1; n += 4;				/* one selection */
	rsp[n + 1] = 0x0B; rsp[n + 2] = 3; rsp[n + 4] = 0x03; n += 6;
	rsp[n + 3] = 2; n += 4;				/* two digests */
	for (i = 0; i < 2; i++) {
		rsp[n + 1] = 32;
		memset(rsp + n + 2, (int)(0xA0 + i), 32);
		n += 34;
	}
	rsp[5] = (unsigned char)n;
	memset(pcrs, 0, sizeof(pcrs));
	CHECK(tl_tpm2_pcr_read_answer(rsp, n, 8, pcrs, &got) == 0 && got == 2 && pcrs[8][0] == 0xA0 && pcrs[9][31] == 0xA1 &&
	      pcrs[10][0] == 0);
	CHECK(tl_tpm2_pcr_read_answer(rsp, n - 1, 8, pcrs, &got) == TL_TPM2_MALFORMED);	/* size field differs */
	rsp[n - 34 + 1] = 20;	/* a digest of the wrong size */
	CHECK(tl_tpm2_pcr_read_answer(rsp, n, 8, pcrs, &got) == TL_TPM2_MALFORMED);

	/* TPM_CAP_PCRS: SHA-1 with all PCRs and SHA-256 with all PCRs, then SHA-256 only. */
	memset(rsp, 0, sizeof(rsp));
	rsp[0] = 0x80; rsp[1] = 0x01; n = 11;
	rsp[n + 3] = 5; n += 4;
	rsp[n + 3] = 2; n += 4;
	rsp[n + 1] = 0x04; rsp[n + 2] = 3; memset(rsp + n + 3, 0xFF, 3); n += 6;
	rsp[n + 1] = 0x0B; rsp[n + 2] = 3; memset(rsp + n + 3, 0xFF, 3); n += 6;
	rsp[5] = (unsigned char)n;
	CHECK(tl_tpm2_get_pcrs_answer(rsp, n, &only, &full) == 0 && !only && full);
	memset(rsp + n - 9, 0, 3);	/* SHA-1 selects nothing */
	CHECK(tl_tpm2_get_pcrs_answer(rsp, n, &only, &full) == 0 && only && full);
	CHECK(tl_tpm2_get_pcrs(cmd, sizeof(cmd)) == 22 && cmd[13] == 5 && cmd[21] == 1);

	{
		unsigned others[2] = {0x0004u, 0x000Cu};
		n = tl_tpm2_pcr_allocate_sha256(cmd, sizeof(cmd), others, 2);
		/* header 0-9, platform 10-13, auth size 14-17, session 18-26, count 27-30, then three selections of
		 * 6 bytes (hash 2, sizeofSelect 1, pcrSelect 3) at 31, 37 and 43 */
		CHECK(n == 49 && cmd[9] == 0x2B && cmd[10] == 0x40 && cmd[13] == 0x0C && cmd[30] == 3 && cmd[32] == 0x0B &&
		      cmd[33] == 3 && cmd[34] == 0xFF && cmd[36] == 0xFF && cmd[38] == 0x04 && cmd[39] == 3 && cmd[40] == 0 &&
		      cmd[42] == 0 && cmd[44] == 0x0C && cmd[48] == 0);
	}
	/* TPM_PT_MANUFACTURER: the command, and an answer with two properties of which the second is asked. */
	{
		unsigned long value = 0;
		CHECK(tl_tpm2_get_property(cmd, sizeof(cmd), TL_TPM_PT_MANUFACTURER) == 22 && cmd[13] == 6 && cmd[16] == 1 &&
		      cmd[17] == 0x05 && cmd[21] == 1);
		memset(rsp, 0, sizeof(rsp));
		rsp[0] = 0x80; rsp[1] = 0x01; rsp[5] = 35; rsp[14] = 6; rsp[18] = 2;
		rsp[21] = 0x01; rsp[22] = 0x04; rsp[26] = 7;				/* 0x104 = 7 */
		rsp[29] = 0x01; rsp[30] = 0x05; memcpy(rsp + 31, "IBM ", 4);	/* 0x105 = "IBM " */
		CHECK(tl_tpm2_get_property_answer(rsp, 35, TL_TPM_PT_MANUFACTURER, &value) == 0 && value == 0x49424D20ul);
		CHECK(tl_tpm2_get_property_answer(rsp, 35, 0x106u, &value) == TL_TPM2_MALFORMED);
	}
	memset(rsp, 0, sizeof(rsp));
	rsp[0] = 0x80; rsp[1] = 0x02; rsp[5] = 27; rsp[13] = 17; rsp[14] = 1;
	CHECK(tl_tpm2_pcr_allocate_answer(rsp, 27, &ok) == 0 && ok);
	rsp[9] = 0x84;	/* an error code */
	CHECK(tl_tpm2_pcr_allocate_answer(rsp, 27, &ok) == 0x84 && !ok);
}

static const unsigned short secure_boot[] = {'S', 'e', 'c', 'u', 'r', 'e', 'B', 'o', 'o', 't', 0};
static const unsigned short pk[] = {'P', 'K', 0};
static const unsigned short kek[] = {'K', 'E', 'K', 0};
static const unsigned short db[] = {'d', 'b', 0};
static const unsigned short dbx[] = {'d', 'b', 'x', 0};

static void fill_preboot(struct tl_preboot *p)
{
	static const unsigned char off = 0;
	memset(p, 0, sizeof(*p));
	p->version = "4";
	memset(p->stage_digest, 0x5A, sizeof(p->stage_digest));
	p->secure[0].name = secure_boot;
	p->secure[0].data = &off;
	p->secure[0].bytes = 1;
	p->secure[1].name = pk;
	p->secure[2].name = kek;
	p->secure[3].name = db;
	p->secure[4].name = dbx;
}

static void test_measure(void)
{
	static const unsigned expect_pcr[] = {0, 0, 7, 7, 7, 7, 7, 7, 4, 0, 1, 2, 3, 4, 5, 6};
	static const unsigned expect_type[] = {TL_EV_S_CRTM_VERSION, TL_EV_S_CRTM_CONTENTS, TL_EV_EFI_VARIABLE_DRIVER_CONFIG,
		TL_EV_EFI_VARIABLE_DRIVER_CONFIG, TL_EV_EFI_VARIABLE_DRIVER_CONFIG, TL_EV_EFI_VARIABLE_DRIVER_CONFIG,
		TL_EV_EFI_VARIABLE_DRIVER_CONFIG, TL_EV_SEPARATOR, TL_EV_EFI_ACTION, TL_EV_SEPARATOR, TL_EV_SEPARATOR,
		TL_EV_SEPARATOR, TL_EV_SEPARATOR, TL_EV_SEPARATOR, TL_EV_SEPARATOR, TL_EV_SEPARATOR};
	struct model_tpm tpm;
	struct tl_log log;
	struct tl_measurer m;
	struct tl_preboot p;
	struct tl_reader reader;
	struct tl_event e;
	unsigned char replay[24][TL_SHA256_BYTES], digest[32];
	size_t cursor = 0, n;
	unsigned i = 0;
	int r;

	model_init(&tpm);
	CHECK(tl_log_init(&log, area, sizeof(area), final_area, sizeof(final_area)) == 1);
	m.extend = model_extend;
	m.context = &tpm;
	m.log = &log;
	fill_preboot(&p);
	CHECK(tl_measure_preboot(&m, &p, scratch, sizeof(scratch)) == 0);
	CHECK(tpm.extends == 16);

	memset(&reader, 0, sizeof(reader));
	CHECK(tl_log_next(area, log.used, &cursor, &reader, &e) == 1);	/* Spec ID */
	while ((r = tl_log_next(area, log.used, &cursor, &reader, &e)) == 1 && i < 16u) {
		CHECK(e.pcr == expect_pcr[i] && e.type == expect_type[i]);
		i++;
	}
	CHECK(r == 0 && i == 16u);

	/* The version is UTF-16 with its terminator; the SecureBoot entry is UEFI_VARIABLE_DATA with one byte. */
	cursor = 0;
	tl_log_next(area, log.used, &cursor, &reader, &e);
	tl_log_next(area, log.used, &cursor, &reader, &e);
	CHECK(e.bytes == 4 && e.data[0] == '4' && e.data[1] == 0 && e.data[2] == 0 && e.data[3] == 0);
	tl_log_next(area, log.used, &cursor, &reader, &e);
	CHECK(e.digest != NULL && e.digest[0] == 0x5A && e.bytes == 18);
	tl_log_next(area, log.used, &cursor, &reader, &e);
	CHECK(e.bytes == 16 + 8 + 8 + 20 + 1 && e.data[16] == 10 && e.data[24] == 1 && e.data[32] == 'S' &&
	      e.data[52] == 0);
	efl_sha256(e.data, e.bytes, digest);
	CHECK(!memcmp(digest, e.digest, 32));	/* EV_EFI_VARIABLE_DRIVER_CONFIG: the digest is over the event data */
	tl_log_next(area, log.used, &cursor, &reader, &e);
	CHECK(e.bytes == 16 + 8 + 8 + 4 && e.data[24] == 0);	/* PK: absent, measured with no data */

	/* The image event and ExitBootServices, then the replay equals the model TPM in every PCR. */
	{
		static const unsigned char path[6] = {0x7F, 0xFF, 0x04, 0x00, 0, 0};	/* an end node */
		CHECK(tl_measure_image(&m, digest, 0x1000, 0x2000, path, 4, scratch, sizeof(scratch)) == TL_MEASURED);
		CHECK(tl_measure_exit_boot_services(&m) == 0);
	}
	CHECK(tl_log_replay(area, log.used, replay) == 20);
	CHECK(!memcmp(replay, tpm.pcr, sizeof(replay)));

	/* A failed extend logs nothing and is counted. */
	model_init(&tpm);
	tpm.fail_at = 3;
	CHECK(tl_log_init(&log, area, sizeof(area), final_area, sizeof(final_area)) == 1);
	CHECK(tl_measure_preboot(&m, &p, scratch, sizeof(scratch)) == 1);
	CHECK(tl_log_replay(area, log.used, replay) == 16 && !memcmp(replay, tpm.pcr, sizeof(replay)));

	/* The UEFI_VARIABLE_DATA layout does not fit a small buffer. */
	n = tl_variable_data(&p.secure[0], scratch, 52);
	CHECK(n == 0 && tl_variable_data(&p.secure[0], scratch, 53) == 53);
}

static void tcg2_event(unsigned char *out, unsigned pcr, unsigned type, const char *data, size_t bytes)
{
	unsigned long size = TL_EVENT_FIXED_BYTES + bytes;
	memset(out, 0, TL_EVENT_FIXED_BYTES);
	out[0] = (unsigned char)size;
	out[1] = (unsigned char)(size >> 8);
	out[4] = 14;		/* HeaderSize */
	out[8] = 1;		/* HeaderVersion */
	out[10] = (unsigned char)pcr;
	out[14] = (unsigned char)type;
	out[15] = (unsigned char)(type >> 8);
	out[16] = (unsigned char)(type >> 16);
	out[17] = (unsigned char)(type >> 24);
	memcpy(out + TL_EVENT_FIXED_BYTES, data, bytes);
}

static void test_tcg2(const char *real_image)
{
	struct tl_capability cap = {1, 4096, 4096, 0x49424D00ul};
	struct model_tpm tpm;
	struct tl_log log;
	struct tl_measurer m;
	struct tl_reader reader;
	struct tl_event e;
	unsigned char out[64], event[128], replay[24][TL_SHA256_BYTES];
	size_t cursor;

	/* GetCapability: the 1.1 structure, the 1.0 structure, and a structure that is too small. */
	memset(out, 0xEE, sizeof(out));
	out[0] = 36;
	CHECK(tl_tcg2_capability(&cap, out) == TL_OK && out[0] == 36 && out[1] == 1 && out[2] == 1 && out[8] == 2 &&
	      out[12] == 2 && out[16] == 1 && out[19] == 0x10 && out[27] == 0x49 && out[28] == 1 && out[32] == 2 &&
	      out[36] == 0xEE);
	memset(out, 0xEE, sizeof(out));
	out[0] = 30;
	CHECK(tl_tcg2_capability(&cap, out) == TL_OK && out[0] == 28 && out[2] == 0 && out[4] == 0 && out[28] == 0xEE);
	memset(out, 0xEE, sizeof(out));
	out[0] = 27;
	CHECK(tl_tcg2_capability(&cap, out) == TL_BUFFER_TOO_SMALL && out[0] == 36 && out[1] == 0xEE);

	model_init(&tpm);
	CHECK(tl_log_init(&log, area, sizeof(area), final_area, sizeof(final_area)) == 1);
	m.extend = model_extend;
	m.context = &tpm;
	m.log = &log;

	tcg2_event(event, 8, 0x0D, "boot", 4);	/* an IPL event of a boot manager */
	CHECK(tl_tcg2_hash_log_extend(&m, 1, 0, (const unsigned char *)"data", 4, event) == TL_OK && tpm.extends == 1);
	CHECK(tl_tcg2_hash_log_extend(&m, 0, 0, (const unsigned char *)"data", 4, event) == TL_DEVICE_ERROR);
	CHECK(tl_tcg2_hash_log_extend(&m, 1, 0, NULL, 4, event) == TL_INVALID_PARAMETER);
	CHECK(tl_tcg2_hash_log_extend(&m, 1, 0, (const unsigned char *)"data", 4, NULL) == TL_INVALID_PARAMETER);
	/* Extend only: the PCR changes, the log does not. */
	CHECK(tl_tcg2_hash_log_extend(&m, 1, TL_EXTEND_ONLY, (const unsigned char *)"data", 4, event) == TL_OK &&
	      tpm.extends == 2);
	/* EV_NO_ACTION: logged, not extended; no data is necessary. */
	tcg2_event(event, 0, TL_EV_NO_ACTION, "note", 4);
	CHECK(tl_tcg2_hash_log_extend(&m, 1, 0, NULL, 0, event) == TL_OK && tpm.extends == 2);
	/* A PCR above 23, a header of the wrong version, a size below the header. */
	tcg2_event(event, 24, 0x0D, "x", 1);
	CHECK(tl_tcg2_hash_log_extend(&m, 1, 0, (const unsigned char *)"x", 1, event) == TL_INVALID_PARAMETER);
	tcg2_event(event, 8, 0x0D, "x", 1);
	event[8] = 2;
	CHECK(tl_tcg2_hash_log_extend(&m, 1, 0, (const unsigned char *)"x", 1, event) == TL_INVALID_PARAMETER);
	tcg2_event(event, 8, 0x0D, "x", 1);
	event[0] = 17;
	CHECK(tl_tcg2_hash_log_extend(&m, 1, 0, (const unsigned char *)"x", 1, event) == TL_INVALID_PARAMETER);
	/* A PE image that is not one. */
	tcg2_event(event, 4, TL_EV_EFI_BOOT_SERVICES_APPLICATION, "", 0);
	CHECK(tl_tcg2_hash_log_extend(&m, 1, TL_PE_COFF_IMAGE, (const unsigned char *)"MZ", 2, event) == TL_UNSUPPORTED);

	if (real_image != NULL) {
		size_t bytes = 0;
		unsigned char *image = efl_test_load("", real_image, &bytes);
		struct efl_pe pe;
		if (image == NULL) {
			printf("SKIP the PE image measurement: %s does not read\n", real_image);
		} else {
			CHECK(efl_pe_read(image, bytes, &pe) == 1);
			memset(&reader, 0, sizeof(reader));
			cursor = 0;
			CHECK(tl_log_next(area, log.used, &cursor, &reader, &e) == 1);	/* the Spec ID entry fills the reader */
			tl_log_final_start(&log);
			CHECK(tl_tcg2_hash_log_extend(&m, 1, TL_PE_COFF_IMAGE, image, bytes, event) == TL_OK);
			/* The logged digest is the Authenticode digest, and the event is in the final table too. */
			cursor = log.last;
			CHECK(tl_log_next(area, log.used, &cursor, &reader, &e) == 1 && e.digest != NULL &&
			      !memcmp(e.digest, pe.digest, 32) && e.pcr == 4);
			CHECK(final_area[8] == 1);
			free(image);
		}
	}
	memset(&reader, 0, sizeof(reader));
	CHECK(tl_log_replay(area, log.used, replay) > 0);
	/* PCR 8 had an extend that is not in the log (extend only): the replay must differ there, and only there. */
	CHECK(memcmp(replay[8], tpm.pcr[8], 32) != 0 && !memcmp(replay[4], tpm.pcr[4], 32) &&
	      !memcmp(replay[0], tpm.pcr[0], 32));
}

/* The handoff record (driver/tpm/handoff.c): the round trip, and each defect that the filter and the core refuse. */
static void test_handoff(void)
{
	static unsigned char record[TPM_HANDOFF_HEADER_BYTES + 64];
	static const unsigned char volume[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
	unsigned char permanent[40], volatile_state[24];
	struct tpm_handoff_view view;
	size_t n, i;

	for (i = 0; i < sizeof(permanent); i++) permanent[i] = (unsigned char)(0x40 + i);
	for (i = 0; i < sizeof(volatile_state); i++) volatile_state[i] = (unsigned char)(0x90 + i);
	n = tpm_handoff_write(record, sizeof(record), volume, 7, TPM_HANDOFF_STARTED, permanent, sizeof(permanent),
			      volatile_state, sizeof(volatile_state), efl_sha256);
	CHECK(n == TPM_HANDOFF_HEADER_BYTES + 64u && !memcmp(record, "BCTH", 4));
	CHECK(tpm_handoff_read(record, n, efl_sha256, &view) == TPM_HANDOFF_OK && view.record_bytes == n &&
	      view.permanent_bytes == 40u && view.volatile_bytes == 24u && !memcmp(view.permanent, permanent, 40) &&
	      !memcmp(view.volatile_state, volatile_state, 24) && view.flags == TPM_HANDOFF_STARTED &&
	      view.sequence == 7u && !memcmp(view.volume, volume, 16));
	/* A larger range than the record: the bytes after it are not part of it. */
	CHECK(tpm_handoff_read(record, sizeof(record), efl_sha256, &view) == TPM_HANDOFF_OK);
	/* Too small for the record: no write. */
	CHECK(tpm_handoff_write(record, n - 1u, volume, 7, 0, permanent, 40, volatile_state, 24, efl_sha256) == 0);
	n = tpm_handoff_write(record, sizeof(record), volume, 7, TPM_HANDOFF_STARTED, permanent, 40, volatile_state, 24,
			      efl_sha256);

	/* Each byte after the digest is covered: a change in a blob, the flags or the sequence is BAD_DIGEST. */
	for (i = TPM_HANDOFF_DIGEST_FROM + 12u; i < n; i++) {
		if (i >= 40u && i < 52u) continue;	/* the sizes: BAD_FRAME, below */
		record[i] ^= 0x01;
		if (tpm_handoff_read(record, n, efl_sha256, NULL) != TPM_HANDOFF_BAD_DIGEST) {
			CHECK(!"one changed byte of the record was not found");
			record[i] ^= 0x01;
			break;
		}
		/* The filter does not check the digest: the same record passes its frame check. */
		if (tpm_handoff_read(record, n, NULL, NULL) != TPM_HANDOFF_OK) CHECK(!"the frame check reads the digest");
		record[i] ^= 0x01;
	}
	checks++;
	/* The frame: no magic, another version, a wrong header size, sizes that do not add up, a record beyond the range. */
	record[0] ^= 1;
	CHECK(tpm_handoff_read(record, n, efl_sha256, NULL) == TPM_HANDOFF_NONE);
	record[0] ^= 1;
	record[4] = 2;
	CHECK(tpm_handoff_read(record, n, NULL, NULL) == TPM_HANDOFF_BAD_FRAME);
	record[4] = 1;
	record[6] = 81;
	CHECK(tpm_handoff_read(record, n, NULL, NULL) == TPM_HANDOFF_BAD_FRAME);
	record[6] = 80;
	record[44] = 41;
	CHECK(tpm_handoff_read(record, n, NULL, NULL) == TPM_HANDOFF_BAD_FRAME);
	record[44] = 40;
	CHECK(tpm_handoff_read(record, n - 1u, NULL, NULL) == TPM_HANDOFF_BAD_FRAME);
	CHECK(tpm_handoff_read(record, 10, NULL, NULL) == TPM_HANDOFF_NONE);
	/* A cleared range (the filter zeros it after the copy): no record. */
	memset(record, 0, sizeof(record));
	CHECK(tpm_handoff_read(record, sizeof(record), efl_sha256, NULL) == TPM_HANDOFF_NONE);
}

/* A model of AES-256-GCM for the frame tests of the state file: a keystream of SHA-256(key, nonce, counter) and a
 * tag of SHA-256(key, nonce, additional data, ciphertext). It is an authenticated cipher in the same contract, so
 * every frame rule of statefile.c is exercised; the real GCM has its known-answer test in the engine test. */
static int model_gcm(int seal, const unsigned char key[32], const unsigned char nonce[12], const unsigned char *aad,
		     size_t aad_bytes, const unsigned char *in, size_t n, unsigned char *out, unsigned char tag[16])
{
	unsigned char t[32], block[32];
	struct efl_sha256 s;
	size_t i;

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
			unsigned char ctr[44];
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

/* An RSMB table: the 8-byte header, a type 1 with a UUID, a type 2 whose serial is string 4, the end structure. */
static size_t make_rsmb(unsigned char *t, int with_type1, int with_type2, unsigned char uuid_first)
{
	static const unsigned char type2_strings[] = "maker\0board\0v1\0SERIAL-42\0";
	size_t p = 8, i;
	memset(t, 0, 256);
	t[1] = 3; t[2] = 3;
	if (with_type1) {
		t[p] = 1; t[p + 1] = 0x1B; t[p + 2] = 1;
		for (i = 0; i < 16; i++) t[p + 8 + i] = (unsigned char)(uuid_first + i);
		p += 0x1B;
		t[p] = 0; t[p + 1] = 0;	/* no strings: the double zero */
		p += 2;
	}
	if (with_type2) {
		t[p] = 2; t[p + 1] = 0x0F; t[p + 2] = 2;
		t[p + 4] = 1; t[p + 5] = 2; t[p + 6] = 3; t[p + 7] = 4;
		p += 0x0F;
		memcpy(t + p, type2_strings, sizeof(type2_strings));	/* the last NUL of the literal ends the set */
		p += sizeof(type2_strings);
	}
	t[p] = 127; t[p + 1] = 4; p += 4;
	t[p] = 0; t[p + 1] = 0; p += 2;
	t[4] = (unsigned char)(p - 8);
	return p;
}

/* The expected key, built here from its definition and not from statefile.c. */
static void expect_key(unsigned char uuid_first, const char *serial, unsigned char key[32])
{
	static const char label[] = "BC-250 TPM state key, version 1";
	unsigned char in[128];
	size_t n = sizeof(label) - 1u, len = strlen(serial), i;
	memcpy(in, label, n);
	for (i = 0; i < 16; i++) in[n + i] = (unsigned char)(uuid_first + i);
	in[n + 16] = (unsigned char)len;
	in[n + 17] = 0;
	memcpy(in + n + 18, serial, len);
	efl_sha256(in, n + 18 + len, key);
}

/* The state file (driver/tpm/statefile.c): the board key, the round trip, each defect, and the A/B choice. */
static void test_statefile(void)
{
	static unsigned char file[TPM_STATEFILE_HEADER_BYTES + 100 + TPM_STATEFILE_TAG_BYTES], blob[100], back[100];
	static const unsigned char nonce[12] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2 };
	unsigned char rsmb[256], key[32], other[32], want[32];
	unsigned long long seq = 0, next = 0;
	size_t n, got = 0, i, r;
	int bad = 0;

	/* The key: label, UUID, serial; another UUID or no serial is another key; no type 1 is no key. */
	r = make_rsmb(rsmb, 1, 1, 0x10);
	expect_key(0x10, "SERIAL-42", want);
	CHECK(tpm_statefile_board_key(rsmb, r, efl_sha256, key) == 1 && !memcmp(key, want, 32));
	r = make_rsmb(rsmb, 1, 0, 0x10);
	expect_key(0x10, "", want);
	CHECK(tpm_statefile_board_key(rsmb, r, efl_sha256, other) == 1 && !memcmp(other, want, 32) && memcmp(other, key, 32));
	r = make_rsmb(rsmb, 1, 1, 0x11);
	CHECK(tpm_statefile_board_key(rsmb, r, efl_sha256, other) == 1 && memcmp(other, key, 32));
	r = make_rsmb(rsmb, 0, 1, 0x10);
	CHECK(tpm_statefile_board_key(rsmb, r, efl_sha256, other) == 0);
	/* A table cut inside the type 1 structure: no key, no read past the end. */
	r = make_rsmb(rsmb, 1, 1, 0x10);
	CHECK(tpm_statefile_board_key(rsmb, 8 + 20, efl_sha256, other) == 0);
	CHECK(tpm_statefile_board_key(rsmb, 4, efl_sha256, other) == 0);

	/* The round trip. */
	for (i = 0; i < sizeof(blob); i++) blob[i] = (unsigned char)(0x30 + i);
	n = tpm_statefile_write(file, sizeof(file), 5, nonce, key, blob, sizeof(blob), model_gcm);
	CHECK(n == sizeof(file) && !memcmp(file, "BCTS", 4) && memcmp(file + TPM_STATEFILE_HEADER_BYTES, blob, 16));
	CHECK(tpm_statefile_read(file, n, key, model_gcm, back, sizeof(back), &got, &seq) == TPM_STATEFILE_OK &&
	      got == sizeof(blob) && seq == 5u && !memcmp(back, blob, sizeof(blob)));
	/* Another key: the tag fails, and the buffer holds no plaintext. */
	memset(back, 0xEE, sizeof(back));
	CHECK(tpm_statefile_read(file, n, other, model_gcm, back, sizeof(back), &got, &seq) == TPM_STATEFILE_BAD_TAG);
	for (i = 0; i < sizeof(back); i++) bad |= back[i] != 0;
	CHECK(!bad);
	/* A buffer too small for the blob. */
	CHECK(tpm_statefile_read(file, n, key, model_gcm, back, sizeof(back) - 1u, &got, &seq) == TPM_STATEFILE_SMALL);

	/* Each byte is covered: a changed byte is never OK. The magic gives NONE, the frame fields BAD_FRAME, every
	 * other byte BAD_TAG (the header is the additional data, so the reserved bytes and the nonce count too). */
	for (i = 0; i < n; i++) {
		enum tpm_statefile_result want_result = TPM_STATEFILE_BAD_TAG, res;
		if (i < 4) want_result = TPM_STATEFILE_NONE;			/* the magic */
		else if (i < 8 || (i >= 16 && i < 20)) want_result = TPM_STATEFILE_BAD_FRAME;	/* version, sizes */
		file[i] ^= 0x01;
		res = tpm_statefile_read(file, n, key, model_gcm, back, sizeof(back), &got, &seq);
		file[i] ^= 0x01;
		if (res != want_result) { CHECK(!"one changed byte of the state file gave another result"); break; }
	}
	checks++;
	/* The frame: a file shorter or longer than its sizes, sequence 0. */
	CHECK(tpm_statefile_read(file, n - 1u, key, model_gcm, back, sizeof(back), &got, &seq) == TPM_STATEFILE_BAD_FRAME);
	CHECK(tpm_statefile_read(file, 10, key, model_gcm, back, sizeof(back), &got, &seq) == TPM_STATEFILE_NONE);
	memset(file + 8, 0, 8);
	CHECK(tpm_statefile_read(file, n, key, model_gcm, back, sizeof(back), &got, &seq) == TPM_STATEFILE_BAD_FRAME);
	/* The writer refuses sequence 0, an empty blob and a buffer too small. */
	CHECK(tpm_statefile_write(file, sizeof(file), 0, nonce, key, blob, sizeof(blob), model_gcm) == 0);
	CHECK(tpm_statefile_write(file, sizeof(file), 1, nonce, key, blob, 0, model_gcm) == 0);
	CHECK(tpm_statefile_write(file, sizeof(file) - 1u, 1, nonce, key, blob, sizeof(blob), model_gcm) == 0);

	/* The choice and the next write. */
	CHECK(tpm_statefile_choose(TPM_STATEFILE_OK, 4, TPM_STATEFILE_OK, 5) == 1);
	CHECK(tpm_statefile_choose(TPM_STATEFILE_OK, 6, TPM_STATEFILE_OK, 5) == 0);
	CHECK(tpm_statefile_choose(TPM_STATEFILE_OK, 5, TPM_STATEFILE_OK, 5) == 0);
	CHECK(tpm_statefile_choose(TPM_STATEFILE_BAD_TAG, 9, TPM_STATEFILE_OK, 2) == 1);
	CHECK(tpm_statefile_choose(TPM_STATEFILE_OK, 2, TPM_STATEFILE_NONE, 0) == 0);
	CHECK(tpm_statefile_choose(TPM_STATEFILE_BAD_FRAME, 0, TPM_STATEFILE_BAD_TAG, 0) == -1);
	CHECK(tpm_statefile_next(-1, 0, 0, &next) == 0 && next == 1u);
	CHECK(tpm_statefile_next(0, 7, 3, &next) == 1 && next == 8u);
	CHECK(tpm_statefile_next(1, 7, 9, &next) == 0 && next == 10u);
}

unsigned tl_tests(const char *real_log, const char *real_image, unsigned *check_count)
{
	checks = failures = 0;
	test_handoff();
	test_statefile();
	test_log_format();
	test_log_full();
	test_real_log(real_log);
	test_tpm2();
	test_measure();
	test_tcg2(real_image);
	*check_count = checks;
	return failures;
}
