/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The command response buffer (CRB) of the TCG PC Client Platform TPM Profile 2.0, as the servicing side sees it.
 * The register map and the page stride come from the PTP through MdePkg\Include\IndustryStandard\TpmPtp.h: one
 * locality is one 4 KiB page, the registers sit at the start of the page, and the data buffer runs from 0x80 to
 * the end of the page. Locality n is at base + n * 0x1000.
 *
 * This module is the state machine only: it reads and writes a window of memory that somebody else maps, and it
 * asks a callback for the answer of one command. It holds no hardware access and no operating system call, so a
 * host test can drive both sides of it.
 */
#ifndef BC250_CRB_H
#define BC250_CRB_H
#include <stddef.h>
#include <stdint.h>

#define CRB_LOCALITY_STRIDE	0x1000u
#define CRB_LOCALITIES		5u
#define CRB_WINDOW_BYTES	(CRB_LOCALITY_STRIDE * CRB_LOCALITIES)

/* Locality 0 registers, as offsets inside the page of the locality. */
#define CRB_LOC_STATE		0x00u
#define CRB_LOC_CTRL		0x08u
#define CRB_LOC_STS		0x0Cu
#define CRB_INTERFACE_ID	0x30u
#define CRB_CTRL_EXT		0x38u
#define CRB_CTRL_REQ		0x40u
#define CRB_CTRL_STS		0x44u
#define CRB_CTRL_CANCEL		0x48u
#define CRB_CTRL_START		0x4Cu
#define CRB_INT_ENABLE		0x50u
#define CRB_INT_STS		0x54u
#define CRB_CMD_SIZE		0x58u
#define CRB_CMD_LADDR		0x5Cu
#define CRB_CMD_HADDR		0x60u
#define CRB_RSP_SIZE		0x64u
#define CRB_RSP_ADDR		0x68u
#define CRB_RSP_HADDR		0x6Cu
#define CRB_DATA_BUFFER		0x80u
#define CRB_DATA_BYTES		(CRB_LOCALITY_STRIDE - CRB_DATA_BUFFER)

/* The bits of the two registers that decide the handshake. */
#define CRB_REQ_GO_READY	0x1u
#define CRB_REQ_GO_IDLE		0x2u
#define CRB_START_CMD_READY	0x1u
#define CRB_INTF_ID_CRB		0x00u	/* bits 3:0: interface type 0 is CRB on the TPM 2.0 profile */
#define CRB_INTF_ID_INTF_SEL	0x04u	/* bit 2: interface selection, 0 = CRB, 1 = FIFO */
#define CRB_LOC_STATE_ESTABLISHED	0x1u

/* The answer of one command: the callee fills response with at most capacity bytes and returns the count, or -1. */
typedef long (*crb_command_fn)(void *context, const unsigned char *command, size_t command_bytes,
			       unsigned char *response, size_t capacity);

struct crb {
	volatile unsigned char *window;
	crb_command_fn command;		/* the core that answers */
	void *context;
	unsigned locality;		/* the locality of the command in progress, 0 when idle */
	size_t response_bytes;		/* the answer that the last command left for the requester */
	int failed;			/* 1: the core refused; the next start returns an error status */
};

/* The address of a register of a locality. The stride is the PTP rule, and the tests hold it. */
static inline volatile uint32_t *crb_reg(const struct crb *c, unsigned locality, unsigned offset)
{
	return (volatile uint32_t *)(void *)(c->window + (size_t)locality * CRB_LOCALITY_STRIDE + offset);
}

/* Reads and writes a field of the window, so that the caller never touches the pointer arithmetic. */
uint32_t crb_read32(const struct crb *c, unsigned locality, unsigned offset);
void crb_write32(struct crb *c, unsigned locality, unsigned offset, uint32_t value);

/* Puts the window into the state that a TPM reset leaves it in: interface identity, locality 0 established and
 * idle, no command pending. */
void crb_reset(struct crb *c);

/* Runs the handshake that the requester drives, once. Returns 1 when it served one command, 0 when there was
 * nothing to do, -1 when the window is not in a state this module understands. */
int crb_poll(struct crb *c);

/* The bytes of the answer that the last served command left. */
size_t crb_response_bytes(const struct crb *c);

#endif
