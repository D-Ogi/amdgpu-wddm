/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host test of the CRB state machine. The page is a byte array; the test plays the requester (tpm.sys) through the
 * PTP sequence: request locality 0, cmdReady, write the command, START, wait for START = 0, read the response,
 * goIdle, relinquish. The module plays the TPM. Bit values come from crb.h, which takes them from edk2 TpmPtp.h.
 */
#include <stdio.h>
#include <string.h>
#include "../crb.h"

static int failures;
static int checks;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static unsigned char page[CRB_PAGE_BYTES] __attribute__((aligned(4096)));
static unsigned char cmd[CRB_DATA_BYTES];

static crb_u32 rd(unsigned o) { crb_u32 v; memcpy(&v, page + o, 4); return v; }
static void wr(unsigned o, crb_u32 v) { memcpy(page + o, &v, 4); }

static void put_command(const unsigned char *bytes, size_t n) { memcpy(page + CRB_DATA_BUFFER, bytes, n); }

static void test_reset(struct crb *c)
{
	memset(page, 0xA5, sizeof(page));
	crb_reset(c, page, 0x7DDD3000ull);
	CHECK((rd(CRB_INTERFACE_ID) & 0xFu) == 1u);		/* InterfaceType CRB */
	CHECK(((rd(CRB_INTERFACE_ID) >> 4) & 0xFu) == 1u);	/* InterfaceVersion CRB */
	CHECK(((rd(CRB_INTERFACE_ID) >> 8) & 1u) == 0u);	/* CapLocality: locality 0 only */
	CHECK(((rd(CRB_INTERFACE_ID) >> 14) & 1u) == 1u);	/* CapCRB */
	CHECK(((rd(CRB_INTERFACE_ID) >> 17) & 3u) == 1u);	/* InterfaceSelector CRB */
	CHECK(rd(CRB_LOC_STATE) == (CRB_LOC_STATE_VALID | CRB_LOC_STATE_ESTABLISHED));
	CHECK(rd(CRB_LOC_STS) == 0);
	CHECK(rd(CRB_CTRL_STS) == CRB_STS_IDLE);
	CHECK(rd(CRB_CTRL_REQ) == 0 && rd(CRB_CTRL_START) == 0 && rd(CRB_CTRL_CANCEL) == 0);
	CHECK(rd(CRB_CMD_SIZE) == 0xF80u && rd(CRB_RSP_SIZE) == 0xF80u);
	CHECK(rd(CRB_CMD_LADDR) == 0x7DDD3080u && rd(CRB_CMD_HADDR) == 0);
	CHECK(rd(CRB_RSP_LADDR) == 0x7DDD3080u && rd(CRB_RSP_HADDR) == 0);
	CHECK(page[CRB_DATA_BUFFER] == 0xA5);		/* reset leaves the data buffer alone */
	CHECK(!crb_busy(c));
}

static void test_locality_and_ready(struct crb *c)
{
	size_t n;

	wr(CRB_LOC_CTRL, CRB_LOC_CTRL_REQUEST);
	CHECK(crb_poll(c, cmd, sizeof(cmd), &n) == CRB_POLL_NOTHING);
	CHECK(rd(CRB_LOC_CTRL) == 0);
	CHECK(rd(CRB_LOC_STS) == CRB_LOC_STS_GRANTED);
	CHECK(rd(CRB_LOC_STATE) & CRB_LOC_STATE_ASSIGNED);
	CHECK((rd(CRB_LOC_STATE) & CRB_LOC_STATE_ACTIVE_MASK) == 0);

	wr(CRB_CTRL_REQ, CRB_REQ_CMD_READY);
	CHECK(crb_poll(c, cmd, sizeof(cmd), &n) == CRB_POLL_NOTHING);
	CHECK(rd(CRB_CTRL_REQ) == 0);
	CHECK((rd(CRB_CTRL_STS) & CRB_STS_IDLE) == 0);
}

static void test_command(struct crb *c)
{
	/* TPM2_GetRandom(32) and a 44-byte answer as libtpms gives it. */
	static const unsigned char getrandom[12] = {0x80, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x01, 0x7B, 0x00, 0x20};
	unsigned char rsp[44];
	size_t n = 99;

	memset(rsp, 0x5A, sizeof(rsp));
	memcpy(rsp, "\x80\x01\x00\x00\x00\x2c\x00\x00\x00\x00\x00\x20", 12);
	put_command(getrandom, sizeof(getrandom));
	CHECK(crb_poll(c, cmd, sizeof(cmd), &n) == CRB_POLL_NOTHING);	/* no START yet */
	CHECK(n == 0);
	wr(CRB_CTRL_START, CRB_START);
	CHECK(crb_poll(c, cmd, sizeof(cmd), &n) == CRB_POLL_COMMAND);
	CHECK(n == 12 && memcmp(cmd, getrandom, 12) == 0);		/* the length comes from the header */
	CHECK(crb_busy(c));
	CHECK(rd(CRB_CTRL_START) == CRB_START);			/* START stays set while the core works */

	/* goIdle while busy waits; a second poll hands out nothing. */
	wr(CRB_CTRL_REQ, CRB_REQ_GO_IDLE);
	CHECK(crb_poll(c, cmd, sizeof(cmd), &n) == CRB_POLL_NOTHING);
	CHECK(rd(CRB_CTRL_REQ) == CRB_REQ_GO_IDLE);
	wr(CRB_CTRL_CANCEL, CRB_CANCEL);
	CHECK(crb_poll(c, cmd, sizeof(cmd), &n) == CRB_POLL_NOTHING);
	CHECK(c->count.cancels == 1);

	crb_complete(c, rsp, sizeof(rsp));
	CHECK(rd(CRB_CTRL_START) == 0);
	CHECK(!crb_busy(c));
	CHECK(memcmp(page + CRB_DATA_BUFFER, rsp, sizeof(rsp)) == 0);
	wr(CRB_CTRL_CANCEL, 0);

	/* The goIdle that waited is served now. */
	CHECK(crb_poll(c, cmd, sizeof(cmd), &n) == CRB_POLL_NOTHING);
	CHECK(rd(CRB_CTRL_REQ) == 0);
	CHECK(rd(CRB_CTRL_STS) == CRB_STS_IDLE);
	CHECK(c->count.commands == 1 && c->count.idle == 1 && c->count.ready == 1);

	/* A second complete without a command does nothing. */
	crb_complete(c, rsp, sizeof(rsp));
	CHECK(rd(CRB_CTRL_START) == 0);
}

static void expect_bad_size_answer(void)
{
	CHECK(rd(CRB_CTRL_START) == 0);
	CHECK(memcmp(page + CRB_DATA_BUFFER, "\x80\x01\x00\x00\x00\x0a\x00\x00\x01\x42", 10) == 0);
}

static void test_bad_sizes(struct crb *c)
{
	size_t n;
	unsigned char big[CRB_DATA_BYTES + 1];

	/* A header size below 10. */
	put_command((const unsigned char *)"\x80\x01\x00\x00\x00\x08\x00\x00\x01\x7b", 10);
	wr(CRB_CTRL_START, CRB_START);
	CHECK(crb_poll(c, cmd, sizeof(cmd), &n) == CRB_POLL_NOTHING);
	expect_bad_size_answer();
	/* A header size above the buffer. */
	put_command((const unsigned char *)"\x80\x01\x00\x00\x0f\x81\x00\x00\x01\x7b", 10);
	wr(CRB_CTRL_START, CRB_START);
	CHECK(crb_poll(c, cmd, sizeof(cmd), &n) == CRB_POLL_NOTHING);
	expect_bad_size_answer();
	/* A header size above the caller's buffer. */
	put_command((const unsigned char *)"\x80\x01\x00\x00\x00\x20\x00\x00\x01\x7b", 10);
	wr(CRB_CTRL_START, CRB_START);
	CHECK(crb_poll(c, cmd, 16, &n) == CRB_POLL_NOTHING);
	expect_bad_size_answer();
	CHECK(c->count.bad_size == 3);
	/* START while idle is served, counted, and takes the TPM out of idle. The bad-size STARTs above came in the
	 * idle state, so the first of them left idle; go idle again first. */
	CHECK(c->count.start_while_idle == 1);
	wr(CRB_CTRL_REQ, CRB_REQ_GO_IDLE);
	CHECK(crb_poll(c, cmd, sizeof(cmd), &n) == CRB_POLL_NOTHING);
	CHECK(rd(CRB_CTRL_STS) == CRB_STS_IDLE);
	put_command((const unsigned char *)"\x80\x01\x00\x00\x00\x0a\x00\x00\x01\x7b", 10);
	wr(CRB_CTRL_START, CRB_START);
	CHECK(crb_poll(c, cmd, sizeof(cmd), &n) == CRB_POLL_COMMAND && n == 10);
	CHECK(c->count.start_while_idle >= 1);
	/* A core answer larger than the buffer becomes TPM_RC_COMMAND_SIZE. */
	memset(big, 0, sizeof(big));
	crb_complete(c, big, sizeof(big));
	expect_bad_size_answer();
	CHECK(!crb_busy(c));
}

static void test_relinquish(struct crb *c)
{
	size_t n;
	wr(CRB_LOC_CTRL, CRB_LOC_CTRL_RELINQUISH);
	CHECK(crb_poll(c, cmd, sizeof(cmd), &n) == CRB_POLL_NOTHING);
	CHECK(rd(CRB_LOC_STS) == 0);
	CHECK((rd(CRB_LOC_STATE) & CRB_LOC_STATE_ASSIGNED) == 0);
	CHECK(rd(CRB_LOC_STATE) & CRB_LOC_STATE_VALID);
}

int main(void)
{
	struct crb c;

	CHECK(CRB_CONTROL_AREA == 0x40u);
	CHECK(CRB_DATA_BYTES == 0xF80u);
	test_reset(&c);
	test_locality_and_ready(&c);
	test_command(&c);
	test_bad_sizes(&c);
	test_relinquish(&c);
	printf("crb_test: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
