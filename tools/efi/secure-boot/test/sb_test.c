/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host tests of the Secure Boot module: the DER reader, the signature lists, the Authenticode digest, the
 * certificate and signature readers, RSA, and the verdict.
 *
 * The images, certificates and databases come from test/make_fixtures.py, which makes new keys at each run and
 * writes no private key. When the fixture directory is not there, the tests that need it are skipped and the test
 * says so. One more optional input is a real signed UEFI image (for example a Windows Boot Manager): it is the
 * positive control of the whole chain on bytes that this project did not make.
 */
#include "efl_test.h"
#include "efi.h"
#include "efl_sha256.h"
#include "sb_der.h"
#include "sb_pe.h"
#include "sb_pkcs7.h"
#include "sb_siglist.h"
#include "sb_verify.h"
#include "sb_x509.h"

/* ---- DER ----------------------------------------------------------------------------------------- */

static void test_der(void)
{
	static const unsigned char two[] = { 0x30, 0x03, 0x02, 0x01, 0x05, 0x04, 0x00 };
	unsigned char big[4 + 300];
	struct sb_span in = { two, sizeof(two) }, inner;
	struct sb_tlv t;

	CHECK(sb_der_expect(&in, SB_DER_SEQUENCE, &t) && t.value.n == 3 && t.whole.n == 5 && in.n == 2);
	inner = t.value;
	CHECK(!sb_der_expect(&inner, SB_DER_OID, &t) && inner.n == 3);		/* a wrong tag moves nothing */
	CHECK(sb_der_expect(&inner, SB_DER_INTEGER, &t) && t.value.n == 1 && t.value.p[0] == 5 && inner.n == 0);
	CHECK(sb_der_next(&in, &t) && t.tag == SB_DER_OCTET_STRING && t.value.n == 0 && in.n == 0);
	CHECK(!sb_der_next(&in, &t));

	/* The long length form. */
	memset(big, 0, sizeof(big));
	big[0] = 0x04; big[1] = 0x82; big[2] = 0x01; big[3] = 0x2C;
	in.p = big; in.n = sizeof(big);
	CHECK(sb_der_next(&in, &t) && t.value.n == 300 && t.whole.n == 304 && in.n == 0);
	in.p = big; in.n = sizeof(big) - 1u;					/* the value is cut */
	CHECK(!sb_der_next(&in, &t));
	big[1] = 0x80;								/* the indefinite form */
	in.p = big; in.n = sizeof(big);
	CHECK(!sb_der_next(&in, &t));
	big[1] = 0x85;								/* five length octets */
	CHECK(!sb_der_next(&in, &t));
	big[0] = 0x1F; big[1] = 0x01;						/* the long tag form */
	CHECK(!sb_der_next(&in, &t));
	in.n = 1;
	CHECK(!sb_der_next(&in, &t));
	in.p = NULL; in.n = 0;
	CHECK(!sb_der_next(&in, &t));
}

/* ---- images and databases ------------------------------------------------------------------------ */

struct file { unsigned char *data; size_t bytes; };

static struct file load(const char *dir, const char *name)
{
	struct file f = { NULL, 0 };
	if (name != NULL) {
		f.data = efl_test_load(dir, name, &f.bytes);
		if (f.data == NULL) {
			failures++;
			printf("FAIL the fixture %s did not read\n", name);
		}
	}
	return f;
}

static enum sb_verdict verdict_of(const struct file *image, const struct file *db, const struct file *dbx,
				  unsigned char *digest)
{
	struct sb_databases d = { db->data, dbx->data, db->bytes, dbx->bytes };
	struct sb_result result;
	sb_verify_image(image->data, image->bytes, &d, &result);
	if (digest != NULL) memcpy(digest, result.digest, 32);
	return result.verdict;
}

static void test_verdicts(const char *dir)
{
	static const struct { const char *image, *db, *dbx; enum sb_verdict want; } rows[] = {
		/* A chain of three keys (4096, 3072 and 2048 bits), with each of its certificates as the db entry. */
		{ "signed.efi", "db_root.esl", NULL, SB_ALLOWED_SIGNATURE },
		{ "signed.efi", "db_inter.esl", NULL, SB_ALLOWED_SIGNATURE },
		{ "signed.efi", "db_leaf.esl", NULL, SB_ALLOWED_SIGNATURE },
		{ "signed32.efi", "db_root.esl", NULL, SB_ALLOWED_SIGNATURE },
		{ "signed.efi", "db_mixed.esl", NULL, SB_ALLOWED_SIGNATURE },
		{ "signed_two.efi", "db_root.esl", NULL, SB_ALLOWED_SIGNATURE },
		/* No db entry leads to the signature. */
		{ "signed.efi", NULL, NULL, SB_NOT_TRUSTED },
		{ "signed.efi", "bad.esl", NULL, SB_NOT_TRUSTED },
		{ "signed_other.efi", "db_root.esl", NULL, SB_NOT_TRUSTED },
		{ "signed_noca.efi", "db_root.esl", NULL, SB_NOT_TRUSTED },
		/* The digest of the image. */
		{ "unsigned.efi", "db_digest.esl", NULL, SB_ALLOWED_DIGEST },
		{ "unsigned.efi", "db_mixed.esl", NULL, SB_ALLOWED_DIGEST },
		{ "signed_other.efi", "db_digest.esl", NULL, SB_ALLOWED_DIGEST },
		{ "unsigned.efi", "db_root.esl", NULL, SB_NOT_SIGNED },
		{ "unsigned.efi", NULL, NULL, SB_NOT_SIGNED },
		{ "unsigned32.efi", "db_digest.esl", NULL, SB_NOT_SIGNED },
		/* The forbidden database wins over the allowed one. */
		{ "unsigned.efi", "db_digest.esl", "dbx_digest.esl", SB_FORBIDDEN_DIGEST },
		{ "signed.efi", "db_root.esl", "dbx_digest.esl", SB_FORBIDDEN_DIGEST },
		{ "signed.efi", "db_root.esl", "dbx_leaf.esl", SB_FORBIDDEN_CERTIFICATE },
		{ "signed.efi", "db_root.esl", "dbx_inter.esl", SB_FORBIDDEN_CERTIFICATE },
		{ "signed.efi", "db_root.esl", "dbx_root.esl", SB_FORBIDDEN_CERTIFICATE },
		{ "signed.efi", "db_leaf.esl", "dbx_inter.esl", SB_FORBIDDEN_CERTIFICATE },
		{ "signed.efi", "db_leaf.esl", "dbx_root.esl", SB_FORBIDDEN_CERTIFICATE },
		{ "signed_two.efi", "db_root.esl", "dbx_leaf.esl", SB_FORBIDDEN_CERTIFICATE },
		{ "signed_other.efi", "db_root.esl", "dbx_leaf.esl", SB_NOT_TRUSTED },
		/* Changed bytes. */
		{ "signed_tampered.efi", "db_root.esl", NULL, SB_BAD_SIGNATURE },
		{ "signed_tampered.efi", "db_digest.esl", NULL, SB_BAD_SIGNATURE },
		{ "signed_badsig.efi", "db_root.esl", NULL, SB_BAD_SIGNATURE },
		{ "signed_nobag.efi", "db_leaf.esl", NULL, SB_BAD_SIGNATURE },
		{ "signed_badtable.efi", "db_root.esl", NULL, SB_NOT_AN_IMAGE },
		{ "not_pe.bin", "db_root.esl", NULL, SB_NOT_AN_IMAGE },
	};
	unsigned i;
	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		struct file image = load(dir, rows[i].image), db = load(dir, rows[i].db), dbx = load(dir, rows[i].dbx);
		enum sb_verdict got = verdict_of(&image, &db, &dbx, NULL);
		checks++;
		if (got != rows[i].want) {
			failures++;
			printf("FAIL verdict row %u: %s with %s and %s: got \"%s\", want \"%s\"\n", i, rows[i].image,
			       rows[i].db ? rows[i].db : "no db", rows[i].dbx ? rows[i].dbx : "no dbx", sb_verdict_text(got),
			       sb_verdict_text(rows[i].want));
		}
		CHECK(sb_verdict_allows(got) == (got == SB_ALLOWED_SIGNATURE || got == SB_ALLOWED_DIGEST));
		free(image.data);
		free(db.data);
		free(dbx.data);
	}
}

static void test_digests(const char *dir)
{
	static const char *const names[] = { "unsigned.efi", "unsigned32.efi", "signed.efi" };
	char digest_name[64];
	unsigned i;
	for (i = 0; i < 3; i++) {
		struct file image = load(dir, names[i]), want;
		struct sb_pe pe;
		snprintf(digest_name, sizeof(digest_name), "%s.digest", names[i]);
		want = load(dir, digest_name);
		CHECK(sb_pe_read(image.data, image.bytes, &pe) == 1);
		CHECK(want.bytes == 32 && !memcmp(pe.digest, want.data, 32));
		CHECK((pe.cert_bytes != 0) == (i == 2));
		free(image.data);
		free(want.data);
	}
}

static void test_lists(const char *dir)
{
	struct file mixed = load(dir, "db_mixed.esl"), bad = load(dir, "bad.esl"), leaf = load(dir, "db_leaf.esl");
	struct file inter = load(dir, "db_inter.esl"), root = load(dir, "db_root.esl");
	struct sb_siglist_count count;
	struct sb_sig_cursor cursor;
	enum sb_sig_type type;
	struct sb_span data;
	struct sb_cert c_leaf, c_inter, c_root;

	CHECK(sb_siglist_count(mixed.data, mixed.bytes, &count) == 1);
	CHECK(count.x509 == 2 && count.sha256 == 2 && count.other == 1);
	CHECK(sb_siglist_count(bad.data, bad.bytes, &count) == 0);
	CHECK(sb_siglist_count(NULL, 0, &count) == 1 && count.x509 + count.sha256 + count.other == 0);
	CHECK(sb_siglist_count(mixed.data, 27, &count) == 0);

	/* The certificates of the chain, read from the lists. */
	sb_siglist_open(&cursor, leaf.data, leaf.bytes);
	CHECK(sb_siglist_next(&cursor, &type, &data) == 1 && type == SB_SIG_X509 && sb_cert_parse(data.p, data.n, &c_leaf));
	CHECK(sb_siglist_next(&cursor, &type, &data) == 0);
	sb_siglist_open(&cursor, inter.data, inter.bytes);
	CHECK(sb_siglist_next(&cursor, &type, &data) == 1 && sb_cert_parse(data.p, data.n, &c_inter));
	sb_siglist_open(&cursor, root.data, root.bytes);
	CHECK(sb_siglist_next(&cursor, &type, &data) == 1 && sb_cert_parse(data.p, data.n, &c_root));
	CHECK(c_leaf.rsa && c_inter.rsa && c_root.rsa && c_leaf.sha256_rsa);
	CHECK(!c_leaf.ca && c_inter.ca && c_root.ca);
	CHECK(c_leaf.signature.n == 384 && c_inter.signature.n == 512 && c_root.signature.n == 512);
	CHECK(sb_cert_signed_by(&c_leaf, &c_inter) == 1 && sb_cert_signed_by(&c_inter, &c_root) == 1);
	CHECK(sb_cert_signed_by(&c_root, &c_root) == 1);
	CHECK(sb_cert_signed_by(&c_inter, &c_leaf) == 0 && sb_cert_signed_by(&c_leaf, &c_root) == 0);
	/* A certificate with one changed byte in its signed part does not verify. One with bytes after it is not a
	 * certificate. */
	if (data.n > 200u) {
		unsigned char *changed = malloc(data.n + 1u);
		struct sb_cert c;
		memcpy(changed, data.p, data.n);
		changed[data.n] = 0;
		CHECK(sb_cert_parse(changed, data.n + 1u, &c) == 0);
		changed[150] ^= 1u;
		if (sb_cert_parse(changed, data.n, &c)) CHECK(sb_cert_signed_by(&c, &c_root) == 0);
		free(changed);
	}
	free(mixed.data);
	free(bad.data);
	free(leaf.data);
	free(inter.data);
	free(root.data);
}

/* Changed and cut images: no change of a signed byte is allowed, and no input makes the reader leave its buffer
 * (the address sanitizer of the test build watches that). */
static void test_robust(const char *dir)
{
	struct file image = load(dir, "signed.efi"), db = load(dir, "db_root.esl"), none = { NULL, 0 };
	unsigned char *copy;
	size_t at, security, checksum, pe_at;
	unsigned allowed = 0;
	if (image.data == NULL || db.data == NULL) return;
	copy = malloc(image.bytes);
	pe_at = image.data[0x3C];
	checksum = pe_at + 24u + 64u;
	security = pe_at + 24u + 112u + 4u * 8u;

	/* Every byte of the headers and the sections, one at a time. The checksum field and the certificate table
	 * entry are not under the digest. */
	for (at = 0; at < 0x600u; at++) {
		struct file changed = { copy, image.bytes };
		if ((at >= checksum && at < checksum + 4u) || (at >= security && at < security + 8u)) continue;
		memcpy(copy, image.data, image.bytes);
		copy[at] ^= 0x04u;
		if (sb_verdict_allows(verdict_of(&changed, &db, &none, NULL))) allowed++;
	}
	CHECK(allowed == 0);
	/* Every byte of the certificate table, one at a time. A change of a byte that no signature covers can stay
	 * allowed, so only the count of the refused changes has a floor: the three RSA signatures and the signed
	 * parts are most of the table. */
	allowed = 0;
	for (at = 0x600u; at < image.bytes; at++) {
		struct file changed = { copy, image.bytes };
		memcpy(copy, image.data, image.bytes);
		copy[at] ^= 0x04u;
		if (sb_verdict_allows(verdict_of(&changed, &db, &none, NULL))) allowed++;
	}
	CHECK(allowed < (image.bytes - 0x600u) / 4u);
	printf("secure-boot: %u of %u one-byte changes of the certificate table stay allowed (bytes under no signature)\n",
	       allowed, (unsigned)(image.bytes - 0x600u));
	/* Every length of the file. Only the whole file is allowed. */
	allowed = 0;
	for (at = 0; at < image.bytes; at++) {
		unsigned char *cut = malloc(at ? at : 1u);
		struct file changed = { cut, at };
		memcpy(cut, image.data, at);
		if (sb_verdict_allows(verdict_of(&changed, &db, &none, NULL))) allowed++;
		free(cut);
	}
	CHECK(allowed == 0);
	free(copy);
	free(image.data);
	free(db.data);
}

/* A real signed image: its signature must verify under its own signer, and the image must be allowed when the
 * issuer of that signer is the one db entry, and not trusted with an empty db. */
static void test_real_image(const char *path)
{
	struct file image = { NULL, 0 }, none = { NULL, 0 }, db = { NULL, 0 };
	const unsigned char *signature;
	size_t cursor = 0, signature_bytes;
	struct sb_pe pe;
	struct sb_pkcs7 p7;
	struct sb_cert signer, issuer;
	struct sb_span bag;
	int have_issuer = 0;

	if (path == NULL || path[0] == 0) {
		printf("SKIP secure-boot, real signed image: no path given (build.ps1 -SignedImage <file>)\n");
		return;
	}
	image.data = efl_test_load("", path, &image.bytes);
	if (image.data == NULL) {
		printf("SKIP secure-boot, real signed image: %s did not read\n", path);
		return;
	}
	CHECK(sb_pe_read(image.data, image.bytes, &pe) == 1 && pe.cert_bytes != 0);
	CHECK(sb_pe_next_signature(image.data, &pe, &cursor, &signature, &signature_bytes) == 1);
	CHECK(sb_pkcs7_parse(signature, signature_bytes, &p7) == 1);
	/* The digest that the signer put into the signature is the digest that this code computes. */
	CHECK(p7.image_digest.n == 32 && !memcmp(p7.image_digest.p, pe.digest, 32));
	CHECK(sb_pkcs7_signer(&p7, &signer) == 1 && sb_pkcs7_signed_by(&p7, &signer) == 1);
	bag = p7.certificates;
	while (sb_pkcs7_next_cert(&bag, &issuer))
		if (!sb_span_equal(&issuer.whole, &signer.whole) && sb_cert_signed_by(&signer, &issuer)) {
			have_issuer = 1;
			break;
		}
	CHECK(have_issuer);
	CHECK(verdict_of(&image, &none, &none, NULL) == SB_NOT_TRUSTED);
	if (have_issuer) {
		/* One EFI_SIGNATURE_LIST with the issuer's certificate. */
		static const unsigned char x509_guid[16] = { 0xA1, 0x59, 0xC0, 0xA5, 0xE4, 0x94, 0xA7, 0x4A,
							     0x87, 0xB5, 0xAB, 0x15, 0x5C, 0x2B, 0xF0, 0x72 };
		size_t entry = 16u + issuer.whole.n, total = 28u + entry;
		db.data = calloc(1, total);
		db.bytes = total;
		memcpy(db.data, x509_guid, 16);
		db.data[16] = (unsigned char)total; db.data[17] = (unsigned char)(total >> 8);
		db.data[24] = (unsigned char)entry; db.data[25] = (unsigned char)(entry >> 8);
		memcpy(db.data + 28u + 16u, issuer.whole.p, issuer.whole.n);
		CHECK(verdict_of(&image, &db, &none, NULL) == SB_ALLOWED_SIGNATURE);
		CHECK(verdict_of(&image, &none, &db, NULL) == SB_FORBIDDEN_CERTIFICATE);
		image.data[image.bytes / 3u] ^= 1u;
		CHECK(!sb_verdict_allows(verdict_of(&image, &db, &none, NULL)));
		printf("secure-boot: real signed image %s: %u bytes, signer key %u bits, issuer key %u bits: verified\n", path,
		       (unsigned)image.bytes, (unsigned)signer.signature.n * 8u, (unsigned)issuer.signature.n * 8u);
	}
	free(db.data);
	free(image.data);
}

unsigned sb_tests(const char *fixture_dir, const char *real_image, unsigned *check_count)
{
	size_t probe_bytes = 0;
	unsigned char *probe = efl_test_load(fixture_dir, "signed.efi", &probe_bytes);
	test_der();
	if (probe == NULL) {
		printf("SKIP secure-boot, images and databases: no fixtures in %s (make_fixtures.py needs the Python package "
		       "'cryptography')\n", fixture_dir ? fixture_dir : "(no directory given)");
	} else {
		free(probe);
		test_digests(fixture_dir);
		test_lists(fixture_dir);
		test_verdicts(fixture_dir);
		test_robust(fixture_dir);
	}
	test_real_image(real_image);
	*check_count = checks;
	return failures;
}
