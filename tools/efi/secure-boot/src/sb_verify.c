/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The image verdict (sb_verify.h). */
#include <stddef.h>
#include <string.h>
#include "efl_sha256.h"
#include "efl_pe.h"
#include "sb_pkcs7.h"
#include "sb_siglist.h"
#include "sb_verify.h"

#define MAX_CHAIN	8u	/* certificates from the signer up; a longer chain is not followed */

enum chain { CHAIN_UNKNOWN = 0, CHAIN_ALLOWED, CHAIN_FORBIDDEN };

static int digest_listed(const unsigned char *database, size_t bytes, const unsigned char *digest)
{
	struct sb_sig_cursor cursor;
	enum sb_sig_type type;
	struct sb_span data;
	sb_siglist_open(&cursor, database, bytes);
	while (sb_siglist_next(&cursor, &type, &data) == 1)
		if (type == SB_SIG_SHA256 && !memcmp(data.p, digest, EFL_SHA256_BYTES)) return 1;
	return 0;
}

/* 1 when the database holds this certificate, byte for byte. */
static int cert_listed(const unsigned char *database, size_t bytes, const struct sb_cert *cert)
{
	struct sb_sig_cursor cursor;
	enum sb_sig_type type;
	struct sb_span data;
	sb_siglist_open(&cursor, database, bytes);
	while (sb_siglist_next(&cursor, &type, &data) == 1)
		if (type == SB_SIG_X509 && sb_span_equal(&data, &cert->whole)) return 1;
	return 0;
}

/* 1 when the database holds the SHA-256 digest of the signed part of this certificate. */
static int cert_digest_listed(const unsigned char *database, size_t bytes, const struct sb_cert *cert)
{
	unsigned char digest[EFL_SHA256_BYTES];
	struct sb_sig_cursor cursor;
	enum sb_sig_type type;
	struct sb_span data;
	efl_sha256(cert->tbs.p, cert->tbs.n, digest);
	sb_siglist_open(&cursor, database, bytes);
	while (sb_siglist_next(&cursor, &type, &data) == 1)
		if (type == SB_SIG_X509_SHA256 && !memcmp(data.p, digest, EFL_SHA256_BYTES)) return 1;
	return 0;
}

/* 1 when a certificate of the database signed cert. */
static int cert_issued_by(const unsigned char *database, size_t bytes, const struct sb_cert *cert)
{
	struct sb_sig_cursor cursor;
	enum sb_sig_type type;
	struct sb_span data;
	struct sb_cert issuer;
	sb_siglist_open(&cursor, database, bytes);
	while (sb_siglist_next(&cursor, &type, &data) == 1)
		if (type == SB_SIG_X509 && sb_cert_parse(data.p, data.n, &issuer) && sb_cert_signed_by(cert, &issuer))
			return 1;
	return 0;
}

/* Walks from the signer's certificate up through the certificates of the signature. The walk goes to the top
 * also after db allowed a certificate, because a dbx entry higher in the chain still forbids the image. */
static enum chain walk(const struct sb_pkcs7 *p7, const struct sb_cert *signer, const struct sb_databases *d)
{
	struct sb_cert current = *signer, next;
	unsigned depth;
	int allowed = 0;
	for (depth = 0; depth < MAX_CHAIN; depth++) {
		struct sb_span cursor = p7->certificates;
		int found = 0;
		if (cert_listed(d->dbx, d->dbx_bytes, &current) || cert_digest_listed(d->dbx, d->dbx_bytes, &current) ||
		    cert_issued_by(d->dbx, d->dbx_bytes, &current))
			return CHAIN_FORBIDDEN;
		if (!allowed && (cert_listed(d->db, d->db_bytes, &current) || cert_issued_by(d->db, d->db_bytes, &current)))
			allowed = 1;
		/* One step up: a certificate of the signature that is a CA and signed the current one. A certificate
		 * that signed itself ends the walk. */
		while (sb_pkcs7_next_cert(&cursor, &next)) {
			if (!next.ca || sb_span_equal(&next.whole, &current.whole)) continue;
			if (sb_cert_signed_by(&current, &next)) {
				found = 1;
				break;
			}
		}
		if (!found) break;
		current = next;
	}
	return allowed ? CHAIN_ALLOWED : CHAIN_UNKNOWN;
}
void sb_verify_image(const unsigned char *image, size_t bytes, const struct sb_databases *d, struct sb_result *out)
{
	struct sb_siglist_count count;
	struct efl_pe pe;
	const unsigned char *signature;
	size_t cursor = 0, signature_bytes;
	unsigned valid = 0, allowed = 0, forbidden = 0, seen = 0;
	int more;

	memset(out, 0, sizeof(*out));
	if (!sb_siglist_count(d->db, d->db_bytes, &count) || !sb_siglist_count(d->dbx, d->dbx_bytes, &count)) {
		out->verdict = SB_BAD_DATABASE;
		return;
	}
	if (!efl_pe_read(image, bytes, &pe)) {
		out->verdict = SB_NOT_AN_IMAGE;
		return;
	}
	memcpy(out->digest, pe.digest, sizeof(out->digest));
	if (digest_listed(d->dbx, d->dbx_bytes, pe.digest)) {
		out->verdict = SB_FORBIDDEN_DIGEST;
		return;
	}
	while ((more = efl_pe_next_signature(image, &pe, &cursor, &signature, &signature_bytes)) == 1) {
		struct sb_pkcs7 p7;
		struct sb_cert signer;
		enum chain chain;
		seen++;
		if (!sb_pkcs7_parse(signature, signature_bytes, &p7)) continue;
		if (memcmp(p7.image_digest.p, pe.digest, EFL_SHA256_BYTES) != 0) continue;
		if (!sb_pkcs7_signer(&p7, &signer) || !sb_pkcs7_signed_by(&p7, &signer)) continue;
		valid++;
		chain = walk(&p7, &signer, d);
		if (chain == CHAIN_FORBIDDEN) forbidden++;
		else if (chain == CHAIN_ALLOWED) allowed++;
	}
	if (forbidden)
		out->verdict = SB_FORBIDDEN_CERTIFICATE;
	else if (allowed)
		out->verdict = SB_ALLOWED_SIGNATURE;
	else if (digest_listed(d->db, d->db_bytes, pe.digest))
		out->verdict = SB_ALLOWED_DIGEST;
	else if (valid)
		out->verdict = SB_NOT_TRUSTED;
	else if (seen || more < 0)
		out->verdict = SB_BAD_SIGNATURE;
	else
		out->verdict = SB_NOT_SIGNED;
}

int sb_verdict_allows(enum sb_verdict verdict)
{
	return verdict == SB_ALLOWED_SIGNATURE || verdict == SB_ALLOWED_DIGEST;
}

const char *sb_verdict_text(enum sb_verdict verdict)
{
	switch (verdict) {
	case SB_ALLOWED_SIGNATURE: return "allowed: a signature leads to a certificate of db";
	case SB_ALLOWED_DIGEST: return "allowed: the digest of the image is in db";
	case SB_FORBIDDEN_DIGEST: return "forbidden: the digest of the image is in dbx";
	case SB_FORBIDDEN_CERTIFICATE: return "forbidden: a certificate of its signature is in dbx";
	case SB_NOT_SIGNED: return "not allowed: no signature that this stage reads, and the digest is not in db";
	case SB_NOT_TRUSTED: return "not allowed: the signature is valid and leads to no certificate of db";
	case SB_BAD_SIGNATURE: return "not allowed: no signature of the image verifies";
	case SB_NOT_AN_IMAGE: return "not allowed: not a sound PE image";
	case SB_BAD_DATABASE: return "not allowed: db or dbx is not a well-formed signature database";
	}
	return "not allowed";
}
