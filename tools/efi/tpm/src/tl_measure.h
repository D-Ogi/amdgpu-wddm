/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * What the pre-OS TPM measures, into which PCR, in which order (docs/design/tpm-preos.md, "The measurements").
 * The order and the event data follow edk2 SecurityPkg/Tcg/Tcg2Dxe/Tcg2Dxe.c (MeasureSecureBootPolicy,
 * OnReadyToBoot, OnExitBootServices, MeasureVariable) and DxeTpm2MeasureBootLib.c (the image event), fetched
 * 2026-11-14; no code is copied.
 *
 * Pure code: one measurement is "digest, extend, log" through struct tl_measurer.
 */
#ifndef TL_MEASURE_H
#define TL_MEASURE_H
#include <stddef.h>
#include "tl_log.h"

struct tl_measurer {
	/* Extends the SHA-256 bank of one PCR. 1 on success. */
	int (*extend)(void *context, unsigned pcr, const unsigned char digest[TL_SHA256_BYTES]);
	void *context;
	struct tl_log *log;
};

/* The result of a measurement. */
enum tl_measured {
	TL_MEASURED = 0,	/* extended and logged */
	TL_NOT_LOGGED = 1,	/* extended, the log was full (EFI_VOLUME_FULL of HashLogExtendEvent) */
	TL_NOT_EXTENDED = 2	/* the TPM did not extend: nothing logged */
};

/* digest = SHA-256(hashed), extend, log the event with data. */
enum tl_measured tl_measure_data(const struct tl_measurer *m, unsigned pcr, unsigned type, const void *hashed,
				 size_t hashed_bytes, const void *data, size_t bytes);
/* The digest is given (an Authenticode digest, a digest the caller computed). */
enum tl_measured tl_measure_digest(const struct tl_measurer *m, unsigned pcr, unsigned type,
				   const unsigned char digest[TL_SHA256_BYTES], const void *data, size_t bytes);

/* One UEFI variable as GetVariable returned it; data NULL and bytes 0 when it does not exist. */
struct tl_variable {
	const unsigned short *name;	/* UTF-16, zero-terminated */
	unsigned char guid[16];		/* EFI_GUID in its memory layout */
	const unsigned char *data;
	size_t bytes;
};

#define TL_SECURE_VARIABLES 5u	/* SecureBoot, PK, KEK, db, dbx */

struct tl_preboot {
	const char *version;			/* the stage version, ASCII: EV_S_CRTM_VERSION as UTF-16 */
	unsigned char stage_digest[TL_SHA256_BYTES];	/* the Authenticode digest of the stage file */
	struct tl_variable secure[TL_SECURE_VARIABLES];
};

/* The events before the first boot application, in this order: PCR 0 the CRTM version and contents (the stage is
 * the static root here); PCR 7 the Secure Boot variables and its separator; PCR 4 "Calling EFI Application from
 * Boot Option"; the separators of PCR 0 to 6. scratch is for the UEFI_VARIABLE_DATA of one variable. Returns the
 * count of events that were not extended (0 on full success). */
unsigned tl_measure_preboot(const struct tl_measurer *m, const struct tl_preboot *p, unsigned char *scratch,
			    size_t scratch_bytes);

/* The boot application: EV_EFI_BOOT_SERVICES_APPLICATION in PCR 4 with UEFI_IMAGE_LOAD_EVENT as data. */
enum tl_measured tl_measure_image(const struct tl_measurer *m, const unsigned char digest[TL_SHA256_BYTES],
				  unsigned long long location, unsigned long long length, const void *device_path,
				  size_t device_path_bytes, unsigned char *scratch, size_t scratch_bytes);

/* ExitBootServices: "Exit Boot Services Invocation" and "Exit Boot Services Returned with Success" in PCR 5. */
unsigned tl_measure_exit_boot_services(const struct tl_measurer *m);

/* The UEFI_VARIABLE_DATA of a variable into out. Returns the size, 0 when out is too small. */
size_t tl_variable_data(const struct tl_variable *v, unsigned char *out, size_t cap);

#endif
