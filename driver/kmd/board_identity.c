#include "board_identity.h"
#include <string.h>

static unsigned read16(const unsigned char* p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static unsigned read32(const unsigned char* p)
{
    return read16(p) | (read16(p + 2) << 16);
}
static int string_is(const unsigned char* start, const unsigned char* end,
                     unsigned index, const char* expected)
{
    unsigned current = 1;
    size_t length = strlen(expected);
    if (!index) return 0;
    while (start < end && *start) {
        const unsigned char* next = start;
        while (next < end && *next) ++next;
        if (next == end) return 0;
        if (current == index)
            return (size_t)(next - start) == length && !memcmp(start, expected, length);
        start = next + 1;
        ++current;
    }
    return 0;
}
unsigned BoardIdentitySelect(const unsigned char* pci, size_t pciBytes,
                             const unsigned char* rsmb, size_t rsmbBytes, unsigned* reason)
{
    size_t at, size;
    unsigned boards = 0, bios = 0, boardMatch = 0, biosMatch = 0, ended = 0;
    *reason = 2;
    if (!pci || pciBytes != 64 || read16(pci) != 0x1002 || read16(pci + 2) != 0x13fe ||
        (pci[14] & 0x7f) != 0 || read16(pci + 44) != 0x1022 || read16(pci + 46) != 0)
        return 0;
    *reason = 3;
    if (!rsmb || rsmbBytes < 8 || read32(rsmb + 4) != rsmbBytes - 8) return 0;
    size = rsmbBytes;
    for (at = 8; at < size;) {
        const unsigned char* h = rsmb + at;
        size_t end, next;
        if (size - at < 4 || h[1] < 4 || h[1] > size - at) return 0;
        end = at + h[1];
        while (end + 1 < size && (rsmb[end] || rsmb[end + 1])) ++end;
        if (end + 1 >= size) return 0;
        next = end + 2;
        if (h[0] == 0) {
            const unsigned char* strings = h + h[1];
            if (++bios != 1 || h[1] < 9) return 0;
            biosMatch = string_is(strings, rsmb + end + 1, h[4], "American Megatrends Inc.") &&
                ((string_is(strings, rsmb + end + 1, h[5], "P2.00") &&
                  string_is(strings, rsmb + end + 1, h[8], "11/09/2021")) ||
                 (string_is(strings, rsmb + end + 1, h[5], "P3.00") &&
                  string_is(strings, rsmb + end + 1, h[8], "12/09/2021")) ||
                 (string_is(strings, rsmb + end + 1, h[5], "P5.00") &&
                  string_is(strings, rsmb + end + 1, h[8], "05/03/2022")));
        } else if (h[0] == 2) {
            if (++boards != 1 || h[1] < 6) return 0;
            boardMatch = string_is(h + h[1], rsmb + end + 1, h[4], "ASRock") &&
                         string_is(h + h[1], rsmb + end + 1, h[5], "AMD BC-250");
        } else if (h[0] == 127) {
            if (h[1] != 4 || next != size) return 0;
            ended = 1;
            break;
        }
        at = next;
    }
    if (!ended || boards != 1 || !boardMatch) { *reason = 2; return 0; }
    return bios == 1 && biosMatch;
}

int BoardIdentityQualification(const unsigned char* rsmb, size_t bytes,
                               unsigned* biosId, unsigned char machineId[16])
{
    size_t at;
    unsigned bios = 0, machines = 0, selected = 0, ended = 0;
    unsigned char identity[16] = {0};
    if (biosId) *biosId = 0;
    if (machineId) memset(machineId, 0, 16);
    if (!biosId || !machineId || !rsmb || bytes < 8 || read32(rsmb + 4) != bytes - 8 ||
        rsmb[1] < 2 || (rsmb[1] == 2 && rsmb[2] < 6)) return 0;
    for (at = 8; at < bytes;) {
        const unsigned char* h = rsmb + at;
        size_t end, next;
        if (bytes - at < 4 || h[1] < 4 || h[1] > bytes - at) return 0;
        end = at + h[1];
        while (end + 1 < bytes && (rsmb[end] || rsmb[end + 1])) ++end;
        if (end + 1 >= bytes) return 0;
        next = end + 2;
        if (h[0] == 0) {
            const unsigned char* strings = h + h[1];
            if (++bios != 1 || h[1] < 9 ||
                !string_is(strings, rsmb + end + 1, h[4], "American Megatrends Inc.")) return 0;
            if (string_is(strings, rsmb + end + 1, h[5], "P2.00") &&
                string_is(strings, rsmb + end + 1, h[8], "11/09/2021")) selected = 2;
            else if (string_is(strings, rsmb + end + 1, h[5], "P3.00") &&
                     string_is(strings, rsmb + end + 1, h[8], "12/09/2021")) selected = 3;
            else if (string_is(strings, rsmb + end + 1, h[5], "P5.00") &&
                     string_is(strings, rsmb + end + 1, h[8], "05/03/2022")) selected = 5;
            else return 0;
        } else if (h[0] == 1) {
            unsigned i, any = 0, nonff = 0;
            if (++machines != 1 || h[1] < 24) return 0;
            for (i = 0; i < 16; ++i) { any |= h[8 + i]; nonff |= h[8 + i] ^ 0xffu; }
            if (!any || !nonff) return 0;
            memcpy(identity, h + 8, 16);
        } else if (h[0] == 127) {
            if (h[1] != 4 || next != bytes) return 0;
            ended = 1;
            break;
        }
        at = next;
    }
    if (!ended || bios != 1 || machines != 1 || !selected) return 0;
    *biosId = selected;
    memcpy(machineId, identity, 16);
    return 1;
}
