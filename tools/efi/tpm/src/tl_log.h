/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The TCG event log of the pre-OS TPM (docs/design/tpm-preos.md): the crypto-agile format of the TCG EFI Protocol
 * Specification (family 2.0, level 00 revision 00.13, section 5.3 "Event Log Header" and TCG_PCR_EVENT2), with one
 * PCR bank, SHA-256. The layout and the final events table follow edk2 SecurityPkg/Tcg/Tcg2Dxe/Tcg2Dxe.c
 * (SetupEventLog, TcgDxeLogEvent; fetched 2026-11-14, SHA-256 1D930FC4); no code is copied.
 *
 * Pure code: the caller gives the memory. All integers are little-endian, as in the specification.
 */
#ifndef TL_LOG_H
#define TL_LOG_H
#include <stddef.h>

#define TL_SHA256_BYTES		32u
#define TL_ALG_SHA256		0x000Bu		/* TPM_ALG_SHA256 */

/* Event types (TCG PC Client PFP; edk2 MdePkg/Include/IndustryStandard/UefiTcgPlatform.h). */
#define TL_EV_NO_ACTION				0x00000003u
#define TL_EV_SEPARATOR				0x00000004u
#define TL_EV_S_CRTM_CONTENTS			0x00000007u
#define TL_EV_S_CRTM_VERSION			0x00000008u
#define TL_EV_EFI_VARIABLE_DRIVER_CONFIG	0x80000001u
#define TL_EV_EFI_BOOT_SERVICES_APPLICATION	0x80000003u
#define TL_EV_EFI_ACTION			0x80000007u

/* The header of the final events table: Version (UINT64, value 1) and NumberOfEvents (UINT64). */
#define TL_FINAL_HEADER_BYTES	16u
/* The first entry, in the TCG_PCR_EVENT format of TPM 1.2: PCRIndex, EventType, a SHA-1 sized digest, EventSize,
 * then TCG_EfiSpecIDEventStruct with one algorithm: signature[16], platformClass, specVersionMinor,
 * specVersionMajor, specErrata, uintnSize, numberOfAlgorithms, {algorithmId, digestSize}, vendorInfoSize. */
#define TL_SPEC_ID_EVENT_BYTES	(16u + 4u + 4u + 4u + 4u + 1u)
#define TL_SPEC_ID_ENTRY_BYTES	(4u + 4u + 20u + 4u + TL_SPEC_ID_EVENT_BYTES)
/* A TCG_PCR_EVENT2 header with one SHA-256 digest, without the event data. */
#define TL_EVENT2_HEADER_BYTES	(4u + 4u + 4u + 2u + TL_SHA256_BYTES + 4u)

struct tl_log {
	unsigned char *area;		/* the event log, filled with 0xFF beyond the last event */
	size_t capacity, used;
	size_t last;			/* the offset of the last event */
	int truncated;			/* an event did not fit: the log misses it */
	/* The final events table (EFI_TCG2_FINAL_EVENTS_TABLE). Each event after GetEventLog goes here too. */
	unsigned char *final;
	size_t final_capacity, final_used;	/* final_used counts the header */
	int final_on;			/* GetEventLog was called */
	int final_truncated;
};

/* Fills both areas with 0xFF, writes the Spec ID event and the final table header. 0 when an area is too small. */
int tl_log_init(struct tl_log *log, unsigned char *area, size_t capacity, unsigned char *final, size_t final_capacity);

/* Appends one TCG_PCR_EVENT2 with the SHA-256 digest (NULL: zeros, for EV_NO_ACTION). Returns 1 when the event
 * is in the log and, after GetEventLog, in the final table; 0 when one of them was full: the area then says
 * truncated. The PCR was extended by the caller in both cases. */
int tl_log_add(struct tl_log *log, unsigned pcr, unsigned type, const unsigned char *digest, const void *data,
	       size_t bytes);

/* GetEventLog: from now on each event goes to the final table too. */
void tl_log_final_start(struct tl_log *log);

/* The offset of the last event; the Spec ID entry counts as an event. */
size_t tl_log_last(const struct tl_log *log);

/* ---- reading a log, for the tests and for the check of the handoff --------------------------------- */

#define TL_READER_MAX_ALGS	8u

/* The digest sizes that the Spec ID entry declares. tl_log_next fills it from the first entry. */
struct tl_reader {
	unsigned algs;
	unsigned id[TL_READER_MAX_ALGS], size[TL_READER_MAX_ALGS];
};

struct tl_event {
	unsigned pcr, type;
	unsigned digests;		/* the count of digests in the entry */
	const unsigned char *digest;	/* the SHA-256 digest, NULL when the entry has none (and in the Spec ID entry) */
	const unsigned char *data;
	size_t bytes;
	size_t offset, size;		/* where the entry is and its whole size */
};

/* Reads the entry at *cursor (0 for the first). Returns 1 with the entry, 0 at the end (0xFF or the end of the
 * bytes), -1 when the entry is malformed. The first entry must be the Spec ID entry of a crypto-agile log; it
 * fills the reader, which the later calls use. bytes may also be the events of a final events table (without
 * its header), with *cursor > 0 at the start and a reader filled from the main log. */
int tl_log_next(const unsigned char *bytes, size_t length, size_t *cursor, struct tl_reader *reader,
		struct tl_event *event);

/* Replays the extends of a log into 24 SHA-256 PCRs with the values of a TPM2_Startup (zero, and all ones for
 * PCR 17 to 22). Returns the number of events, -1 on a malformed log. */
int tl_log_replay(const unsigned char *bytes, size_t length, unsigned char pcrs[24][TL_SHA256_BYTES]);

#endif
