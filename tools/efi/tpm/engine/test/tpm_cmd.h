/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * A small writer of TPM 2.0 commands and reader of responses for the engine test. The formats are those of the TPM
 * 2.0 Library, Part 1 (the command and response structures) and Part 3 (the commands); all integers are big
 * endian.
 */
#ifndef BC250_TPM_ENGINE_TEST_CMD_H
#define BC250_TPM_ENGINE_TEST_CMD_H
#include <stddef.h>

#define TPM_ST_NO_SESSIONS 0x8001u
#define TPM_ST_SESSIONS 0x8002u
#define TPM_RH_OWNER 0x40000001u
#define TPM_RS_PW 0x40000009u

#define TPM_CC_CreatePrimary 0x131u
#define TPM_CC_Startup 0x144u
#define TPM_CC_Create 0x153u
#define TPM_CC_Load 0x157u
#define TPM_CC_Unseal 0x15Eu
#define TPM_CC_FlushContext 0x165u
#define TPM_CC_GetRandom 0x17Bu
#define TPM_CC_PCR_Read 0x17Eu
#define TPM_CC_PCR_Extend 0x182u

#define TPM_RC_SUCCESS 0x000u
#define TPM_RC_INITIALIZE 0x100u
#define TPM_SU_CLEAR 0x0000u

#define TPM_CMD_MAX 4096

struct tpm_cmd {
	unsigned char b[TPM_CMD_MAX];
	size_t n;
	int overflow;
};

/* The header: tag, a size that tpm_cmd_end fills, the command code. */
void tpm_cmd_begin(struct tpm_cmd *c, unsigned tag, unsigned code);
void tpm_cmd_u8(struct tpm_cmd *c, unsigned v);
void tpm_cmd_u16(struct tpm_cmd *c, unsigned v);
void tpm_cmd_u32(struct tpm_cmd *c, unsigned v);
void tpm_cmd_bytes(struct tpm_cmd *c, const void *p, size_t n);
/* A TPM2B: the 16-bit size, then the bytes. */
void tpm_cmd_b2b(struct tpm_cmd *c, const void *p, size_t n);
/* The authorization area with one password session and an empty password. */
void tpm_cmd_password_session(struct tpm_cmd *c);
/* Writes the size into the header. Returns the size, 0 when the command did not fit. */
size_t tpm_cmd_end(struct tpm_cmd *c);

struct tpm_rsp {
	const unsigned char *p;
	size_t n;
	size_t at;
	int bad;	/* a read past the end */
};

/* Reads the header of a response. Returns the response code, or 0xFFFFFFFF for a response shorter than its header
 * or with a size field that is not its length. */
unsigned tpm_rsp_begin(struct tpm_rsp *r, const unsigned char *p, size_t n);
unsigned tpm_rsp_u16(struct tpm_rsp *r);
unsigned tpm_rsp_u32(struct tpm_rsp *r);
/* A TPM2B: returns its bytes and their count. raw: the start of the size field, for a copy of the whole TPM2B. */
const unsigned char *tpm_rsp_b2b(struct tpm_rsp *r, size_t *n, const unsigned char **raw);
void tpm_rsp_skip(struct tpm_rsp *r, size_t n);

#endif
