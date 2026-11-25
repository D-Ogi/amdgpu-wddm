/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The servicer chain on the host: the test writes TPM 2.0 commands into a CRB page as tpm.sys does (PTP order:
 * locality, cmdReady, command, START, wait for START = 0, response, goIdle), crb.c serves the page, and tpmcore.c
 * (libtpms) answers. The kernel filter only moves bytes between these two, so this is the whole TPM behaviour of
 * the servicer except the transport. It does not touch hardware.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../crb.h"
#include "../core/tpmcore.h"

static unsigned char page[CRB_PAGE_BYTES] __attribute__((aligned(4096)));
static int failures, checks;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static crb_u32 rd(unsigned o) { crb_u32 v; memcpy(&v, page + o, 4); return v; }
static void wr(unsigned o, crb_u32 v) { memcpy(page + o, &v, 4); }
static unsigned be32(const unsigned char *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }

/* The servicer side of one poll tick: what the filter timer and the service do together. */
static void tick(struct crb *c)
{
	static unsigned char cmd[CRB_DATA_BYTES], rsp[CRB_DATA_BYTES];
	size_t n, r;
	if (crb_poll(c, cmd, sizeof(cmd), &n) != CRB_POLL_COMMAND) return;
	r = tpmcore_process(cmd, n, rsp, sizeof(rsp));
	crb_complete(c, rsp, r);
}

/* The requester: one command, PTP order. Returns the response size, 0 on a handshake failure. */
static size_t request(struct crb *c, const unsigned char *cmd, size_t n, unsigned char *answer)
{
	int k;
	size_t size;
	wr(CRB_CTRL_REQ, CRB_REQ_CMD_READY);
	for (k = 0; k < 3 && rd(CRB_CTRL_REQ) != 0; k++) tick(c);
	if (rd(CRB_CTRL_REQ) != 0 || (rd(CRB_CTRL_STS) & CRB_STS_IDLE)) return 0;
	if (n > rd(CRB_CMD_SIZE)) return 0;
	memcpy(page + CRB_DATA_BUFFER, cmd, n);
	wr(CRB_CTRL_START, CRB_START);
	for (k = 0; k < 3 && rd(CRB_CTRL_START) != 0; k++) tick(c);
	if (rd(CRB_CTRL_START) != 0) return 0;
	size = be32(page + CRB_DATA_BUFFER + 2);
	if (size < 10 || size > rd(CRB_RSP_SIZE)) return 0;
	memcpy(answer, page + CRB_DATA_BUFFER, size);
	wr(CRB_CTRL_REQ, CRB_REQ_GO_IDLE);
	for (k = 0; k < 3 && rd(CRB_CTRL_REQ) != 0; k++) tick(c);
	return size;
}

int main(void)
{
	static const unsigned char startup[12] = {0x80, 0x01, 0, 0, 0, 12, 0, 0, 0x01, 0x44, 0x00, 0x00};
	static const unsigned char getrandom[12] = {0x80, 0x01, 0, 0, 0, 12, 0, 0, 0x01, 0x7B, 0x00, 0x20};
	/* TPM2_GetCapability(TPM_CAP_TPM_PROPERTIES, TPM_PT_MAX_COMMAND_SIZE 0x11E, 2): the two buffer limits. */
	static const unsigned char getcap[22] = {0x80, 0x01, 0, 0, 0, 22, 0, 0, 0x01, 0x7A, 0, 0, 0, 6, 0, 0, 0x01, 0x1E, 0, 0, 0, 2};
	unsigned char answer[CRB_DATA_BYTES];
	wchar_t dir[MAX_PATH], blob[MAX_PATH];
	struct crb c;
	size_t n;
	int k;

	GetTempPathW(MAX_PATH, dir);
	wcscat(dir, L"bc250-tpmcore-test");
	_snwprintf(blob, MAX_PATH, L"%ls\\permall", dir);
	DeleteFileW(blob);
	CHECK(tpmcore_start(dir, CRB_DATA_BYTES) == 0);
	crb_reset(&c, page, 0x7DDD3000ull);
	wr(CRB_LOC_CTRL, CRB_LOC_CTRL_REQUEST);
	tick(&c);
	CHECK(rd(CRB_LOC_STS) & CRB_LOC_STS_GRANTED);

	/* tpm.sys may send its own TPM2_Startup: the core ran one already, so the answer is TPM_RC_INITIALIZE. */
	n = request(&c, startup, sizeof(startup), answer);
	CHECK(n == 10 && be32(answer + 6) == 0x100u);

	n = request(&c, getrandom, sizeof(getrandom), answer);
	CHECK(n == 44 && be32(answer + 6) == 0 && answer[10] == 0 && answer[11] == 32);

	/* The core states the CRB buffer as its limits, so tpm.sys never sends a command larger than the page. */
	n = request(&c, getcap, sizeof(getcap), answer);
	CHECK(n == 10 + 1 + 4 + 4 + 2 * 8 && be32(answer + 6) == 0);
	if (n == 35) {
		CHECK(be32(answer + 19) == 0x11Eu && be32(answer + 23) <= CRB_DATA_BYTES);	/* MAX_COMMAND_SIZE */
		CHECK(be32(answer + 27) == 0x11Fu && be32(answer + 31) <= CRB_DATA_BYTES);	/* MAX_RESPONSE_SIZE */
		printf("max command %u, max response %u\n", be32(answer + 23), be32(answer + 31));
	}

	/* The permanent state is on disk, and a second start of the core reads it back. */
	tpmcore_stop();
	CHECK(GetFileAttributesW(blob) != INVALID_FILE_ATTRIBUTES);
	CHECK(tpmcore_start(dir, CRB_DATA_BYTES) == 0);
	crb_reset(&c, page, 0x7DDD3000ull);
	for (k = 0; k < 2; k++) {
		n = request(&c, getrandom, sizeof(getrandom), answer);
		CHECK(n == 44 && be32(answer + 6) == 0);
	}
	tpmcore_stop();
	CHECK(c.count.start_while_idle == 0 && c.count.bad_size == 0);

	printf("crb bridge: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
