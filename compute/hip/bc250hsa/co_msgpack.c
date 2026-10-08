/* co_msgpack.c - the MessagePack reader of the NT_AMDGPU_METADATA note.
 *
 * Layer 1 of docs/design/m16-hip-route-b.md, section 3.1. It reads maps, arrays,
 * strings, unsigned and signed integers, booleans and nil, which is every type the
 * measured notes use. Every other first byte sets the failure flag. The reader never
 * copies and never allocates: a string value points into the note.
 */
#include "co_internal.h"

void bc250hsa_mp_init(bc250hsa_mp_reader* r, const void* bytes, size_t byte_count)
{
    r->at = (const uint8_t*)bytes;
    r->end = r->at + byte_count;
    r->failed = 0;
}

static int have(const bc250hsa_mp_reader* r, size_t need)
{
    return (size_t)(r->end - r->at) >= need;
}

static uint64_t read_be(bc250hsa_mp_reader* r, uint32_t width)
{
    uint64_t v = 0;
    uint32_t i;
    for (i = 0; i < width; i++) {
        v = (v << 8) | (uint64_t)(*r->at++);
    }
    return v;
}

static int fail(bc250hsa_mp_reader* r)
{
    r->failed = 1;
    return 0;
}

static int take_str(bc250hsa_mp_reader* r, uint32_t length, bc250hsa_mp_value* out)
{
    if (!have(r, length)) {
        return fail(r);
    }
    out->kind = BC250HSA_MP_STR;
    out->as.str.text = (const char*)r->at;
    out->as.str.bytes = length;
    r->at += length;
    return 1;
}

int bc250hsa_mp_next(bc250hsa_mp_reader* r, bc250hsa_mp_value* out)
{
    uint8_t tag;

    out->kind = BC250HSA_MP_NIL;
    out->as.u = 0;
    if (r->failed || !have(r, 1)) {
        return fail(r);
    }
    tag = *r->at++;

    /* positive fixint */
    if (tag <= 0x7Fu) {
        out->kind = BC250HSA_MP_UINT;
        out->as.u = tag;
        return 1;
    }
    /* negative fixint */
    if (tag >= 0xE0u) {
        out->kind = BC250HSA_MP_INT;
        out->as.i = (int64_t)(int8_t)tag;
        return 1;
    }
    /* fixmap, fixarray, fixstr */
    if ((tag & 0xF0u) == 0x80u) {
        out->kind = BC250HSA_MP_MAP;
        out->as.count = tag & 0x0Fu;
        return 1;
    }
    if ((tag & 0xF0u) == 0x90u) {
        out->kind = BC250HSA_MP_ARRAY;
        out->as.count = tag & 0x0Fu;
        return 1;
    }
    if ((tag & 0xE0u) == 0xA0u) {
        return take_str(r, tag & 0x1Fu, out);
    }

    switch (tag) {
    case 0xC0u: /* nil */
        out->kind = BC250HSA_MP_NIL;
        return 1;
    case 0xC2u: /* false */
    case 0xC3u: /* true */
        out->kind = BC250HSA_MP_BOOL;
        out->as.boolean = (tag == 0xC3u);
        return 1;
    case 0xCCu: case 0xCDu: case 0xCEu: case 0xCFu: { /* uint 8, 16, 32, 64 */
        const uint32_t width = 1u << (tag - 0xCCu);
        if (!have(r, width)) { return fail(r); }
        out->kind = BC250HSA_MP_UINT;
        out->as.u = read_be(r, width);
        return 1;
    }
    case 0xD0u: case 0xD1u: case 0xD2u: case 0xD3u: { /* int 8, 16, 32, 64 */
        const uint32_t width = 1u << (tag - 0xD0u);
        uint64_t raw;
        if (!have(r, width)) { return fail(r); }
        raw = read_be(r, width);
        out->kind = BC250HSA_MP_INT;
        switch (width) {
        case 1: out->as.i = (int64_t)(int8_t)(uint8_t)raw; break;
        case 2: out->as.i = (int64_t)(int16_t)(uint16_t)raw; break;
        case 4: out->as.i = (int64_t)(int32_t)(uint32_t)raw; break;
        default: out->as.i = (int64_t)raw; break;
        }
        return 1;
    }
    case 0xD9u: case 0xDAu: case 0xDBu: { /* str 8, 16, 32 */
        const uint32_t width = (tag == 0xD9u) ? 1u : (tag == 0xDAu) ? 2u : 4u;
        uint64_t length;
        if (!have(r, width)) { return fail(r); }
        length = read_be(r, width);
        if (length > 0xFFFFFFFFu) { return fail(r); }
        return take_str(r, (uint32_t)length, out);
    }
    case 0xDCu: case 0xDDu: { /* array 16, 32 */
        const uint32_t width = (tag == 0xDCu) ? 2u : 4u;
        uint64_t count;
        if (!have(r, width)) { return fail(r); }
        count = read_be(r, width);
        if (count > 0xFFFFFFFFu) { return fail(r); }
        out->kind = BC250HSA_MP_ARRAY;
        out->as.count = (uint32_t)count;
        return 1;
    }
    case 0xDEu: case 0xDFu: { /* map 16, 32 */
        const uint32_t width = (tag == 0xDEu) ? 2u : 4u;
        uint64_t count;
        if (!have(r, width)) { return fail(r); }
        count = read_be(r, width);
        if (count > 0xFFFFFFFFu) { return fail(r); }
        out->kind = BC250HSA_MP_MAP;
        out->as.count = (uint32_t)count;
        return 1;
    }
    default:
        /* bin, ext, float and every reserved byte. The measured notes use none of
         * them, so the reader refuses rather than steps over an unknown length. */
        return fail(r);
    }
}

int bc250hsa_mp_skip(bc250hsa_mp_reader* r)
{
    bc250hsa_mp_value v;
    uint32_t          i;
    uint32_t          children;

    if (!bc250hsa_mp_next(r, &v)) {
        return 0;
    }
    if (v.kind == BC250HSA_MP_ARRAY) {
        children = v.as.count;
    } else if (v.kind == BC250HSA_MP_MAP) {
        if (v.as.count > 0xFFFFFFFFu / 2u) {
            return fail(r);
        }
        children = v.as.count * 2u;
    } else {
        return 1;
    }
    for (i = 0; i < children; i++) {
        if (!bc250hsa_mp_skip(r)) {
            return 0;
        }
    }
    return 1;
}

int bc250hsa_mp_str_is(const bc250hsa_mp_value* v, const char* key)
{
    size_t length;

    if (v->kind != BC250HSA_MP_STR) {
        return 0;
    }
    length = strlen(key);
    if (length != (size_t)v->as.str.bytes) {
        return 0;
    }
    return memcmp(v->as.str.text, key, length) == 0;
}

int bc250hsa_mp_as_u64(const bc250hsa_mp_value* v, uint64_t* out)
{
    if (v->kind == BC250HSA_MP_UINT) {
        *out = v->as.u;
        return 1;
    }
    if (v->kind == BC250HSA_MP_INT && v->as.i >= 0) {
        *out = (uint64_t)v->as.i;
        return 1;
    }
    if (v->kind == BC250HSA_MP_BOOL) {
        *out = v->as.boolean ? 1u : 0u;
        return 1;
    }
    return 0;
}
