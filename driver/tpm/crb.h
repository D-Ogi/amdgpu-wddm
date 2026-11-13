/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The command response buffer (CRB) interface of the TCG PC Client Platform TPM Profile (PTP), seen from the TPM
 * side. The register map and the bits come from edk2 MdePkg/Include/IndustryStandard/TpmPtp.h (PTP_CRB_REGISTERS,
 * PTP_CRB_INTERFACE_IDENTIFIER and the PTP_CRB_* bits), not from memory.
 *
 * The window is one 4 KiB page of locality 0: the registers at 0x00..0x7F, the data buffer at 0x80..0xFFF. The
 * command and the response share the data buffer, which the PTP permits (CMD_ADDR may equal RSP_ADDR). The page
 * has no locality 1..4, so the interface identifier states CapLocality = 0 (locality 0 only).
 *
 * This module is the state machine only. It reads and writes a window that the caller maps, and it holds no
 * operating system call, so a host test drives both sides. The answer of a command is asynchronous: crb_poll hands
 * out a command, the caller gets the answer from the TPM core at any later time, and crb_complete writes it back.
 *
 * Concurrency: the requester (tpm.sys) and this module write different registers, except CTRL_REQ, LOC_CTRL and
 * START, where the requester sets a bit and this module clears it after it acts (the PTP handshake). Every
 * register access is one aligned 32-bit access through a volatile pointer. crb_complete writes the response bytes
 * before it clears START; the caller must map the window uncached, as the requester does, so that order holds.
 */
#ifndef BC250_CRB_H
#define BC250_CRB_H
#include <stddef.h>

/* The kernel build has no stdint.h (WDK km\crt), so the module names its two fixed widths itself. Both hold on
 * every target of this project (LLP64 Windows, LP64 host test). */
typedef unsigned int crb_u32;
typedef unsigned long long crb_u64;
typedef char crb_u32_is_32_bits[sizeof(crb_u32) == 4 ? 1 : -1];
typedef char crb_u64_is_64_bits[sizeof(crb_u64) == 8 ? 1 : -1];

#define CRB_PAGE_BYTES		0x1000u

/* Register offsets of locality 0 (PTP_CRB_REGISTERS). */
#define CRB_LOC_STATE		0x00u
#define CRB_LOC_CTRL		0x08u
#define CRB_LOC_STS		0x0Cu
#define CRB_INTERFACE_ID	0x30u
#define CRB_VID_DID		0x34u
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
#define CRB_RSP_LADDR		0x68u
#define CRB_RSP_HADDR		0x6Cu
#define CRB_DATA_BUFFER		0x80u
#define CRB_DATA_BYTES		(CRB_PAGE_BYTES - CRB_DATA_BUFFER)	/* 0xF80, the PTP maximum */

/* The control area that the ACPI TPM2 table names for start method 7 is the page plus this offset. */
#define CRB_CONTROL_AREA	CRB_CTRL_REQ

/* TPM_LOC_STATE_0 */
#define CRB_LOC_STATE_ESTABLISHED	0x01u	/* 1 until a _TPM_Hash_End; no dynamic launch here */
#define CRB_LOC_STATE_ASSIGNED		0x02u
#define CRB_LOC_STATE_ACTIVE_MASK	0x1Cu	/* bits 4:2; locality 0 is 0 */
#define CRB_LOC_STATE_VALID		0x80u
/* TPM_LOC_CTRL_0 */
#define CRB_LOC_CTRL_REQUEST		0x01u
#define CRB_LOC_CTRL_RELINQUISH		0x02u
#define CRB_LOC_CTRL_SEIZE		0x04u
#define CRB_LOC_CTRL_RESET_ESTABLISHMENT 0x08u
/* TPM_LOC_STS_0 */
#define CRB_LOC_STS_GRANTED		0x01u
#define CRB_LOC_STS_BEEN_SEIZED		0x02u
/* TPM_CRB_CTRL_REQ_0 */
#define CRB_REQ_CMD_READY		0x01u
#define CRB_REQ_GO_IDLE			0x02u
/* TPM_CRB_CTRL_STS_0 */
#define CRB_STS_FATAL			0x01u	/* tpmSts */
#define CRB_STS_IDLE			0x02u	/* tpmIdle */
/* TPM_CRB_CTRL_CANCEL_0, TPM_CRB_CTRL_START_0 */
#define CRB_CANCEL			0x01u
#define CRB_START			0x01u

/* TPM_CRB_INTF_ID_0 bits 31:0: InterfaceType 1 (CRB), InterfaceVersion 1 (CRB), CapLocality 0, CapCRBIdleBypass 0,
 * CapDataXferSizeSupport 3 (64 bytes), CapFIFO 0, CapCRB 1, InterfaceSelector 1 (CRB), IntfSelLock 1, RID 0. */
#define CRB_INTERFACE_ID_VALUE	(0x1u | (0x1u << 4) | (0x3u << 11) | (0x1u << 14) | (0x1u << 17) | (0x1u << 19))

/* The smallest TPM 2.0 command: tag (2), commandSize (4), commandCode (4). */
#define CRB_TPM_HEADER_BYTES	10u
/* TPM_RC_COMMAND_SIZE, the answer to a command whose header size does not fit the buffer it came in. */
#define CRB_TPM_RC_COMMAND_SIZE	0x142u

/* Counters for the diagnostics of the caller. They only grow. */
struct crb_counters {
	crb_u32 commands;		/* commands handed out by crb_poll */
	crb_u32 bad_size;		/* commands answered here with TPM_RC_COMMAND_SIZE */
	crb_u32 start_while_idle;	/* START seen while tpmIdle was set; served anyway, see crb.c */
	crb_u32 cancels;		/* CANCEL seen while a command was out */
	crb_u32 ready, idle;		/* cmdReady and goIdle requests */
	crb_u32 locality_requests, locality_relinquish;
};

struct crb {
	volatile unsigned char *window;	/* the mapped page */
	int busy;			/* 1 from crb_poll handing out a command to crb_complete */
	struct crb_counters count;
};

enum crb_poll_result {
	CRB_POLL_NOTHING = 0,		/* no command; handshake requests may have been served */
	CRB_POLL_COMMAND = 1,		/* a command is in the caller buffer; call crb_complete later */
};

crb_u32 crb_read32(const struct crb *c, unsigned offset);
void crb_write32(struct crb *c, unsigned offset, crb_u32 value);

/* Puts the page into the state of a TPM after reset: interface identifier, locality 0 not assigned, tpmIdle set,
 * no request, the command and the response buffer at page_phys + 0x80 with 0xF80 bytes each. page_phys is the
 * physical address that the requester uses for CMD_ADDR and RSP_ADDR. */
void crb_reset(struct crb *c, volatile void *window, crb_u64 page_phys);

/* Serves the handshake requests once (locality, cmdReady, goIdle) and, when START is set and no command is out,
 * copies the command into command[0..capacity) and returns CRB_POLL_COMMAND with *command_bytes set. A command
 * whose header size is below 10 or above the buffer is answered here with TPM_RC_COMMAND_SIZE. */
enum crb_poll_result crb_poll(struct crb *c, unsigned char *command, size_t capacity, size_t *command_bytes);

/* Writes the answer of the command that crb_poll handed out and clears START. A response larger than the data
 * buffer, or one shorter than a TPM header, is replaced with a TPM_RC_COMMAND_SIZE answer (the core broke its
 * buffer contract; see TPMLIB_SetBufferSize in the core). Does nothing when no command is out. */
void crb_complete(struct crb *c, const unsigned char *response, size_t response_bytes);

/* 1 when a command is out. */
static inline int crb_busy(const struct crb *c) { return c->busy; }

#endif
