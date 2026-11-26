/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The TPM side of the PTP CRB interface. The contract is in crb.h. */
#include <string.h>
#include "crb.h"

crb_u32 crb_read32(const struct crb *c, unsigned offset)
{
	return *(volatile const crb_u32 *)(const void *)(c->window + offset);
}

void crb_write32(struct crb *c, unsigned offset, crb_u32 value)
{
	*(volatile crb_u32 *)(void *)(c->window + offset) = value;
}

static crb_u32 be32(const volatile unsigned char *p)
{
	return ((crb_u32)p[0] << 24) | ((crb_u32)p[1] << 16) | ((crb_u32)p[2] << 8) | (crb_u32)p[3];
}

/* The data buffer gets bytes one at a time: it is uncached memory, and a byte loop has no alignment demand. */
static void put_bytes(struct crb *c, const unsigned char *src, size_t bytes)
{
	volatile unsigned char *dst = c->window + CRB_DATA_BUFFER;
	size_t i;
	for (i = 0; i < bytes; i++) dst[i] = src[i];
}

static void finish(struct crb *c)
{
	/* The response bytes are in the buffer before START is clear: the requester reads the response after it sees
	 * START = 0 (PTP CRB handshake). */
	crb_write32(c, CRB_CTRL_START, 0);
	c->busy = 0;
}

/* A response with no parameters: tag TPM_ST_NO_SESSIONS, size 10, then the response code. */
static void answer_rc(struct crb *c, crb_u32 rc)
{
	unsigned char rsp[CRB_TPM_HEADER_BYTES] = {0x80, 0x01, 0x00, 0x00, 0x00, 0x0A, 0, 0, 0, 0};
	rsp[6] = (unsigned char)((rc >> 24) & 0xFFu);
	rsp[7] = (unsigned char)((rc >> 16) & 0xFFu);
	rsp[8] = (unsigned char)((rc >> 8) & 0xFFu);
	rsp[9] = (unsigned char)(rc & 0xFFu);
	put_bytes(c, rsp, sizeof(rsp));
	finish(c);
}

/* The answer to a command that does not fit. */
static void answer_bad_size(struct crb *c)
{
	answer_rc(c, CRB_TPM_RC_COMMAND_SIZE);
}

void crb_reset(struct crb *c, volatile void *window, crb_u64 page_phys)
{
	crb_u64 buffer = page_phys + CRB_DATA_BUFFER;
	unsigned o;

	memset(c, 0, sizeof(*c));
	c->window = (volatile unsigned char *)window;
	for (o = 0; o < CRB_DATA_BUFFER; o += 4) crb_write32(c, o, 0);
	crb_write32(c, CRB_INTERFACE_ID, CRB_INTERFACE_ID_VALUE);
	crb_write32(c, CRB_VID_DID, 0);		/* no vendor identity is claimed; the core reports its own */
	crb_write32(c, CRB_LOC_STATE, CRB_LOC_STATE_VALID | CRB_LOC_STATE_ESTABLISHED);
	crb_write32(c, CRB_CTRL_STS, CRB_STS_IDLE);
	crb_write32(c, CRB_CMD_SIZE, CRB_DATA_BYTES);
	crb_write32(c, CRB_CMD_LADDR, (crb_u32)buffer);
	crb_write32(c, CRB_CMD_HADDR, (crb_u32)(buffer >> 32));
	crb_write32(c, CRB_RSP_SIZE, CRB_DATA_BYTES);
	crb_write32(c, CRB_RSP_LADDR, (crb_u32)buffer);
	crb_write32(c, CRB_RSP_HADDR, (crb_u32)(buffer >> 32));
}

static void serve_locality(struct crb *c)
{
	crb_u32 ctrl = crb_read32(c, CRB_LOC_CTRL);

	if (ctrl == 0) return;
	crb_write32(c, CRB_LOC_CTRL, 0);
	if (ctrl & (CRB_LOC_CTRL_REQUEST | CRB_LOC_CTRL_SEIZE)) {
		/* Locality 0 is the only one, so a request is granted at once and nothing is seized from anybody. */
		c->count.locality_requests++;
		crb_write32(c, CRB_LOC_STS, CRB_LOC_STS_GRANTED);
		crb_write32(c, CRB_LOC_STATE, CRB_LOC_STATE_VALID | CRB_LOC_STATE_ESTABLISHED | CRB_LOC_STATE_ASSIGNED);
	} else if (ctrl & CRB_LOC_CTRL_RELINQUISH) {
		c->count.locality_relinquish++;
		crb_write32(c, CRB_LOC_STS, 0);
		crb_write32(c, CRB_LOC_STATE, CRB_LOC_STATE_VALID | CRB_LOC_STATE_ESTABLISHED);
	}
	/* RESET_ESTABLISHMENT: the bit stays 1, as no dynamic launch can clear it on this platform. */
}

static void serve_request(struct crb *c)
{
	crb_u32 req = crb_read32(c, CRB_CTRL_REQ);

	if (req == 0 || c->busy) return;	/* the PTP forbids both requests while a command runs */
	if (req & CRB_REQ_CMD_READY) {
		c->count.ready++;
		crb_write32(c, CRB_CTRL_STS, 0);
	} else if (req & CRB_REQ_GO_IDLE) {
		c->count.idle++;
		crb_write32(c, CRB_CTRL_STS, CRB_STS_IDLE);
	}
	crb_write32(c, CRB_CTRL_REQ, 0);	/* the TPM clears the bit when the state change is complete */
}

enum crb_poll_result crb_poll(struct crb *c, unsigned char *command, size_t capacity, size_t *command_bytes)
{
	crb_u32 size;
	size_t i;
	const volatile unsigned char *buffer = c->window + CRB_DATA_BUFFER;

	*command_bytes = 0;
	serve_locality(c);
	serve_request(c);
	if (c->busy) {
		/* The PTP lets the TPM finish a command that software cancels; the core cannot stop one, so a
		 * cancel only counts. Software clears CANCEL itself after START goes to 0. */
		if (crb_read32(c, CRB_CTRL_CANCEL) & CRB_CANCEL) c->count.cancels++;
		return CRB_POLL_NOTHING;
	}
	if (!(crb_read32(c, CRB_CTRL_START) & CRB_START)) return CRB_POLL_NOTHING;

	c->busy = 1;
	/* With CapCRBIdleBypass = 0 the requester must send cmdReady first. A START in the idle state is served
	 * anyway and counted: refusing it would hang the requester, and a counter shows the deviation. */
	if (crb_read32(c, CRB_CTRL_STS) & CRB_STS_IDLE) {
		c->count.start_while_idle++;
		crb_write32(c, CRB_CTRL_STS, 0);
	}
	/* The command length is commandSize in the header (big endian, bytes 2..5), not CMD_SIZE: CMD_SIZE is the
	 * size of the buffer, which the TPM states. */
	size = be32(buffer + 2);
	if (size < CRB_TPM_HEADER_BYTES || size > CRB_DATA_BYTES || size > capacity) {
		c->count.bad_size++;
		answer_bad_size(c);
		return CRB_POLL_NOTHING;
	}
	for (i = 0; i < size; i++) command[i] = buffer[i];
	*command_bytes = size;
	c->count.commands++;
	return CRB_POLL_COMMAND;
}

void crb_complete(struct crb *c, const unsigned char *response, size_t response_bytes)
{
	if (!c->busy) return;
	if (response == NULL || response_bytes < CRB_TPM_HEADER_BYTES || response_bytes > CRB_DATA_BYTES) {
		c->count.bad_size++;
		answer_bad_size(c);
		return;
	}
	put_bytes(c, response, response_bytes);
	finish(c);
}

void crb_fail(struct crb *c, crb_u32 rc)
{
	if (!c->busy) return;
	c->count.failed++;
	answer_rc(c, rc);
}
