/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The RAM variable store (sb_vars.h). This code also runs after ExitBootServices: it calls nothing but memcpy
 * and memcmp and the firmware function that the caller gives it. */
#include <stddef.h>
#include <string.h>
#include "sb_vars.h"

static size_t name_chars(const CHAR16 *name, size_t limit)
{
	size_t n = 0;
	while (n < limit && name[n]) n++;
	return n;
}

static int name_is(const CHAR16 *stored, const CHAR16 *name)
{
	size_t i;
	for (i = 0; i < SB_VARS_NAME_CHARS; i++) {
		if (stored[i] != name[i]) return 0;
		if (!stored[i]) return 1;
	}
	return 0;
}

void sb_vars_init(struct sb_var_store *store)
{
	memset(store, 0, sizeof(*store));
	store->magic = SB_VARS_MAGIC;
}

int sb_vars_sound(const struct sb_var_store *store)
{
	UINT32 i;
	if (store->magic != SB_VARS_MAGIC || store->count > SB_VARS_MAX || store->used > SB_VARS_DATA_BYTES) return 0;
	for (i = 0; i < store->count; i++) {
		const struct sb_var *v = &store->vars[i];
		if (v->offset > store->used || v->bytes > store->used - v->offset) return 0;
		if (name_chars(v->name, SB_VARS_NAME_CHARS) == SB_VARS_NAME_CHARS || !v->name[0]) return 0;
	}
	return 1;
}

int sb_vars_find(const struct sb_var_store *store, const CHAR16 *name, const EFI_GUID *guid)
{
	UINT32 i;
	for (i = 0; i < store->count; i++)
		if (name_is(store->vars[i].name, name) && !memcmp(&store->vars[i].guid, guid, sizeof(*guid))) return (int)i;
	return -1;
}

int sb_vars_add(struct sb_var_store *store, const EFI_GUID *guid, const CHAR16 *name, UINT32 attributes,
		const void *data, size_t bytes)
{
	struct sb_var *v;
	size_t chars = name_chars(name, SB_VARS_NAME_CHARS);
	if (chars == 0 || chars == SB_VARS_NAME_CHARS || store->count >= SB_VARS_MAX) return 0;
	if (bytes > SB_VARS_DATA_BYTES - store->used || sb_vars_find(store, name, guid) >= 0) return 0;
	v = &store->vars[store->count++];
	memset(v, 0, sizeof(*v));
	v->guid = *guid;
	memcpy(v->name, name, chars * sizeof(CHAR16));
	v->attributes = attributes;
	v->offset = store->used;
	v->bytes = (UINT32)bytes;
	if (bytes) memcpy(store->data + store->used, data, bytes);
	store->used += (UINT32)bytes;
	return 1;
}

EFI_STATUS sb_vars_get(const struct sb_var_store *store, int index, UINT32 *attributes, UINTN *size, void *data)
{
	const struct sb_var *v = &store->vars[index];
	if (size == NULL) return EFI_INVALID_PARAMETER;
	/* The attributes are given also with EFI_BUFFER_TOO_SMALL, as the specification allows since 2.8. */
	if (attributes) *attributes = v->attributes;
	if (*size < v->bytes) {
		*size = v->bytes;
		return EFI_BUFFER_TOO_SMALL;
	}
	if (data == NULL && v->bytes) return EFI_INVALID_PARAMETER;
	if (v->bytes) memcpy(data, store->data + v->offset, v->bytes);
	*size = v->bytes;
	return EFI_SUCCESS;
}

/* Puts the name of the variable at index into the caller's buffer. */
static EFI_STATUS give(const struct sb_var_store *store, UINT32 index, UINTN *size, CHAR16 *name, EFI_GUID *guid)
{
	const struct sb_var *v = &store->vars[index];
	UINTN need = (name_chars(v->name, SB_VARS_NAME_CHARS) + 1u) * sizeof(CHAR16);
	if (*size < need) {
		*size = need;
		return EFI_BUFFER_TOO_SMALL;
	}
	memcpy(name, v->name, need);
	*guid = v->guid;
	*size = need;
	return EFI_SUCCESS;
}

EFI_STATUS sb_vars_next(const struct sb_var_store *store, sb_vars_next_fn firmware, UINTN *size, CHAR16 *name,
			EFI_GUID *guid)
{
	EFI_STATUS status;
	int index;
	if (size == NULL || name == NULL || guid == NULL || *size < sizeof(CHAR16)) return EFI_INVALID_PARAMETER;
	if (!name[0] && store->count) return give(store, 0, size, name, guid);
	index = name[0] ? sb_vars_find(store, name, guid) : -1;
	if (index >= 0 && (UINT32)index + 1u < store->count) return give(store, (UINT32)index + 1u, size, name, guid);
	if (index >= 0) {
		/* After the last name of the store: the first name of the firmware. The caller's buffer is put back
		 * when the firmware gives no name, so that a second call with a larger buffer starts at the same
		 * place. */
		CHAR16 first = name[0];
		EFI_GUID saved = *guid;
		name[0] = 0;
		status = firmware(size, name, guid);
		if (EFI_ERROR(status)) {
			name[0] = first;
			*guid = saved;
			return status;
		}
		return EFI_SUCCESS;
	}
	return firmware(size, name, guid);
}
