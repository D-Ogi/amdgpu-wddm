/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The state files of the pre-OS TPM on the EFI system partition. The contract is in espstate.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <bcrypt.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#include "espstate.h"
#include "../statefile.h"
#include "../gcm_openssl.h"

static const wchar_t *const names[2] = { L"state-a.bin", L"state-b.bin" };

static void sha256(const void *data, size_t bytes, unsigned char digest[32])
{
	if (SHA256((const unsigned char *)data, bytes, digest) == NULL) memset(digest, 0, 32);
}

/* The volume name (\\?\Volume{...}\) of the GPT partition with this partition GUID, into out. */
static int find_volume(const unsigned char guid[16], wchar_t *out, size_t cap)
{
	wchar_t name[MAX_PATH];
	HANDLE find = FindFirstVolumeW(name, MAX_PATH);
	int found = 0;

	if (find == INVALID_HANDLE_VALUE) return 0;
	do {
		size_t n = wcslen(name);
		PARTITION_INFORMATION_EX info;
		DWORD got = 0;
		HANDLE v;
		if (n < 2 || name[n - 1] != L'\\') continue;
		name[n - 1] = 0;	/* the device, not its root directory */
		v = CreateFileW(name, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
		name[n - 1] = L'\\';
		if (v == INVALID_HANDLE_VALUE) continue;
		if (DeviceIoControl(v, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0, &info, sizeof(info), &got, NULL) &&
		    info.PartitionStyle == PARTITION_STYLE_GPT && memcmp(&info.Gpt.PartitionId, guid, 16) == 0 && n < cap) {
			wcscpy(out, name);
			found = 1;
		}
		CloseHandle(v);
	} while (!found && FindNextVolumeW(find, name, MAX_PATH));
	FindVolumeClose(find);
	return found;
}

static int board_key(unsigned char key[32])
{
	UINT n = GetSystemFirmwareTable('RSMB', 0, NULL, 0);
	unsigned char *t;
	int ok;
	if (n == 0) return 0;
	t = (unsigned char *)malloc(n);
	if (t == NULL) return 0;
	ok = GetSystemFirmwareTable('RSMB', 0, t, n) == n && tpm_statefile_board_key(t, n, sha256, key);
	SecureZeroMemory(t, n);
	free(t);
	return ok;
}

static int path_of(const struct espstate *e, int slot, wchar_t *out, size_t cap)
{
	int n = _snwprintf(out, cap, L"%ls\\%ls", e->dir, names[slot]);
	return n > 0 && (size_t)n < cap;
}

/* The result and sequence of one copy, with the blob into scratch (wiped by the caller). */
static enum tpm_statefile_result read_copy(const struct espstate *e, int slot, unsigned char *scratch,
					   unsigned long long *sequence)
{
	wchar_t path[MAX_PATH];
	unsigned char *file;
	LARGE_INTEGER size;
	DWORD got = 0;
	HANDLE h;
	size_t blob = 0;
	enum tpm_statefile_result r = TPM_STATEFILE_NONE;

	*sequence = 0;
	if (!path_of(e, slot, path, MAX_PATH)) return TPM_STATEFILE_NONE;
	h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) return TPM_STATEFILE_NONE;
	if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > TPM_STATEFILE_MAX_BYTES) {
		CloseHandle(h);
		return TPM_STATEFILE_BAD_FRAME;
	}
	file = (unsigned char *)malloc((size_t)size.QuadPart);
	if (file != NULL && ReadFile(h, file, (DWORD)size.QuadPart, &got, NULL) && got == (DWORD)size.QuadPart)
		r = tpm_statefile_read(file, got, e->key, tpm_gcm_openssl, scratch, TPM_STATEFILE_MAX_BLOB, &blob, sequence);
	else
		r = TPM_STATEFILE_BAD_FRAME;
	CloseHandle(h);
	free(file);
	return r;
}

enum espstate_result espstate_open(struct espstate *e, const unsigned char partition_guid[16])
{
	wchar_t volume[MAX_PATH], dir[MAX_PATH];
	unsigned char key[32];
	enum espstate_result r;

	memset(e, 0, sizeof(*e));
	if (!find_volume(partition_guid, volume, MAX_PATH)) return ESPSTATE_NO_VOLUME;
	if (!board_key(key)) return ESPSTATE_NO_KEY;
	/* volume ends with a backslash */
	_snwprintf(dir, MAX_PATH, L"%lsEFI\\BC250\\tpm", volume);
	r = espstate_open_at(e, dir, key);
	SecureZeroMemory(key, sizeof(key));
	return r;
}

enum espstate_result espstate_open_at(struct espstate *e, const wchar_t *dir, const unsigned char key[32])
{
	unsigned long long seq[2];
	enum tpm_statefile_result r[2];
	unsigned char *scratch;
	int slot;

	memset(e, 0, sizeof(*e));
	if (wcslen(dir) + 16 >= sizeof(e->dir) / sizeof(e->dir[0])) return ESPSTATE_NO_DIRECTORY;
	wcscpy(e->dir, dir);
	memcpy(e->key, key, sizeof(e->key));
	if (!CreateDirectoryW(e->dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
		espstate_close(e);
		return ESPSTATE_NO_DIRECTORY;
	}
	scratch = (unsigned char *)malloc(TPM_STATEFILE_MAX_BLOB);
	for (slot = 0; slot < 2; slot++) {
		r[slot] = scratch != NULL ? read_copy(e, slot, scratch, &seq[slot]) : TPM_STATEFILE_NONE;
		if (r[slot] != TPM_STATEFILE_OK) seq[slot] = 0;
	}
	if (scratch != NULL) {
		SecureZeroMemory(scratch, TPM_STATEFILE_MAX_BLOB);
		free(scratch);
	}
	e->next_slot = tpm_statefile_next(tpm_statefile_choose(r[0], seq[0], r[1], seq[1]), seq[0], seq[1],
					  &e->next_sequence);
	e->open = 1;
	return ESPSTATE_OK;
}

int espstate_write(struct espstate *e, const unsigned char *blob, size_t bytes, unsigned long *error)
{
	wchar_t path[MAX_PATH];
	unsigned char nonce[12];
	unsigned char *file;
	size_t n, cap = TPM_STATEFILE_HEADER_BYTES + bytes + TPM_STATEFILE_TAG_BYTES;
	DWORD put = 0;
	HANDLE h;
	int ok;

	*error = 0;
	if (!e->open || !path_of(e, e->next_slot, path, MAX_PATH)) return 0;
	/* A random nonce for each write: 96 bits, so a repeat under one key is not a practical event. */
	if (BCryptGenRandom(NULL, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return 0;
	file = (unsigned char *)malloc(cap);
	if (file == NULL) return 0;
	n = tpm_statefile_write(file, cap, e->next_sequence, nonce, e->key, blob, bytes, tpm_gcm_openssl);
	if (n == 0) {
		free(file);
		return 0;
	}
	/* In place: a write that a power cut breaks leaves this copy unsound and the other copy as the state. */
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	ok = h != INVALID_HANDLE_VALUE;
	if (ok) {
		ok = WriteFile(h, file, (DWORD)n, &put, NULL) && put == n && FlushFileBuffers(h);
		if (!ok) *error = GetLastError();
		CloseHandle(h);
	} else {
		*error = GetLastError();
	}
	free(file);
	if (!ok) return 0;
	e->written = e->next_sequence;
	e->next_slot ^= 1;
	e->next_sequence++;
	return 1;
}

unsigned long long espstate_written_sequence(const struct espstate *e)
{
	return e->written;
}

void espstate_close(struct espstate *e)
{
	SecureZeroMemory(e, sizeof(*e));
}
