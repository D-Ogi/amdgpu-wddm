/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * tpm_pcrs: reads the 24 SHA-256 PCRs of the TPM of this Windows through TBS, and with a TCG log file replays the
 * log and says for each PCR whether the replay gives the value of the TPM.
 *
 *   tpm_pcrs.exe                      the PCR values
 *   tpm_pcrs.exe <log>                the values, the replay and the comparison (exit 0 when PCR 0-7 match)
 *   tpm_pcrs.exe --replay <log>       only the replay, no TPM (exit 0 when the log reads)
 *
 * The Windows copy of the log of the current boot is the newest file of C:\Windows\Logs\MeasuredBoot. It reads
 * only; it sends TPM2_PCR_Read and nothing else.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tbs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tl_log.h"
#include "tl_tpm2.h"

#ifndef TBS_CONTEXT_VERSION_TWO
/* The llvm-mingw tbs.h has only version 1 of the parameters. The layout of the Windows SDK tbs.h. */
#define TBS_CONTEXT_VERSION_TWO 2
typedef struct {
	UINT32 version;
	union {
		struct {
			UINT32 requestRaw : 1;
			UINT32 includeTpm12 : 1;
			UINT32 includeTpm20 : 1;
		};
		UINT32 asUINT32;
	};
} TBS_CONTEXT_PARAMS2;
#endif

static int read_pcrs(unsigned char pcrs[TL_TPM2_PCRS][TL_TPM2_SHA256_BYTES])
{
	TBS_CONTEXT_PARAMS2 params;
	TBS_HCONTEXT context = 0;
	unsigned first;
	memset(&params, 0, sizeof(params));
	params.version = TBS_CONTEXT_VERSION_TWO;
	params.includeTpm20 = 1;
	if (Tbsi_Context_Create((PCTBS_CONTEXT_PARAMS)&params, &context) != TBS_SUCCESS) {
		fprintf(stderr, "tpm_pcrs: no TBS context (no TPM 2.0?)\n");
		return 0;
	}
	for (first = 0; first < TL_TPM2_PCRS; first += 8u) {
		unsigned char cmd[64], rsp[1024];
		UINT32 rsp_bytes = sizeof(rsp);
		unsigned got = 0, rc;
		size_t n = tl_tpm2_pcr_read(cmd, sizeof(cmd), first, 8u);
		if (Tbsip_Submit_Command(context, TBS_COMMAND_LOCALITY_ZERO, TBS_COMMAND_PRIORITY_NORMAL, cmd, (UINT32)n,
					 rsp, &rsp_bytes) != TBS_SUCCESS) {
			fprintf(stderr, "tpm_pcrs: TBS submit failed\n");
			Tbsip_Context_Close(context);
			return 0;
		}
		rc = tl_tpm2_pcr_read_answer(rsp, rsp_bytes, first, pcrs, &got);
		if (rc != 0 || got != 8u) {
			fprintf(stderr, "tpm_pcrs: PCR_Read %u..%u: rc 0x%x, %u digests\n", first, first + 7u, rc, got);
			Tbsip_Context_Close(context);
			return 0;
		}
	}
	Tbsip_Context_Close(context);
	return 1;
}

static unsigned char *read_file(const char *path, size_t *bytes)
{
	FILE *f = fopen(path, "rb");
	unsigned char *data;
	long size;
	if (f == NULL) return NULL;
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	data = malloc((size_t)size);
	if (data != NULL && fread(data, 1, (size_t)size, f) != (size_t)size) {
		free(data);
		data = NULL;
	}
	fclose(f);
	*bytes = (size_t)size;
	return data;
}

static void hex(const unsigned char *d)
{
	unsigned i;
	for (i = 0; i < TL_TPM2_SHA256_BYTES; i++) printf("%02x", d[i]);
}

int main(int argc, char **argv)
{
	unsigned char tpm[TL_TPM2_PCRS][TL_TPM2_SHA256_BYTES], replay[24][TL_SHA256_BYTES];
	const char *log_path = NULL;
	int replay_only = 0, have_tpm = 0, have_log = 0, events = 0, mismatch = 0;
	unsigned i;

	if (argc == 3 && !strcmp(argv[1], "--replay")) {
		replay_only = 1;
		log_path = argv[2];
	} else if (argc == 2) {
		log_path = argv[1];
	} else if (argc != 1) {
		fprintf(stderr, "usage: tpm_pcrs [--replay] [log]\n");
		return 2;
	}
	if (log_path != NULL) {
		size_t bytes = 0;
		unsigned char *log = read_file(log_path, &bytes);
		if (log == NULL) {
			fprintf(stderr, "tpm_pcrs: %s does not read\n", log_path);
			return 2;
		}
		events = tl_log_replay(log, bytes, replay);
		free(log);
		if (events < 0) {
			fprintf(stderr, "tpm_pcrs: %s is not a crypto-agile TCG log with a SHA-256 bank\n", log_path);
			return 1;
		}
		have_log = 1;
		printf("log: %d events\n", events);
	}
	if (!replay_only) {
		if (!read_pcrs(tpm)) return 1;
		have_tpm = 1;
	}
	for (i = 0; i < TL_TPM2_PCRS; i++) {
		printf("PCR %2u ", i);
		if (have_tpm) hex(tpm[i]);
		if (have_log) {
			printf(have_tpm ? "  replay " : "replay ");
			hex(replay[i]);
			if (have_tpm) {
				int same = !memcmp(tpm[i], replay[i], TL_TPM2_SHA256_BYTES);
				printf(same ? "  same" : "  DIFFERENT");
				if (!same && i < 8u) mismatch++;
			}
		}
		printf("\n");
	}
	if (have_tpm && have_log) printf("PCR 0-7: %s\n", mismatch ? "the replay differs" : "the replay matches the TPM");
	return mismatch ? 1 : 0;
}
