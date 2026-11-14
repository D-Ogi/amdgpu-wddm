/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The Authenticode view of a PE image: the digest of the image and its attribute certificate table. The file
 * layout is that of the Microsoft "PE Format" specification, and the digest is the one of "Windows Authenticode
 * Portable Executable Signature Format" (version 1.0, 2008), section "Calculating the PE Image Hash". The order of
 * the steps was compared with HashPeImage in edk2 SecurityPkg/Library/DxeImageVerificationLib (commit
 * 8628578dacd3). No code is copied.
 *
 * Stricter than the format: the attribute certificate table must be the last bytes of the file, and no hashed
 * range may reach into it. A file that keeps bytes outside both is refused.
 */
#ifndef EFL_PE_H
#define EFL_PE_H
#include <stddef.h>

struct efl_pe {
	unsigned char digest[32];	/* the SHA-256 Authenticode digest */
	size_t cert_offset, cert_bytes;	/* the attribute certificate table, 0 bytes when the image has none */
};

/* 1 when the bytes are a PE32 or PE32+ image whose layout is sound. */
int efl_pe_read(const unsigned char *image, size_t bytes, struct efl_pe *out);

/* The next PKCS #7 SignedData of the certificate table (a WIN_CERTIFICATE of revision 2.0 and type
 * WIN_CERT_TYPE_PKCS_SIGNED_DATA). *cursor starts at 0. Returns 1 with the bytes, 0 at the end of the table, -1
 * when the table is malformed. Entries of another type are skipped. */
int efl_pe_next_signature(const unsigned char *image, const struct efl_pe *pe, size_t *cursor,
			 const unsigned char **signature, size_t *signature_bytes);

#endif
