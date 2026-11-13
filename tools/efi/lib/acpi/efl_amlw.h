/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * A writer for a small part of AML and for the header of a definition block, with the encodings of the ACPI
 * Specification 6.5: the table header of section 5.2.6, and of chapter 20 the name strings (20.2.2), the integers
 * (20.2.3), the package length (20.2.4), Scope, Name, Method and Package (20.2.5). It writes into the caller's
 * buffer and never past it. A writer that ran out of room stays in the overflow state, and the table end then
 * returns 0.
 */
#ifndef EFL_AMLW_H
#define EFL_AMLW_H
#include <stddef.h>

struct efl_amlw {
	unsigned char *out;
	size_t capacity, used;
	int overflow;
};

void efl_amlw_init(struct efl_amlw *w, unsigned char *out, size_t capacity);
void efl_amlw_bytes(struct efl_amlw *w, const void *bytes, size_t count);

/* The 36-byte header. The length and the checksum are set by efl_amlw_table_end. The texts are padded with
 * spaces: signature and creator of 4 characters, OEM ID of 6, OEM table ID of 8. */
void efl_amlw_table_begin(struct efl_amlw *w, const char *signature, unsigned revision, const char *oem_id,
			  const char *oem_table_id, unsigned oem_revision, const char *creator_id,
			  unsigned creator_revision);
/* Sets the length and the checksum. Returns the bytes of the table, 0 when the writer overflowed. */
size_t efl_amlw_table_end(struct efl_amlw *w);

/* An absolute path such as "\_PR_.P000" as a NameString: the root character, then no prefix, the dual prefix or
 * the multi prefix. Every segment has 4 characters. A path that is not of that form sets the overflow state. */
void efl_amlw_path(struct efl_amlw *w, const char *path);

/* An integer in its shortest form: ZeroOp, OneOp, or a byte, word, dword or qword prefix. */
void efl_amlw_integer(struct efl_amlw *w, unsigned long long value);

/* A construct with a package length. The begin functions write the opcode and what stands before the body and
 * return a mark. efl_amlw_end(mark) then puts the shortest package length in front of everything after the
 * mark. */
size_t efl_amlw_scope_begin(struct efl_amlw *w, const char *path);
size_t efl_amlw_package_begin(struct efl_amlw *w, unsigned elements);	/* Package: at most 255 elements */
void efl_amlw_end(struct efl_amlw *w, size_t mark);

/* NameOp and one name segment. The object follows. */
void efl_amlw_name(struct efl_amlw *w, const char *segment);

/* A Device definition: the extended opcode of ACPI 6.5 section 20.2.5.3, then the 4-character name, then the body.
 * The mark goes to efl_amlw_end like the scope mark. */
size_t efl_amlw_device_begin(struct efl_amlw *w, const char *segment);

/* A string object: the StringPrefix and the characters, with the null terminator. */
void efl_amlw_string(struct efl_amlw *w, const char *text);

/* A named string: Name(_HID, "BC250001"). */
void efl_amlw_name_string(struct efl_amlw *w, const char *segment, const char *text);

/* A method with an empty body: Method (segment, arguments, NotSerialized) {}. */
void efl_amlw_empty_method(struct efl_amlw *w, const char *segment, unsigned arguments);

#endif
