/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The runner of the host tests of the BC-250 stage. Each part of the stage has its own test file and gives its
 * counts to this runner. The firmware is not here: the tests run the pure code of each part against models and
 * stored tables.
 *
 *   host_test <evidence ACPI directory> <unit A ACPI dump directory> <fixture directory> <output directory>
 *             [<real signed image> [<second real signed image>]]
 *
 * A directory that is not there makes the tests that need it say SKIP. It does not make them fail.
 */
#include <stdio.h>

unsigned cu_tests(const char *evidence_dir, unsigned *check_count);
unsigned stage_tests(unsigned *check_count);
unsigned lib_tests(unsigned *check_count);
unsigned ar_tests(const char *evidence_dir, const char *dump_dir, const char *out_dir, unsigned *check_count);
unsigned sb_tests(const char *fixture_dir, const char *real_image, const char *second_image, unsigned *check_count);

static unsigned total_checks, total_failures;

static void report(const char *part, unsigned failed, unsigned count)
{
	printf("%-12s %5u checks, %u failures\n", part, count, failed);
	total_checks += count;
	total_failures += failed;
}

int main(int argc, char **argv)
{
	const char *evidence = argc > 1 ? argv[1] : NULL, *dump = argc > 2 ? argv[2] : NULL;
	const char *fixtures = argc > 3 ? argv[3] : NULL, *out = argc > 4 ? argv[4] : NULL;
	const char *real_image = argc > 5 ? argv[5] : NULL, *second_image = argc > 6 ? argv[6] : NULL;
	unsigned count = 0, failed;

	failed = lib_tests(&count);
	report("lib", failed, count);
	failed = stage_tests(&count);
	report("core", failed, count);
	failed = cu_tests(evidence, &count);
	report("core-unlock", failed, count);
	failed = ar_tests(evidence, dump, out, &count);
	report("acpi-repair", failed, count);
	failed = sb_tests(fixtures, real_image, second_image, &count);
	report("secure-boot", failed, count);
	printf("bc250-stage host tests: %u checks, %u failures\n", total_checks, total_failures);
	return total_failures ? 1 : 0;
}
