/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The text and path work of core-unlock that needs no firmware: the load options, the UEFI device paths (UEFI
 * Specification 2.10 section 10.3: node type, subtype, two-byte length; the media file path node is type 4,
 * subtype 4) and a small formatter for the console lines. UTF-16 strings are arrays of unsigned short.
 */
#ifndef CU_TEXT_H
#define CU_TEXT_H
#include <stdarg.h>
#include <stddef.h>

/* The reset after the message (docs/design/efi-core-unlock.md, "The reset"). Which one keeps the core mask is not
 * measured. */
enum cu_reset_method {
	CU_RESET_WARM = 0,	/* the runtime service ResetSystem with EfiResetWarm: the default */
	CU_RESET_FADT = 1,	/* the FADT reset register with the FADT reset value */
	CU_RESET_NONE = 2,	/* no reset: the message goes out and Windows starts. The marker is not written */
	CU_RESET_COUNT
};

struct cu_options {
	enum cu_reset_method reset;
	unsigned unknown;	/* name=value tokens that were not understood; each is reported */
};

/* The load options: tokens separated by spaces, "reset=warm", "reset=fadt" or "reset=none". A token without "=" (the
 * image name that a shell passes first) is skipped. Parsing stops at the first zero or at chars. */
void cu_options_parse(const unsigned short *text, size_t chars, struct cu_options *out);
const char *cu_reset_method_name(enum cu_reset_method method);

/* The bytes of a device path up to and including its first end node, 0 when it is malformed inside max_bytes. */
size_t cu_devpath_bytes(const unsigned char *path, size_t max_bytes);

/* The file path of a device path: its media file path nodes, joined with backslashes. 0 on failure. */
int cu_devpath_file(const unsigned char *path, size_t max_bytes, unsigned short *out, size_t capacity);

/* The marker beside the image: the directory of image_file and the name CU_MARKER_NAME. 0 on failure. */
#define CU_MARKER_NAME "core-unlock.mark"
int cu_marker_path(const unsigned short *image_file, unsigned short *out, size_t capacity);

/* The device part of device (every node before its first end node), one media file path node with file, and the
 * end node. Returns the bytes written, 0 on failure. */
size_t cu_devpath_with_file(const unsigned char *device, size_t max_bytes, const unsigned short *file,
			    unsigned char *out, size_t capacity);

/* A small vsnprintf: %s, %d, %u, %x, %llu, %llx and %%. Always terminates out. Returns the characters written. */
size_t cu_vformat(char *out, size_t capacity, const char *format, va_list args);
size_t cu_format(char *out, size_t capacity, const char *format, ...);

#endif
