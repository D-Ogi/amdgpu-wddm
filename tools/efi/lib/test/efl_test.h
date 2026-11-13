/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* What every host test file of tools/efi shares: the check counter and a file reader. Each test file includes
 * this header once and gives its counts to the runner (stage/test/host_main.c). */
#ifndef EFL_TEST_H
#define EFL_TEST_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* Reads a file into memory that the caller frees: dir/name, or name alone when dir is "". NULL when the file is
 * not there or no directory is given. */
__attribute__((unused)) static unsigned char *efl_test_load(const char *dir, const char *name, size_t *bytes)
{
	char path[1024];
	unsigned char *data;
	long size;
	FILE *f;
	*bytes = 0;
	if (dir == NULL || name == NULL) return NULL;
	snprintf(path, sizeof(path), "%s%s%s", dir, dir[0] ? "/" : "", name);
	f = fopen(path, "rb");
	if (f == NULL) return NULL;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = size > 0 ? malloc((size_t)size) : NULL;
	if (data != NULL && fread(data, 1, (size_t)size, f) == (size_t)size) *bytes = (size_t)size;
	fclose(f);
	if (*bytes == 0) {
		free(data);
		return NULL;
	}
	return data;
}

#endif
