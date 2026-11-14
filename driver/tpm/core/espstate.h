/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The state files of the pre-OS TPM on the EFI system partition, from the side of the Windows core: the core is the
 * only writer (docs/design/tpm-preos.md, "The state on the EFI system partition"). The format is
 * driver/tpm/statefile.h; the stage reads the same two files.
 *
 * The volume is the GPT partition that the handoff record names (the volume of the stage). The files are
 * \EFI\BC250\tpm\state-a.bin and state-b.bin on it, beside the stage image \EFI\BC250\bc250-stage.efi.
 */
#ifndef BC250_ESPSTATE_H
#define BC250_ESPSTATE_H
#include <stddef.h>
#include <wchar.h>

struct espstate {
	wchar_t dir[300];		/* \\?\Volume{...}\EFI\BC250\tpm */
	unsigned char key[32];		/* the board key; zeros after espstate_close */
	int next_slot;			/* 0: A, 1: B */
	unsigned long long next_sequence;
	unsigned long long written;	/* the sequence of the last write, 0 before any */
	unsigned char state[32];	/* SHA-256 of the state in the newest sound copy, when have_state */
	int have_state;
	int open;
};

enum espstate_write_result {
	ESPSTATE_WRITTEN = 0,	/* the next copy holds the blob */
	ESPSTATE_UNCHANGED,	/* the newest copy holds this blob already: no file is written */
	ESPSTATE_WRITE_FAILED	/* *error has the Win32 error, or 0 when the encryption failed */
};

enum espstate_result {
	ESPSTATE_OK = 0,
	ESPSTATE_NO_VOLUME,	/* no volume has that GPT partition GUID */
	ESPSTATE_NO_KEY,	/* the SMBIOS table gives no board key */
	ESPSTATE_NO_DIRECTORY	/* \EFI\BC250\tpm does not exist and cannot be made */
};

/* Finds the volume, derives the key, and reads both copies to find where the next write goes (the copy that is not
 * the state, with the next sequence). A copy that does not open counts as no copy here: the stage refused to start
 * from it, so a handoff that comes with it is from a new TPM. */
enum espstate_result espstate_open(struct espstate *e, const unsigned char partition_guid[16]);

/* The same in a given directory with a given key: the second half of espstate_open, and the entry of the host test. */
enum espstate_result espstate_open_at(struct espstate *e, const wchar_t *dir, const unsigned char key[32]);

/* Writes the permanent state of libtpms to the next copy and flushes it. After ESPSTATE_WRITTEN the next write goes
 * to the other copy. A blob that the newest sound copy holds already (a store that left the state as it was, or the
 * state that the stage read) gives ESPSTATE_UNCHANGED and no write: each write is a flushed write on the partition. */
enum espstate_write_result espstate_write(struct espstate *e, const unsigned char *blob, size_t bytes,
					  unsigned long *error);

/* The sequence of the copy that the last espstate_write wrote, 0 before any write. */
unsigned long long espstate_written_sequence(const struct espstate *e);

void espstate_close(struct espstate *e);

#endif
