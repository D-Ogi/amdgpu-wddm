/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The writer of the state files (core/espstate.c) in a directory of the host: the A/B order, the sequence after a
 * new open, a damaged newer copy, no write of an unchanged state, and the files in the format that the stage reads
 * (statefile.h with the same OpenSSL GCM). It does not touch an EFI system partition: espstate_open_at takes the
 * directory and the key.
 *
 *   espstate_test.exe <empty directory>
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/espstate.h"
#include "../statefile.h"
#include "../gcm_openssl.h"

static int failures, checks;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static wchar_t g_dir[MAX_PATH];

static unsigned char *load(const wchar_t *name, size_t *bytes)
{
	wchar_t path[MAX_PATH];
	FILE *f;
	unsigned char *p;
	long n;
	_snwprintf(path, MAX_PATH, L"%ls\\%ls", g_dir, name);
	f = _wfopen(path, L"rb");
	if (f == NULL) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	p = (unsigned char *)malloc((size_t)n);
	*bytes = fread(p, 1, (size_t)n, f);
	fclose(f);
	return p;
}

/* The copy as the stage reads it: its result, sequence and blob. */
static enum tpm_statefile_result stage_read(const wchar_t *name, const unsigned char key[32], unsigned char *blob,
					    size_t *blob_bytes, unsigned long long *sequence)
{
	size_t n = 0;
	unsigned char *file = load(name, &n);
	enum tpm_statefile_result r;
	if (file == NULL) return TPM_STATEFILE_NONE;
	r = tpm_statefile_read(file, n, key, tpm_gcm_openssl, blob, TPM_STATEFILE_MAX_BLOB, blob_bytes, sequence);
	free(file);
	return r;
}

static void flip(const wchar_t *name, size_t at)
{
	wchar_t path[MAX_PATH];
	FILE *f;
	int c;
	_snwprintf(path, MAX_PATH, L"%ls\\%ls", g_dir, name);
	f = _wfopen(path, L"r+b");
	if (f == NULL) return;
	fseek(f, (long)at, SEEK_SET);
	c = fgetc(f);
	fseek(f, (long)at, SEEK_SET);
	fputc(c ^ 1, f);
	fclose(f);
}

int wmain(int argc, wchar_t **argv)
{
	static unsigned char blob[TPM_STATEFILE_MAX_BLOB];
	unsigned char key[32], other[32], one[300], two[200];
	struct espstate e;
	unsigned long long seq = 0;
	unsigned long error = 0;
	size_t n = 0, i;

	if (argc != 2) { printf("usage: espstate_test <empty directory>\n"); return 2; }
	_snwprintf(g_dir, MAX_PATH, L"%ls\\tpm", argv[1]);
	for (i = 0; i < 32; i++) { key[i] = (unsigned char)(0xA0 + i); other[i] = key[i]; }
	other[5] ^= 1;
	for (i = 0; i < sizeof(one); i++) one[i] = (unsigned char)i;
	for (i = 0; i < sizeof(two); i++) two[i] = (unsigned char)(0xFF - i);

	/* An empty directory: the first write is A with sequence 1. The directory is made. */
	CHECK(espstate_open_at(&e, g_dir, key) == ESPSTATE_OK && e.next_slot == 0 && e.next_sequence == 1u);
	CHECK(espstate_written_sequence(&e) == 0u);
	CHECK(espstate_write(&e, one, sizeof(one), &error) == ESPSTATE_WRITTEN && espstate_written_sequence(&e) == 1u);
	CHECK(stage_read(L"state-a.bin", key, blob, &n, &seq) == TPM_STATEFILE_OK && seq == 1u && n == sizeof(one) &&
	      !memcmp(blob, one, n));
	/* The second write goes to B and does not touch A. */
	CHECK(espstate_write(&e, two, sizeof(two), &error) == ESPSTATE_WRITTEN && espstate_written_sequence(&e) == 2u);
	CHECK(stage_read(L"state-b.bin", key, blob, &n, &seq) == TPM_STATEFILE_OK && seq == 2u && n == sizeof(two) &&
	      !memcmp(blob, two, n));
	CHECK(stage_read(L"state-a.bin", key, blob, &n, &seq) == TPM_STATEFILE_OK && seq == 1u);
	/* The same state again is not written: no file changes, and the next write still goes to A with 3. */
	CHECK(espstate_write(&e, two, sizeof(two), &error) == ESPSTATE_UNCHANGED && espstate_written_sequence(&e) == 2u);
	CHECK(e.next_slot == 0 && e.next_sequence == 3u);
	CHECK(stage_read(L"state-a.bin", key, blob, &n, &seq) == TPM_STATEFILE_OK && seq == 1u);
	espstate_close(&e);
	CHECK(!e.open && e.key[0] == 0);

	/* A new open after a restart: B (2) is the state, the next write is A with 3. The state of B itself is not
	 * written again (a resume stores the state that it was given), another state is. */
	CHECK(espstate_open_at(&e, g_dir, key) == ESPSTATE_OK && e.next_slot == 0 && e.next_sequence == 3u);
	CHECK(espstate_write(&e, two, sizeof(two), &error) == ESPSTATE_UNCHANGED && e.next_sequence == 3u);
	CHECK(espstate_write(&e, one, sizeof(one), &error) == ESPSTATE_WRITTEN && espstate_written_sequence(&e) == 3u);
	CHECK(stage_read(L"state-a.bin", key, blob, &n, &seq) == TPM_STATEFILE_OK && seq == 3u && n == sizeof(one));
	espstate_close(&e);

	/* A damaged (a write that a power cut broke): B (2) is the state, the next write repairs A with 3. */
	flip(L"state-a.bin", 60);
	CHECK(stage_read(L"state-a.bin", key, blob, &n, &seq) == TPM_STATEFILE_BAD_TAG);
	CHECK(espstate_open_at(&e, g_dir, key) == ESPSTATE_OK && e.next_slot == 0 && e.next_sequence == 3u);
	CHECK(espstate_write(&e, one, sizeof(one), &error) == ESPSTATE_WRITTEN);
	CHECK(stage_read(L"state-a.bin", key, blob, &n, &seq) == TPM_STATEFILE_OK && seq == 3u);
	espstate_close(&e);

	/* Another key opens neither copy: the stage gives no TPM then, and the reader of this side sees no state. */
	CHECK(stage_read(L"state-a.bin", other, blob, &n, &seq) == TPM_STATEFILE_BAD_TAG);
	CHECK(espstate_open_at(&e, g_dir, other) == ESPSTATE_OK && e.next_slot == 0 && e.next_sequence == 1u);
	espstate_close(&e);

	SecureZeroMemory(blob, sizeof(blob));
	printf("%s: espstate %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
	return failures ? 1 : 0;
}
