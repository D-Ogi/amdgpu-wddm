/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The text and path work of the stage that needs no firmware: the UEFI device paths (UEFI
 * Specification 2.10 section 10.3: node type, subtype, two-byte length; the media file path node is type 4,
 * subtype 4) and a small formatter for the console lines. UTF-16 strings are arrays of unsigned short.
 */
#ifndef EFL_TEXT_H
#define EFL_TEXT_H
#include <stdarg.h>
#include <stddef.h>

/* The bytes of a device path up to and including its first end node, 0 when it is malformed inside max_bytes. */
size_t efl_devpath_bytes(const unsigned char *path, size_t max_bytes);

/* The file path of a device path: its media file path nodes, joined with backslashes. 0 on failure. */
int efl_devpath_file(const unsigned char *path, size_t max_bytes, unsigned short *out, size_t capacity);

/* A file beside the image: the directory of image_file and the ASCII name. 0 on failure. */
int efl_sibling_path(const unsigned short *image_file, const char *name, unsigned short *out, size_t capacity);

/* The device part of device (every node before its first end node), one media file path node with file, and the
 * end node. Returns the bytes written, 0 on failure. */
size_t efl_devpath_with_file(const unsigned char *device, size_t max_bytes, const unsigned short *file,
			    unsigned char *out, size_t capacity);

/* A small vsnprintf: %s, %d, %u, %x, %llu, %llx and %%. Always terminates out. Returns the characters written. */
size_t efl_vformat(char *out, size_t capacity, const char *format, va_list args);
size_t efl_format(char *out, size_t capacity, const char *format, ...);

#endif
