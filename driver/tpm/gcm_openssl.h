/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * AES-256-GCM of the state file (statefile.h, tpm_statefile_gcm) over the EVP interface of OpenSSL. One source for
 * both users: the TPM engine of the stage links it with its OpenSSL (tools/efi/tpm/engine), the Windows core with
 * its own (driver/tpm/core). Both are the OpenSSL revision of tools/efi/tpm/PROVENANCE.md.
 */
#ifndef TPM_GCM_OPENSSL_H
#define TPM_GCM_OPENSSL_H
#include <stddef.h>

int tpm_gcm_openssl(int seal, const unsigned char key[32], const unsigned char nonce[12], const unsigned char *aad,
		    size_t aad_bytes, const unsigned char *in, size_t n, unsigned char *out, unsigned char tag[16]);

#endif
