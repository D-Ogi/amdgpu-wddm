/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The whole chain on the host: a requester writes TPM2 commands into a CRB window exactly as the profile describes,
 * the module's handshake serves them, and the answers come from the libtpms TPM 2 core. This is the measure that the
 * servicer works as a state machine; it is not a measure of Windows, and it does not touch hardware.
 */
#include <stdio.h>
#include <string.h>
#include "crb.h"
#include "libtpms/tpm_library.h"

static unsigned char window[CRB_WINDOW_BYTES] __attribute__((aligned(4096)));
static int failures, checks;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static long core(void *context, const unsigned char *command, size_t bytes, unsigned char *response, size_t capacity)
{
	unsigned char *resp = NULL;
	uint32_t rlen = 0, rbufsize = 0;
	(void)context;
	if (TPMLIB_Process(&resp, &rlen, &rbufsize, (unsigned char *)command, (uint32_t)bytes) != 0) return -1;
	if (resp == NULL || rlen == 0 || rlen > capacity || rlen > 0x80000000u) return -1;
	memcpy(response, resp, rlen);
	return (long)rlen;
}

/* The requester side: write the command into the data buffer, set the size, set START, poll until it reads 0. */
static int request(struct crb *c, const unsigned char *cmd, size_t n, unsigned char *answer, size_t *answer_bytes)
{
	uint32_t rsp;
	memcpy((void *)(window + CRB_DATA_BUFFER), cmd, n);
	crb_write32(c, 0, CRB_CMD_SIZE, (uint32_t)n);
	crb_write32(c, 0, CRB_CTRL_START, CRB_START_CMD_READY);
	if (crb_poll(c) != 1) return 0;
	if (crb_read32(c, 0, CRB_CTRL_START) != 0) return 0;
	rsp = crb_read32(c, 0, CRB_RSP_SIZE);
	if (rsp == 0 || rsp > CRB_DATA_BYTES) return 0;
	memcpy(answer, (const void *)(window + CRB_DATA_BUFFER), rsp);
	*answer_bytes = rsp;
	return 1;
}

int main(void)
{
	static const unsigned char startup[12] = {0x80, 0x01, 0, 0, 0, 12, 0, 0, 0x01, 0x44, 0x00, 0x00};
	static const unsigned char getrandom[12] = {0x80, 0x01, 0, 0, 0, 12, 0, 0, 0x01, 0x7B, 0x00, 0x20};
	struct crb c;
	unsigned char answer[CRB_DATA_BYTES];
	size_t n = 0;

	memset(&c, 0, sizeof(c));
	c.window = window;
	c.command = core;
	crb_reset(&c);
	CHECK(TPMLIB_ChooseTPMVersion(TPMLIB_TPM_VERSION_2) == 0);
	CHECK(TPMLIB_MainInit() == 0);

	/* TPM2_Startup through the window: the answer is ten bytes with a TPM_RC of 0. */
	CHECK(request(&c, startup, sizeof(startup), answer, &n));
	CHECK(n == 10 && answer[0] == 0x80 && answer[1] == 0x01 && answer[6] == 0 && answer[7] == 0 &&
	      answer[8] == 0 && answer[9] == 0);

	/* TPM2_GetRandom through the same window: 32 random bytes in a TPM2B_DIGEST. */
	CHECK(request(&c, getrandom, sizeof(getrandom), answer, &n));
	CHECK(n == 44 && answer[0] == 0x80 && answer[1] == 0x01 && answer[10] == 0 && answer[11] == 32);

	printf("crb bridge: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
