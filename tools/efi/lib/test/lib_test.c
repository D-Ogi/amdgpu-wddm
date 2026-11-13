/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Host tests of the shared library of the stage: SHA-256 against the vectors of the standard, and the AML writer
 * against the AML name scan and against the encodings of the ACPI Specification. */
#include "efl_test.h"
#include "efl_aml.h"
#include "efl_amlw.h"
#include "efl_sha256.h"

static int digest_is(const unsigned char *digest, const char *hex)
{
	char text[2u * EFL_SHA256_BYTES + 1u];
	unsigned i;
	for (i = 0; i < EFL_SHA256_BYTES; i++) snprintf(text + 2u * i, 3, "%02x", digest[i]);
	return !strcmp(text, hex);
}

/* The example digests of FIPS 180-4 (the NIST "SHA256" example file) and the one-million "a" message. */
static void test_sha256(void)
{
	static const char two_blocks[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	static char million[1000000];
	unsigned char digest[EFL_SHA256_BYTES], again[EFL_SHA256_BYTES];
	struct efl_sha256 s;
	size_t at, step;

	efl_sha256("", 0, digest);
	CHECK(digest_is(digest, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	efl_sha256("abc", 3, digest);
	CHECK(digest_is(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	efl_sha256(two_blocks, sizeof(two_blocks) - 1u, digest);
	CHECK(digest_is(digest, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
	memset(million, 'a', sizeof(million));
	efl_sha256(million, sizeof(million), digest);
	CHECK(digest_is(digest, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

	/* Updates of every small size give the digest of the whole message. */
	for (step = 1; step < 130u; step++) {
		efl_sha256_init(&s);
		for (at = 0; at < 1000u; at += step)
			efl_sha256_update(&s, million + at, at + step > 1000u ? 1000u - at : step);
		efl_sha256_final(&s, again);
		efl_sha256(million, 1000u, digest);
		CHECK(!memcmp(digest, again, sizeof(digest)));
	}
}

static unsigned char sum_of(const unsigned char *p, size_t n)
{
	unsigned char sum = 0;
	while (n--) sum = (unsigned char)(sum + *p++);
	return sum;
}

static void test_amlw(void)
{
	static struct efl_aml_scan scan;
	static const char *const watch[2] = { "ABCD", "NONE" };
	unsigned char table[512], small[40];
	struct efl_amlw w;
	size_t bytes, mark, inner, length;

	/* The integer forms. */
	efl_amlw_init(&w, table, sizeof(table));
	efl_amlw_integer(&w, 0);
	efl_amlw_integer(&w, 1);
	efl_amlw_integer(&w, 0x12);
	efl_amlw_integer(&w, 0x1234);
	efl_amlw_integer(&w, 0x12345678);
	efl_amlw_integer(&w, 0x123456789ull);
	CHECK(!w.overflow && w.used == 1u + 1u + 2u + 3u + 5u + 9u);
	CHECK(!memcmp(table, "\x00\x01\x0A\x12\x0B\x34\x12\x0C\x78\x56\x34\x12\x0E\x89\x67\x45\x23\x01\x00\x00\x00", 21));

	/* The path forms: one segment, two (dual prefix), three (multi prefix). */
	efl_amlw_init(&w, table, sizeof(table));
	efl_amlw_path(&w, "\\_PR_");
	efl_amlw_path(&w, "\\_PR_.P000");
	efl_amlw_path(&w, "\\_SB_.PCI0.GPP0");
	CHECK(!w.overflow && w.used == 5u + 10u + 15u);
	CHECK(!memcmp(table, "\\_PR_" "\\\x2E_PR_P000" "\\\x2F\x03_SB_PCI0GPP0", 30));
	efl_amlw_init(&w, table, sizeof(table));
	efl_amlw_path(&w, "_PR_.P000");		/* not absolute */
	CHECK(w.overflow);
	efl_amlw_init(&w, table, sizeof(table));
	efl_amlw_path(&w, "\\_PR.P000");		/* a segment of three characters */
	CHECK(w.overflow);

	/* A table: one scope with a package and a method. The scan finds what was written. */
	efl_amlw_init(&w, table, sizeof(table));
	efl_amlw_table_begin(&w, "SSDT", 2, "TEST", "WRITER", 7, "BCST", 1);
	mark = efl_amlw_scope_begin(&w, "\\_PR_.P001");
	efl_amlw_name(&w, "_CST");
	inner = efl_amlw_package_begin(&w, 2);
	efl_amlw_integer(&w, 1);
	efl_amlw_integer(&w, 0x414);
	efl_amlw_end(&w, inner);
	efl_amlw_empty_method(&w, "ABCD", 1);
	efl_amlw_end(&w, mark);
	bytes = efl_amlw_table_end(&w);
	CHECK(bytes == w.used && bytes > 36u);
	CHECK(!memcmp(table, "SSDT", 4) && table[4] == bytes && table[5] == 0 && table[8] == 2);
	CHECK(!memcmp(table + 10, "TEST  WRITER  ", 14) && table[24] == 7 && !memcmp(table + 28, "BCST", 4));
	CHECK(sum_of(table, bytes) == 0);
	CHECK(table[36] == 0x10);			/* ScopeOp */
	CHECK(efl_aml_pkg_length(table + 37, table + bytes, &length) == 1u && 37u + length == bytes);
	CHECK(efl_aml_scan_watch(table, bytes, watch, 2, &scan) == 1);
	CHECK(scan.objects == 1 && scan.malformed == 0 && scan.watched[0] == 1 && scan.watched[1] == 0);
	CHECK(!strcmp(scan.object[0].path, "\\_PR_.P001") && scan.object[0].kind == EFL_AML_SCOPE);
	CHECK(scan.object[0].names == (1u << EFL_AML_CST));
	/* Method (ABCD, 1, NotSerialized) {}: MethodOp, PkgLength 6, the name, the flags. */
	CHECK(!memcmp(table + bytes - 7u, "\x14\x06" "ABCD" "\x01", 7));

	/* A body of more than 63 bytes takes a package length of two bytes. */
	efl_amlw_init(&w, table, sizeof(table));
	efl_amlw_table_begin(&w, "SSDT", 2, "TEST", "LONG", 1, "BCST", 1);
	mark = efl_amlw_scope_begin(&w, "\\_PR_");
	for (inner = 0; inner < 20u; inner++) efl_amlw_empty_method(&w, "ABCD", 0);
	efl_amlw_end(&w, mark);
	bytes = efl_amlw_table_end(&w);
	CHECK(bytes == 36u + 1u + 2u + 5u + 20u * 7u);
	CHECK(efl_aml_pkg_length(table + 37, table + bytes, &length) == 2u && 37u + length == bytes);
	CHECK(sum_of(table, bytes) == 0);

	/* No room: the table end says so, and nothing is written past the buffer. */
	memset(small, 0xEE, sizeof(small));
	efl_amlw_init(&w, small, 38);
	efl_amlw_table_begin(&w, "SSDT", 2, "TEST", "SMALL", 1, "BCST", 1);
	efl_amlw_empty_method(&w, "ABCD", 0);
	CHECK(efl_amlw_table_end(&w) == 0 && w.overflow);
	CHECK(small[38] == 0xEE && small[39] == 0xEE);
}

unsigned lib_tests(unsigned *check_count)
{
	test_sha256();
	test_amlw();
	*check_count = checks;
	return failures;
}
