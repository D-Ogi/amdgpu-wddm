/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The TPM 2.0 core of the servicer. The contract is in tpmcore.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "libtpms/tpm_library.h"
#include "libtpms/tpm_error.h"
#include "tpmcore.h"

#define TPM_RC_INITIALIZE	0x100u	/* TPM2_Startup after a successful one: the TPM already runs */

static wchar_t g_dir[MAX_PATH];

/* dir\name for a state blob name of libtpms ("permall", "volatilestate", "savestate"). */
static int blob_path(const char *name, const wchar_t *suffix, wchar_t *out, size_t cap)
{
	int n = _snwprintf(out, cap, L"%ls\\%hs%ls", g_dir, name, suffix);
	return n > 0 && (size_t)n < cap;
}

static TPM_RESULT nv_init(void) { return TPM_SUCCESS; }

/* libtpms frees the buffer with free(); it links into this image with the same C runtime. */
static TPM_RESULT nv_load(unsigned char **data, uint32_t *length, uint32_t tpm_number, const char *name)
{
	wchar_t path[MAX_PATH];
	HANDLE h;
	LARGE_INTEGER size;
	DWORD got = 0;

	(void)tpm_number;
	*data = NULL;
	*length = 0;
	if (!blob_path(name, L"", path, MAX_PATH)) return TPM_FAIL;
	h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return GetLastError() == ERROR_FILE_NOT_FOUND ? TPM_RETRY : TPM_FAIL;	/* RETRY: manufacture */
	if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 0x1000000) { CloseHandle(h); return TPM_FAIL; }
	*data = (unsigned char *)malloc((size_t)size.QuadPart);
	if (*data == NULL) { CloseHandle(h); return TPM_FAIL; }
	if (!ReadFile(h, *data, (DWORD)size.QuadPart, &got, NULL) || got != (DWORD)size.QuadPart) {
		CloseHandle(h);
		free(*data);
		*data = NULL;
		return TPM_FAIL;
	}
	CloseHandle(h);
	*length = got;
	return TPM_SUCCESS;
}

/* Write to name.tmp, flush, then replace: a power cut leaves the old blob or the new one, never half of one. */
static TPM_RESULT nv_store(const unsigned char *data, uint32_t length, uint32_t tpm_number, const char *name)
{
	wchar_t path[MAX_PATH], tmp[MAX_PATH];
	HANDLE h;
	DWORD put = 0;
	BOOL ok;

	(void)tpm_number;
	if (!blob_path(name, L"", path, MAX_PATH) || !blob_path(name, L".tmp", tmp, MAX_PATH)) return TPM_FAIL;
	h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return TPM_FAIL;
	ok = WriteFile(h, data, length, &put, NULL) && put == length && FlushFileBuffers(h);
	CloseHandle(h);
	if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(tmp);
		return TPM_FAIL;
	}
	return TPM_SUCCESS;
}

static TPM_RESULT nv_delete(uint32_t tpm_number, const char *name, TPM_BOOL must_exist)
{
	wchar_t path[MAX_PATH];
	(void)tpm_number;
	if (!blob_path(name, L"", path, MAX_PATH)) return TPM_FAIL;
	if (DeleteFileW(path)) return TPM_SUCCESS;
	return (!must_exist && GetLastError() == ERROR_FILE_NOT_FOUND) ? TPM_SUCCESS : TPM_FAIL;
}

static TPM_RESULT io_init(void) { return TPM_SUCCESS; }

/* Every command arrives through the locality 0 page (crb.h). */
static TPM_RESULT io_locality(TPM_MODIFIER_INDICATOR *locality, uint32_t tpm_number)
{
	(void)tpm_number;
	*locality = 0;
	return TPM_SUCCESS;
}

/* No physical presence signal exists on this platform. */
static TPM_RESULT io_presence(TPM_BOOL *present, uint32_t tpm_number)
{
	(void)tpm_number;
	*present = FALSE;
	return TPM_SUCCESS;
}

static unsigned be32(const unsigned char *p)
{
	return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | (unsigned)p[3];
}

/* The steps before _TPM_Init that both starts share. */
static TPM_RESULT prepare(const wchar_t *state_dir, unsigned max_buffer)
{
	static struct libtpms_callbacks cb = {
		sizeof(struct libtpms_callbacks), nv_init, nv_load, nv_store, nv_delete, io_init, io_locality, io_presence,
	};
	uint32_t min_size = 0, max_size = 0;
	TPM_RESULT r;

	if (wcslen(state_dir) + 32 >= MAX_PATH) return TPM_FAIL;
	wcscpy(g_dir, state_dir);
	if (!CreateDirectoryW(g_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return TPM_FAIL;
	/* First: a change of the TPM version clears the cached states of libtpms. */
	r = TPMLIB_ChooseTPMVersion(TPMLIB_TPM_VERSION_2);
	if (r != TPM_SUCCESS) return r;
	r = TPMLIB_RegisterCallbacks(&cb);
	if (r != TPM_SUCCESS) return r;
	return TPMLIB_SetBufferSize(max_buffer, &min_size, &max_size) == max_buffer ? TPM_SUCCESS : TPM_FAIL;
}

unsigned tpmcore_resume(const wchar_t *state_dir, unsigned max_buffer, const unsigned char *permanent,
			unsigned permanent_bytes, const unsigned char *volatile_state, unsigned volatile_bytes)
{
	TPM_RESULT r = prepare(state_dir, max_buffer);
	if (r != TPM_SUCCESS) return r;
	/* The permanent state first: TPMLIB_SetState(VOLATILE) reads the permanent state to check the volatile one.
	 * TPMLIB_MainInit then takes both from the cache instead of nv_load, and the TPM runs on without Startup. */
	r = TPMLIB_SetState(TPMLIB_STATE_PERMANENT, permanent, permanent_bytes);
	if (r == TPM_SUCCESS) r = TPMLIB_SetState(TPMLIB_STATE_VOLATILE, volatile_state, volatile_bytes);
	if (r == TPM_SUCCESS) r = TPMLIB_MainInit();
	return r;
}

unsigned tpmcore_start(const wchar_t *state_dir, unsigned max_buffer)
{
	/* TPM2_Startup(TPM_SU_CLEAR): the platform's duty at every TPM reset, which a start of this core is. */
	static const unsigned char startup[12] = {0x80, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x01, 0x44, 0x00, 0x00};
	unsigned char rsp[16];
	TPM_RESULT r;
	size_t n;

	r = prepare(state_dir, max_buffer);
	if (r != TPM_SUCCESS) return r;
	r = TPMLIB_MainInit();
	if (r != TPM_SUCCESS) return r;
	n = tpmcore_process(startup, sizeof(startup), rsp, sizeof(rsp));
	if (n < 10) return TPM_FAIL;
	r = be32(rsp + 6);
	return (r == 0 || r == TPM_RC_INITIALIZE) ? 0 : r;
}

size_t tpmcore_process(const unsigned char *command, size_t command_bytes, unsigned char *response, size_t capacity)
{
	unsigned char *out = NULL;
	uint32_t out_bytes = 0, out_cap = 0;
	unsigned char *in;
	size_t n = 0;

	/* TPMLIB_Process takes a non-const buffer; give it a copy so the caller's command stays as it was. */
	in = (unsigned char *)malloc(command_bytes);
	if (in == NULL) return 0;
	memcpy(in, command, command_bytes);
	if (TPMLIB_Process(&out, &out_bytes, &out_cap, in, (uint32_t)command_bytes) == TPM_SUCCESS && out != NULL &&
	    out_bytes >= 10 && out_bytes <= capacity) {
		memcpy(response, out, out_bytes);
		n = out_bytes;
	}
	free(in);
	free(out);
	return n;
}

void tpmcore_stop(void)
{
	/* TPM2_Shutdown(TPM_SU_CLEAR): the next start is a TPM2_Startup(CLEAR) in any case, and an orderly shutdown
	 * keeps the dictionary-attack logic from counting every service stop as an unorderly one. If tpm.sys sent
	 * its own shutdown already, the answer is an error code and nothing changes. */
	static const unsigned char shutdown[12] = {0x80, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x01, 0x45, 0x00, 0x00};
	unsigned char rsp[16];
	(void)tpmcore_process(shutdown, sizeof(shutdown), rsp, sizeof(rsp));
	TPMLIB_Terminate();
}
