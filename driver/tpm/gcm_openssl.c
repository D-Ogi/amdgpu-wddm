/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* AES-256-GCM over OpenSSL EVP. The contract is tpm_statefile_gcm in statefile.h. */
#include <limits.h>
#include <openssl/evp.h>
#include "gcm_openssl.h"

int tpm_gcm_openssl(int seal, const unsigned char key[32], const unsigned char nonce[12], const unsigned char *aad,
		    size_t aad_bytes, const unsigned char *in, size_t n, unsigned char *out, unsigned char tag[16])
{
	EVP_CIPHER_CTX *c;
	int ok, len = 0, last = 0;

	if (key == NULL || nonce == NULL || tag == NULL || (n && (in == NULL || out == NULL)) || n > INT_MAX ||
	    aad_bytes > INT_MAX || (aad_bytes && aad == NULL))
		return 0;
	c = EVP_CIPHER_CTX_new();
	if (c == NULL) return 0;
	/* The default nonce length of GCM in EVP is 12 bytes, the length of the state file. */
	ok = EVP_CipherInit_ex(c, EVP_aes_256_gcm(), NULL, key, nonce, seal ? 1 : 0) == 1;
	if (ok && aad_bytes) ok = EVP_CipherUpdate(c, NULL, &len, aad, (int)aad_bytes) == 1;
	/* For an open the tag goes in before the final step, which checks it. */
	if (ok && !seal) ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, tag) == 1;
	if (ok && n) ok = EVP_CipherUpdate(c, out, &len, in, (int)n) == 1;
	if (ok) ok = EVP_CipherFinal_ex(c, out != NULL ? out + len : NULL, &last) == 1;
	if (ok && seal) ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
	EVP_CIPHER_CTX_free(c);
	return ok;
}
