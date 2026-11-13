/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * A small store of read-only UEFI variables that live in RAM, and the three decisions of the variable services
 * that serve it: get, next name, and "is this name ours". It is the pure part of the runtime driver
 * (secure-boot/rt/sbvar_rt.c), so the host tests run it.
 *
 * The store holds no pointer, only offsets. The stage fills one in pool memory and the runtime driver copies it
 * as a block. After SetVirtualAddressMap the same bytes are valid at the new address.
 *
 * Nothing here writes a firmware variable. The store has no "set".
 */
#ifndef SB_VARS_H
#define SB_VARS_H
#include <stddef.h>
#include "efi.h"

#define SB_VARS_MAGIC		0x52415653u	/* "SVAR" */
#define SB_VARS_MAX		8u
#define SB_VARS_NAME_CHARS	24u		/* with the terminator */
#define SB_VARS_DATA_BYTES	(256u * 1024u)

/* The attribute bits of a variable (UEFI Specification, GetVariable). The store uses only the two volatile
 * ones: it never states that a variable is non-volatile or authenticated, because it is neither. */
#define SB_VAR_BOOTSERVICE_ACCESS	0x00000002u
#define SB_VAR_RUNTIME_ACCESS		0x00000004u

struct sb_var {
	EFI_GUID guid;
	CHAR16 name[SB_VARS_NAME_CHARS];
	UINT32 attributes;
	UINT32 offset, bytes;		/* in data */
};

struct sb_var_store {
	UINT32 magic;
	UINT32 count;
	UINT32 used;			/* bytes of data in use */
	struct sb_var vars[SB_VARS_MAX];
	UINT8 data[SB_VARS_DATA_BYTES];
};

void sb_vars_init(struct sb_var_store *store);
/* 1 when the variable is in the store. 0 when there is no room, the name is too long or it is there already. */
int sb_vars_add(struct sb_var_store *store, const EFI_GUID *guid, const CHAR16 *name, UINT32 attributes,
		const void *data, size_t bytes);
/* 1 when the block is a store whose counts and offsets are sound. */
int sb_vars_sound(const struct sb_var_store *store);

/* The index of the variable, or -1. */
int sb_vars_find(const struct sb_var_store *store, const CHAR16 *name, const EFI_GUID *guid);

/* GetVariable for the variable at index, with the status and size rules of the UEFI Specification. */
EFI_STATUS sb_vars_get(const struct sb_var_store *store, int index, UINT32 *attributes, UINTN *size, void *data);

/* GetNextVariableName over the store and then the firmware's variables. The store's names come first.
 *
 * The caller's side of the contract: the firmware has no variable with a name and GUID of the store. The Secure
 * Boot module checks that before it starts the runtime driver, and the driver refuses every later write to such
 * a name, so the firmware cannot get one. With that, a name belongs to exactly one of the two lists. */
typedef EFI_STATUS (EFIAPI *sb_vars_next_fn)(UINTN *size, CHAR16 *name, EFI_GUID *guid);
EFI_STATUS sb_vars_next(const struct sb_var_store *store, sb_vars_next_fn firmware, UINTN *size, CHAR16 *name,
			EFI_GUID *guid);

#endif
