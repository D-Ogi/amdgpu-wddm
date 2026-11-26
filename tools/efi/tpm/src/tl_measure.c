/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The measurements of the pre-OS TPM. The contract and the sources are in tl_measure.h. */
#include <string.h>
#include "efl_sha256.h"
#include "tl_measure.h"

/* The action strings of edk2 MdePkg/Include/IndustryStandard/UefiTcgPlatform.h, logged without a terminator. */
static const char calling_application[] = "Calling EFI Application from Boot Option";
static const char exit_invocation[] = "Exit Boot Services Invocation";
static const char exit_succeeded[] = "Exit Boot Services Returned with Success";
static const char crtm_contents[] = "BC-250 stage image";

#define IMAGE_EVENT_HEADER_BYTES 32u	/* UEFI_IMAGE_LOAD_EVENT before DevicePath: four UINT64 */

static void put64(unsigned char *p, unsigned long long v)
{
	unsigned i;
	for (i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8u * i));
}

enum tl_measured tl_measure_digest(const struct tl_measurer *m, unsigned pcr, unsigned type,
				   const unsigned char digest[TL_SHA256_BYTES], const void *data, size_t bytes)
{
	if (!m->extend(m->context, pcr, digest)) return TL_NOT_EXTENDED;
	return tl_log_add(m->log, pcr, type, digest, data, bytes) ? TL_MEASURED : TL_NOT_LOGGED;
}

enum tl_measured tl_measure_data(const struct tl_measurer *m, unsigned pcr, unsigned type, const void *hashed,
				 size_t hashed_bytes, const void *data, size_t bytes)
{
	unsigned char digest[TL_SHA256_BYTES];
	efl_sha256(hashed, hashed_bytes, digest);
	return tl_measure_digest(m, pcr, type, digest, data, bytes);
}

static unsigned separator(const struct tl_measurer *m, unsigned pcr)
{
	static const unsigned char zero[4] = {0, 0, 0, 0};
	return tl_measure_data(m, pcr, TL_EV_SEPARATOR, zero, 4, zero, 4) == TL_NOT_EXTENDED;
}

static unsigned action(const struct tl_measurer *m, unsigned pcr, const char *text)
{
	size_t n = strlen(text);
	return tl_measure_data(m, pcr, TL_EV_EFI_ACTION, text, n, text, n) == TL_NOT_EXTENDED;
}

size_t tl_variable_data(const struct tl_variable *v, unsigned char *out, size_t cap)
{
	size_t chars = 0, size, i;
	while (v->name[chars]) chars++;
	/* VariableName (EFI_GUID), UnicodeNameLength and VariableDataLength (UINT64 each), the name without its
	 * terminator, the data. */
	size = 16u + 8u + 8u + chars * 2u + v->bytes;
	if (size > cap || (v->bytes && v->data == NULL)) return 0;
	memcpy(out, v->guid, 16);
	put64(out + 16, chars);
	put64(out + 24, v->bytes);
	for (i = 0; i < chars; i++) {
		out[32 + 2 * i] = (unsigned char)v->name[i];
		out[33 + 2 * i] = (unsigned char)(v->name[i] >> 8);
	}
	if (v->bytes) memcpy(out + 32 + chars * 2u, v->data, v->bytes);
	return size;
}

unsigned tl_measure_preboot(const struct tl_measurer *m, const struct tl_preboot *p, unsigned char *scratch,
			    size_t scratch_bytes)
{
	unsigned char version[128];
	unsigned failed = 0, i;
	size_t n = 0;

	/* PCR 0: the stage is the code that measures first, so its version and its image are the CRTM. */
	for (i = 0; p->version[i] && n + 4u <= sizeof(version); i++) {
		version[n++] = (unsigned char)p->version[i];
		version[n++] = 0;
	}
	version[n++] = 0;	/* the UTF-16 terminator, as edk2 measures PcdFirmwareVersionString */
	version[n++] = 0;
	failed += tl_measure_data(m, 0, TL_EV_S_CRTM_VERSION, version, n, version, n) == TL_NOT_EXTENDED;
	failed += tl_measure_digest(m, 0, TL_EV_S_CRTM_CONTENTS, p->stage_digest, crtm_contents,
				    sizeof(crtm_contents) - 1u) == TL_NOT_EXTENDED;

	/* PCR 7: the Secure Boot policy, then its separator, before any image is verified (MeasureSecureBootPolicy).
	 * The digest of EV_EFI_VARIABLE_DRIVER_CONFIG is over the whole UEFI_VARIABLE_DATA. */
	for (i = 0; i < TL_SECURE_VARIABLES; i++) {
		size_t bytes = tl_variable_data(&p->secure[i], scratch, scratch_bytes);
		if (bytes == 0) {
			failed++;
			continue;
		}
		failed += tl_measure_data(m, 7, TL_EV_EFI_VARIABLE_DRIVER_CONFIG, scratch, bytes, scratch, bytes) ==
			  TL_NOT_EXTENDED;
	}
	failed += separator(m, 7);

	/* The first boot attempt (OnReadyToBoot): the action in PCR 4, then the separators of PCR 0 to 6. */
	failed += action(m, 4, calling_application);
	for (i = 0; i < 7u; i++) failed += separator(m, i);
	return failed;
}

enum tl_measured tl_measure_image(const struct tl_measurer *m, const unsigned char digest[TL_SHA256_BYTES],
				  unsigned long long location, unsigned long long length, const void *device_path,
				  size_t device_path_bytes, unsigned char *scratch, size_t scratch_bytes)
{
	size_t bytes = IMAGE_EVENT_HEADER_BYTES + device_path_bytes;
	if (bytes > scratch_bytes || (device_path_bytes && device_path == NULL)) {
		/* No room for the event data: the image is measured with the header only. */
		bytes = IMAGE_EVENT_HEADER_BYTES;
		device_path_bytes = 0;
		if (bytes > scratch_bytes) return tl_measure_digest(m, 4, TL_EV_EFI_BOOT_SERVICES_APPLICATION, digest, NULL, 0);
	}
	put64(scratch, location);		/* ImageLocationInMemory */
	put64(scratch + 8, length);		/* ImageLengthInMemory */
	put64(scratch + 16, 0);			/* ImageLinkTimeAddress: not known before the load */
	put64(scratch + 24, device_path_bytes);	/* LengthOfDevicePath */
	if (device_path_bytes) memcpy(scratch + IMAGE_EVENT_HEADER_BYTES, device_path, device_path_bytes);
	return tl_measure_digest(m, 4, TL_EV_EFI_BOOT_SERVICES_APPLICATION, digest, scratch, bytes);
}

unsigned tl_measure_exit_boot_services(const struct tl_measurer *m)
{
	return action(m, 5, exit_invocation) + action(m, 5, exit_succeeded);
}
