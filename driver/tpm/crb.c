/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The CRB handshake of the TCG PC Client Platform TPM Profile 2.0, servicing side.
 *
 * The requester drives it: it writes the command into the data buffer, sets CRB_CTRL_CMD_SIZE, then sets
 * CRB_CTRL_START to CRB_START_CMD_READY. The servicer reads the command, answers through the callback, writes the
 * answer into the data buffer, sets CRB_CTRL_RSP_SIZE, and clears CRB_CTRL_START. The requester polls START until
 * it reads 0. Nothing here is an interrupt and nothing waits: the caller calls crb_poll until it returns 0.
 */
#include "crb.h"

static void copy_in(volatile unsigned char *dst, const unsigned char *src, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) dst[i] = (unsigned char)src[i];
}

static void copy_out(unsigned char *dst, const volatile unsigned char *src, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) dst[i] = (unsigned char)src[i];
}

uint32_t crb_read32(const struct crb *c, unsigned locality, unsigned offset)
{
	return *crb_reg(c, locality, offset);
}

void crb_write32(struct crb *c, unsigned locality, unsigned offset, uint32_t value)
{
	*crb_reg(c, locality, offset) = value;
}

void crb_reset(struct crb *c)
{
	c->locality = 0;
	c->response_bytes = 0;
	c->failed = 0;
	crb_write32(c, 0, CRB_INTERFACE_ID, CRB_INTF_ID_CRB);
	crb_write32(c, 0, CRB_CTRL_EXT, 0);
	crb_write32(c, 0, CRB_LOC_STATE, CRB_LOC_STATE_ESTABLISHED);
	crb_write32(c, 0, CRB_LOC_CTRL, 0);
	crb_write32(c, 0, CRB_LOC_STS, 0);
	crb_write32(c, 0, CRB_CTRL_REQ, 0);
	crb_write32(c, 0, CRB_CTRL_STS, 0);
	crb_write32(c, 0, CRB_CTRL_CANCEL, 0);
	crb_write32(c, 0, CRB_CTRL_START, 0);
	crb_write32(c, 0, CRB_INT_ENABLE, 0);
	crb_write32(c, 0, CRB_INT_STS, 0);
	crb_write32(c, 0, CRB_CMD_SIZE, 0);
	crb_write32(c, 0, CRB_RSP_SIZE, 0);
}

int crb_poll(struct crb *c)
{
	unsigned char command[CRB_DATA_BYTES];
	unsigned char response[CRB_DATA_BYTES];
	unsigned locality;
	uint32_t start, size, rsp;
	long got;

	if (c == NULL || c->window == NULL || c->command == NULL) return -1;
	locality = c->locality;
	if (locality >= CRB_LOCALITIES) return -1;
	start = crb_read32(c, locality, CRB_CTRL_START);
	if ((start & CRB_START_CMD_READY) == 0) return 0;
	size = crb_read32(c, locality, CRB_CMD_SIZE);
	if (size == 0 || size > CRB_DATA_BYTES) {
		/* A size that the window cannot hold is a defect of the requester, and it gets an empty answer. */
		crb_write32(c, locality, CRB_RSP_SIZE, 0);
		crb_write32(c, locality, CRB_CTRL_START, 0);
		c->failed = 1;
		c->response_bytes = 0;
		return 1;
	}
	copy_out(command, c->window + (size_t)locality * CRB_LOCALITY_STRIDE + CRB_DATA_BUFFER, size);
	got = c->command(c->context, command, size, response, sizeof(response));
	if (got <= 0) {
		crb_write32(c, locality, CRB_RSP_SIZE, 0);
		crb_write32(c, locality, CRB_CTRL_START, 0);
		c->failed = 1;
		c->response_bytes = 0;
		return 1;
	}
	rsp = (uint32_t)got;
	if (rsp > CRB_DATA_BYTES) rsp = CRB_DATA_BYTES;
	copy_in(c->window + (size_t)locality * CRB_LOCALITY_STRIDE + CRB_DATA_BUFFER, response, rsp);
	crb_write32(c, locality, CRB_RSP_SIZE, rsp);
	crb_write32(c, locality, CRB_CTRL_START, 0);
	c->failed = 0;
	c->response_bytes = rsp;
	return 1;
}

size_t crb_response_bytes(const struct crb *c)
{
	return c->response_bytes;
}
