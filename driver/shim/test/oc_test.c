/* The overclock tier policy (include/bc250_oc.h, bc250_oc.c): the envelopes, the stored tier against the board it
 * was chosen on, and the board fingerprint. Host-side only: driver/kmd/oc_tier.c runs exactly this decision. */
#include <stdio.h>
#include <string.h>
#include "bc250_oc.h"
#include "bc250_clock.h"

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

/* The absolute GPU cap stays under the imported kernel's own voltage limit (cyan_skillfish_ppt.c, 1129 mV). */
typedef char oc_cap_under_kernel_limit[(BC250_CLOCK_OC_CEILING_MV < 1129u) ? 1 : -1];

static void test_envelopes(void)
{
	struct bc250_oc_envelope e;
	unsigned int t;
	/* The tier none is the plain envelope: the defaults of every start before the tiers. */
	CHECK(bc250_oc_envelope_of(BC250_OC_TIER_NONE, &e) && e.tier == BC250_OC_TIER_NONE);
	CHECK(e.gpu_max_mhz == 2000u && e.gpu_max_mv == 1000u && e.gpu_max_level == 15u);
	CHECK(e.cpu_max_mhz == 3500u && e.cpu_uv_max_steps == 16u);
	CHECK(bc250_oc_envelope_of(BC250_OC_TIER_AIR, &e) && e.tier == BC250_OC_TIER_AIR);
	CHECK(e.gpu_max_mhz == 2200u && e.gpu_max_mv == 1050u && e.gpu_max_level == 17u);
	CHECK(e.cpu_max_mhz == 3900u && e.cpu_uv_max_steps == 40u);
	CHECK(bc250_oc_envelope_of(BC250_OC_TIER_AIO, &e) && e.tier == BC250_OC_TIER_AIO);
	CHECK(e.gpu_max_mhz == 2300u && e.gpu_max_mv == 1075u && e.gpu_max_level == 18u);
	CHECK(e.cpu_max_mhz == 4100u && e.cpu_uv_max_steps == 40u);
	/* An unknown tier gets the plain envelope and says so. */
	for (t = BC250_OC_TIER_COUNT; t < 300u; t++) {
		CHECK(!bc250_oc_envelope_of(t, &e) && e.tier == BC250_OC_TIER_NONE && e.gpu_max_mhz == 2000u &&
		      e.cpu_max_mhz == 3500u);
	}
	/* Every envelope's ceiling is a row of the table, at the row's own voltage, under the absolute caps. */
	for (t = 0; t < BC250_OC_TIER_COUNT; t++) {
		(void)bc250_oc_envelope_of(t, &e);
		CHECK(e.gpu_max_level < BC250_CLOCK_ALL_LEVELS);
		CHECK(bc250_clock_points[e.gpu_max_level].mhz == e.gpu_max_mhz);
		CHECK(bc250_clock_points[e.gpu_max_level].mv == e.gpu_max_mv);
		CHECK(e.gpu_max_mhz <= BC250_CLOCK_OC_CEILING_MHZ && e.gpu_max_mv <= BC250_CLOCK_OC_CEILING_MV);
		CHECK(e.cpu_max_mhz <= BC250_CPU_OC_MAX_MHZ && e.cpu_uv_max_steps <= BC250_CPU_OC_UV_MAX_STEPS);
		CHECK(bc250_oc_cpu_min_uv_steps(t, e.cpu_max_mhz) <= e.cpu_uv_max_steps);
	}
}

static struct bc250_oc_request request(int tier_present, unsigned int tier, int board_present, unsigned int board)
{
	struct bc250_oc_request r;
	memset(&r, 0, sizeof(r));
	r.supported = 1;
	r.current_board = 0x5EEDu;
	r.tier_present = tier_present;
	r.tier = tier;
	r.board_present = board_present;
	r.board = board;
	return r;
}

static void test_decide(void)
{
	struct bc250_oc_request r;
	struct bc250_oc_decision d;
	unsigned int t;
	/* Nothing stored: the tier none, nothing to delete. */
	r = request(0, 0, 0, 0); bc250_oc_decide(&r, &d);
	CHECK(d.tier == BC250_OC_TIER_NONE && d.reason == BC250_OC_REASON_NONE && !d.clear);
	/* A binding without a tier, or with the tier none: the binding goes. */
	r = request(0, 0, 1, 0x5EEDu); bc250_oc_decide(&r, &d);
	CHECK(d.tier == BC250_OC_TIER_NONE && d.reason == BC250_OC_REASON_NONE && d.clear);
	r = request(1, 0, 1, 0x5EEDu); bc250_oc_decide(&r, &d);
	CHECK(d.tier == BC250_OC_TIER_NONE && d.reason == BC250_OC_REASON_NONE && d.clear);
	/* The stored tier runs on the board it was chosen on. */
	for (t = BC250_OC_TIER_AIR; t <= BC250_OC_TIER_AIO; t++) {
		r = request(1, t, 1, 0x5EEDu); bc250_oc_decide(&r, &d);
		CHECK(d.tier == t && d.reason == BC250_OC_REASON_OK && !d.clear);
		/* Another board: deleted, the tier none runs. */
		r.board = 0x5EEEu; bc250_oc_decide(&r, &d);
		CHECK(d.tier == BC250_OC_TIER_NONE && d.reason == BC250_OC_REASON_BOARD_CHANGED && d.clear);
		/* No binding at all, or a zero binding: deleted. */
		r = request(1, t, 0, 0); bc250_oc_decide(&r, &d);
		CHECK(d.tier == BC250_OC_TIER_NONE && d.reason == BC250_OC_REASON_UNBOUND && d.clear);
		r = request(1, t, 1, 0); bc250_oc_decide(&r, &d);
		CHECK(d.tier == BC250_OC_TIER_NONE && d.reason == BC250_OC_REASON_UNBOUND && d.clear);
		/* No positive identification on this start: not run, and not deleted either. */
		r = request(1, t, 1, 0x5EEDu); r.supported = 0; bc250_oc_decide(&r, &d);
		CHECK(d.tier == BC250_OC_TIER_NONE && d.reason == BC250_OC_REASON_UNSUPPORTED && !d.clear);
		r = request(1, t, 1, 0x5EEDu); r.current_board = 0; bc250_oc_decide(&r, &d);
		CHECK(d.tier == BC250_OC_TIER_NONE && d.reason == BC250_OC_REASON_UNSUPPORTED && !d.clear);
		/* The last start went down above the floor: the tier goes, on a match and without identification alike. */
		r = request(1, t, 1, 0x5EEDu); r.unclean_stop = 1; bc250_oc_decide(&r, &d);
		CHECK(d.tier == BC250_OC_TIER_NONE && d.reason == BC250_OC_REASON_UNCLEAN_STOP && d.clear);
		r.supported = 0; bc250_oc_decide(&r, &d);
		CHECK(d.tier == BC250_OC_TIER_NONE && d.reason == BC250_OC_REASON_UNCLEAN_STOP && d.clear);
	}
	/* An unclean stop with no tier stored changes nothing here (DpmStart has its own fallback). */
	r = request(0, 0, 0, 0); r.unclean_stop = 1; bc250_oc_decide(&r, &d);
	CHECK(d.tier == BC250_OC_TIER_NONE && d.reason == BC250_OC_REASON_NONE && !d.clear);
	/* A value that is not a tier stays for the person to read, also after an unclean stop. */
	r = request(1, 7u, 1, 0x5EEDu); r.unclean_stop = 1; bc250_oc_decide(&r, &d);
	CHECK(d.reason == BC250_OC_REASON_INVALID && !d.clear);
	/* Not a tier: kept for the person to read, never run, whatever the binding. */
	for (t = BC250_OC_TIER_COUNT; t < 40u; t++) {
		r = request(1, t, 1, 0x5EEDu); bc250_oc_decide(&r, &d);
		CHECK(d.tier == BC250_OC_TIER_NONE && d.reason == BC250_OC_REASON_INVALID && !d.clear);
	}
	r = request(1, 0xFFFFFFFFu, 0, 0); bc250_oc_decide(&r, &d);
	CHECK(d.tier == BC250_OC_TIER_NONE && d.reason == BC250_OC_REASON_INVALID && !d.clear);
	/* Exhaustive over the small inputs: a tier other than none runs only with reason OK, and OK only on a match. */
	{
		int tp, bp, sup, uc;
		unsigned int b;
		for (tp = 0; tp < 2; tp++) for (bp = 0; bp < 2; bp++) for (sup = 0; sup < 2; sup++) for (uc = 0; uc < 2; uc++)
			for (t = 0; t < 5u; t++) for (b = 0; b < 3u; b++) {
				r = request(tp, t, bp, b ? 0x5EECu + b : 0u); r.supported = sup; r.unclean_stop = uc;
				bc250_oc_decide(&r, &d);
				CHECK((d.tier != BC250_OC_TIER_NONE) == (d.reason == BC250_OC_REASON_OK));
				if (d.reason == BC250_OC_REASON_OK)
					CHECK(tp && sup && bp && !uc && r.board == r.current_board && t > 0u && t < BC250_OC_TIER_COUNT);
				CHECK(d.tier < BC250_OC_TIER_COUNT && d.reason < BC250_OC_REASON_COUNT);
				/* A delete never removes a tier this start runs. */
				if (d.clear) CHECK(d.tier == BC250_OC_TIER_NONE);
			}
	}
}

static void test_fingerprint(void)
{
	unsigned char id[16];
	unsigned int a, b, i, j, zero = 0;
	memset(id, 0, sizeof(id));
	a = bc250_oc_board_fingerprint(id, 3u);
	CHECK(a != 0u);
	CHECK(bc250_oc_board_fingerprint(id, 3u) == a);		/* stable */
	CHECK(bc250_oc_board_fingerprint(id, 5u) != a);		/* a firmware change is a board change */
	/* Every single bit of the UUID moves the fingerprint. */
	for (i = 0; i < 16u; i++)
		for (j = 0; j < 8u; j++) {
			id[i] ^= (unsigned char)(1u << j);
			b = bc250_oc_board_fingerprint(id, 3u);
			CHECK(b != a && b != 0u);
			id[i] ^= (unsigned char)(1u << j);
		}
	/* The pinned value of the all-zero UUID on firmware 3: FNV-1a over 20 bytes, so a change of the hash is a change
	 * of every stored binding and needs its own decision. */
	CHECK(a == 0x0A2B2366u || (printf("fingerprint 0x%08X\n", a), 0));
	for (i = 0; i < 100000u && !zero; i++) {
		id[0] = (unsigned char)i; id[1] = (unsigned char)(i >> 8); id[2] = (unsigned char)(i >> 16);
		zero = bc250_oc_board_fingerprint(id, i) == 0u;
	}
	CHECK(!zero);
}

int main(void)
{
	test_envelopes();
	test_decide();
	test_fingerprint();
	printf("oc policy: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
