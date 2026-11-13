/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host test of the CRB state machine: the window is a byte array, the requester side is written by the test, and
 * the servicer side is the module. The test holds the PTP rules that the module must not break: one locality is
 * one 4 KiB page, the registers sit at the start of the page, and the answer follows the START handshake.
 */
#include <stdio.h>
#include <string.h>
#include "crb.h"

static int failures;
static int checks;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static unsigned char window[CRB_WINDOW_BYTES] __attribute__((aligned(4096)));

/* One command core: it answers TPM2_Startup-like bytes and remembers what it was asked. */
static unsigned char last_command[64];
static size_t last_bytes;

static long answer(void *context, const unsigned char *command, size_t bytes, unsigned char *response, size_t cap)
{
	(void)context;
	if (bytes > sizeof(last_command)) return -1;
	memcpy(last_command, command, bytes);
	last_bytes = bytes;
	if (cap < 10 || bytes < 10) return -1;
	memcpy(response, "\x80\x01\x00\x00\x00\x0a\x00\x00\x00\x00", 10);
	return 10;
}

int main(void)
{
	struct crb c;
	unsigned n;

	/* The stride: the register of locality n is at n * 0x1000 + offset. */
	memset(&c, 0, sizeof(c));
	c.window = window;
	for (n = 0; n < CRB_LOCALITIES; n++) {
		CHECK((const unsigned char *)crb_reg(&c, n, CRB_CTRL_REQ) == window + n * CRB_LOCALITY_STRIDE + 0x40);
		CHECK((const unsigned char *)crb_reg(&c, n, CRB_DATA_BUFFER) == window + n * CRB_LOCALITY_STRIDE + 0x80);
	}
	CHECK(CRB_LOCALITIES * CRB_LOCALITY_STRIDE == 0x5000u);
	CHECK(CRB_DATA_BYTES == 0xF80u);

	/* A reset leaves the window that the PTP describes for locality 0. */
	c.command = answer;
	crb_reset(&c);
	CHECK(crb_read32(&c, 0, CRB_INTERFACE_ID) == CRB_INTF_ID_CRB);
	CHECK(crb_read32(&c, 0, CRB_LOC_STATE) == CRB_LOC_STATE_ESTABLISHED);
	CHECK(crb_read32(&c, 0, CRB_CTRL_START) == 0);
	CHECK(crb_poll(&c) == 0);			/* nothing pending */

	/* The requester writes a command and sets START; the servicer answers and clears START. */
	window[CRB_DATA_BUFFER + 0] = 0x80;
	window[CRB_DATA_BUFFER + 1] = 0x01;
	memset((void *)(window + CRB_DATA_BUFFER + 2), 0, 8);
	crb_write32(&c, 0, CRB_CMD_SIZE, 12);
	crb_write32(&c, 0, CRB_CTRL_START, CRB_START_CMD_READY);
	CHECK(crb_poll(&c) == 1);
	CHECK(last_bytes == 12 && last_command[0] == 0x80 && last_command[1] == 0x01);
	CHECK(crb_read32(&c, 0, CRB_CTRL_START) == 0);
	CHECK(crb_read32(&c, 0, CRB_RSP_SIZE) == 10);
	CHECK(window[CRB_DATA_BUFFER] == 0x80 && window[CRB_DATA_BUFFER + 9] == 0x00);
	CHECK(crb_response_bytes(&c) == 10);
	CHECK(crb_poll(&c) == 0);			/* served once, not twice */

	/* A size that the data buffer cannot hold gets an empty answer and the failure flag, not a read past it. */
	crb_write32(&c, 0, CRB_CMD_SIZE, CRB_DATA_BYTES + 1u);
	crb_write32(&c, 0, CRB_CTRL_START, CRB_START_CMD_READY);
	CHECK(crb_poll(&c) == 1);
	CHECK(crb_read32(&c, 0, CRB_RSP_SIZE) == 0);
	CHECK(crb_read32(&c, 0, CRB_CTRL_START) == 0);
	CHECK(c.failed == 1);

	printf("crb: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
