/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* BD-235: the escapes that display.c's own WddmDiagnosticAllowed admits while FullWddm is set, that is while OS paging
 * can run. No escape that writes the SDMA0 ring or another engine ring may be among them: RUN_GFX other than its STATE
 * op, RUN_FENCE, RUN_SDMACOPY and RUN_SDMAIB (check_sdma0_writers.py names the writers and their paths). The test
 * also pins the whole admitted set of each flavour, so that a new admission comes with its own review. It is
 * compiled twice, without and with AMDGPU_WDDM_LAB_FLAVOUR. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "regs.generated.h"
#include "bc250kmd_escape.h"

#define TRUE 1
#define FALSE 0
typedef unsigned long ULONG;
typedef unsigned char BOOLEAN;

#include "escape_admission_actual.inc"

#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
#define FLAVOUR "lab"
#else
#define FLAVOUR "release"
#endif

static unsigned checks, bad;
#define CHECK(x, cmd, op) do { ++checks; if (!(x)) { ++bad; printf("FAIL line %d: %s (command %u op %u)\n", __LINE__, #x, \
                                                                 (unsigned)(cmd), (unsigned)(op)); } } while (0)

/* The commands each flavour admits under FullWddm; RUN_GFX and RUN_IH only with their STATE op. */
static int expected(ULONG Command, ULONG Op)
{
    (void)Op;   /* the release flavour admits no command by its op */
    switch (Command) {
    case BC250_ESCAPE_RUN_START_HEALTH: case BC250_ESCAPE_OBSERVE_DCN: case BC250_ESCAPE_RUN_CLOCK:
    case BC250_ESCAPE_RUN_CPU: case BC250_ESCAPE_RUN_DPAUDIO: case BC250_ESCAPE_GET_INFO: case BC250_ESCAPE_GET_LOG:
    case BC250_ESCAPE_LOG_SUMMARY: case BC250_ESCAPE_GET_PAGING_JOURNAL:
        return 1;
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
    case BC250_ESCAPE_READ_REG: case BC250_ESCAPE_GET_MEMORY: case BC250_ESCAPE_VRAM_READ: case BC250_ESCAPE_RUN_DCN:
    case BC250_ESCAPE_RUN_FBDUMP:
        return 1;
    case BC250_ESCAPE_RUN_GFX:
        return Op == BC250_GFX_OP_STATE;
    case BC250_ESCAPE_RUN_IH:
        return Op == BC250_IH_OP_STATE;
#endif
    default:
        return 0;
    }
}

/* A ring writer: never admitted, in either flavour, whatever the op. */
static int ring_writer(ULONG Command, ULONG Op)
{
    return (Command == BC250_ESCAPE_RUN_GFX && Op != BC250_GFX_OP_STATE) || Command == BC250_ESCAPE_RUN_FENCE ||
           Command == BC250_ESCAPE_RUN_SDMACOPY || Command == BC250_ESCAPE_RUN_SDMAIB;
}

int main(void)
{
    static union { BC250_ESCAPE e; BC250_ESCAPE_GFX g; BC250_ESCAPE_IH i; unsigned char raw[65536]; } buffer;
    ULONG command, op, admitted = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    for (command = 0; command < 1024; command++) {
        for (op = 0; op < 16; op++) {
            int allowed;
            memset(&buffer, 0, sizeof(buffer));
            buffer.e.Magic = BC250_ESCAPE_MAGIC;
            buffer.e.Command = command;
            if (command == BC250_ESCAPE_RUN_GFX) buffer.g.Op = op;
            if (command == BC250_ESCAPE_RUN_IH) buffer.i.Op = op;
            allowed = WddmDiagnosticAllowed(&buffer.e, sizeof(buffer)) ? 1 : 0;
            admitted += (ULONG)allowed;
            CHECK(!(allowed && ring_writer(command, op)), command, op);
            CHECK(allowed == expected(command, op), command, op);
        }
    }
    /* The STATE ops are refused when the buffer is too short to hold the op. */
    memset(&buffer, 0, sizeof(buffer));
    buffer.e.Magic = BC250_ESCAPE_MAGIC; buffer.e.Command = BC250_ESCAPE_RUN_GFX; buffer.g.Op = BC250_GFX_OP_STATE;
    CHECK(!WddmDiagnosticAllowed(&buffer.e, sizeof(BC250_ESCAPE_GFX) - 1), BC250_ESCAPE_RUN_GFX, BC250_GFX_OP_STATE);
    printf("escape admission (%s): %lu admitted; %u checks, %u failures\n", FLAVOUR, admitted, checks, bad);
    return bad ? 1 : 0;
}
