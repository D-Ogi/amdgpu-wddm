/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * A reader for a signature database: one or more EFI_SIGNATURE_LIST structures, one after the other, as the UEFI
 * Specification defines them for the db and dbx variables (the structure and the two type GUIDs were compared
 * with edk2 MdePkg/Include/Guid/ImageAuthentication.h, commit 8628578dacd3). The stage keeps its databases as
 * files in this format, so the standard tools make them.
 *
 * Three entry types are read: an X.509 certificate (EFI_CERT_X509_GUID), a SHA-256 image digest
 * (EFI_CERT_SHA256_GUID), and the SHA-256 digest of the signed part of a certificate with a revocation time
 * (EFI_CERT_X509_SHA256_GUID), which the forbidden database of a PC uses. An entry of another type is counted, so
 * that the caller can say that it did not read it.
 */
#ifndef SB_SIGLIST_H
#define SB_SIGLIST_H
#include <stddef.h>
#include "sb_der.h"

enum sb_sig_type { SB_SIG_OTHER = 0, SB_SIG_X509, SB_SIG_SHA256, SB_SIG_X509_SHA256 };
/* The data of an SB_SIG_X509_SHA256 entry: the digest, then an EFI_TIME of 16 bytes. */
#define SB_X509_SHA256_DATA_BYTES	48u

struct sb_sig_cursor {
	struct sb_span rest;		/* the lists not yet opened */
	struct sb_span entries;		/* the entries left in the open list */
	size_t entry_bytes;
	enum sb_sig_type type;
};

void sb_siglist_open(struct sb_sig_cursor *cursor, const unsigned char *database, size_t bytes);
/* The next entry: its type and its data without the owner GUID. 1 with an entry, 0 at the end, -1 when the
 * database is malformed. */
int sb_siglist_next(struct sb_sig_cursor *cursor, enum sb_sig_type *type, struct sb_span *data);

struct sb_siglist_count { unsigned x509, sha256, x509_sha256, other; };
/* Counts the entries. 1 when the whole database is well formed. */
int sb_siglist_count(const unsigned char *database, size_t bytes, struct sb_siglist_count *out);

#endif
