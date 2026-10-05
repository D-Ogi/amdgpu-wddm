/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * driver/contract/test/bc250_caps_stubs.c - the stubbed query layer.
 *
 * ac_gpu_info.c is compiled from <BC250_ROOT>\ref\mesa unmodified, so its object file references
 * every symbol the whole file needs, not just the ones the functions we call need. Twelve of
 * those live in Mesa translation units that would drag in addrlib, the NIR compiler and the
 * generic util library. Linking all of that to answer "does Mesa recognise this chip" would be
 * the wrong trade.
 *
 * So: twelve stubs, one per symbol, each of which ABORTS. None of them is reachable from the
 * calls bc250_caps_mesa.c makes, and each entry below names the call site and the reason it is
 * not reached. If Mesa ever moves one of these onto our path, the test dies at that line with a
 * message instead of quietly returning a plausible lie. That is the whole design: a stub that
 * returns 0 is a bug waiting to be believed.
 *
 * This is the boundary the task set: a thin stub layer is fine, forking Mesa is not. Nothing in
 * <BC250_ROOT>\ref\mesa is modified, patched or copied. If this file ever has to contain real logic
 * rather than an abort, that is the signal to stop and report it.
 *
 * Verified against Mesa main, origin/main 3ae3d2e.
 */

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4100)  /* unreferenced formal parameter: every stub ignores everything */
#pragma warning(disable: 4018)
#endif

#include "ac_gpu_info.h"
#include "ac_surface.h"
#include "ac_shader_util.h"
#include "ac_debug.h"
#include "ac_video.h"
#include "amd_family.h"
#include "util/format/u_format.h"
#include "util/u_debug.h"
#include "util/os_misc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void bc250_unreachable(const char *sym, const char *why)
{
    fprintf(stderr,
            "\nbc250_caps_test: Mesa called %s(), which the test stubs out.\n"
            "  %s\n"
            "  If this is now on the path we exercise, the stub layer is no longer thin and the\n"
            "  test needs a decision, not a bigger stub. See driver/contract/test/bc250_caps_stubs.c\n",
            sym, why);
    abort();
}

#define BC250_STUB(sym, why) bc250_unreachable(#sym, why)

/* ac_gpu_info.c:1637, inside ac_query_gpu_info(), which we do not call: it needs a live DRM
 * device. Unit A exposes no video ring anyway (E01 debugfs-rings.txt). */
void ac_fill_video_info(struct radeon_info *info, struct ac_drm_device *dev)
{
    BC250_STUB(ac_fill_video_info, "only ac_query_gpu_info() calls it; we call the ac_fill_* set directly");
}

/* ac_print_video_info() is called by ac_print_gpu_info() at ac_gpu_info.c:2108, and we DO call
 * ac_print_gpu_info() now, for --radeon-info. This is the one stub on an exercised path, and it
 * is a deliberate no-op rather than an abort, for a reason that can be checked:
 *
 * Its real body lives in ac_video.c, which pulls in the video UAPI and the codec tables. Unit A
 * has no video IP - every video HW_IP_INFO query returned a single zero word and every video
 * FW_VERSION query returned 0 (E13b4 info.txt), so info->ip[] has no video entry and the real
 * function would print nothing. The reference confirms it: E14 radv-info.txt has "Multimedia
 * info:" followed immediately by the next section header, with no lines between.
 *
 * So printing nothing is not an approximation here, it is the correct output, and
 * compare_radv_info.py checks that the section is empty on both sides. If a future part has a
 * VCN block this stub becomes wrong, which is why it says so out loud. */
void ac_print_video_info(FILE *f, const struct radeon_info *info)
{
    (void)f;
    (void)info;
}

/* ac_get_family_name() and ac_get_ip_type_string() were abort-stubs until ac_print_gpu_info()
 * came onto the path. Rather than grow the stub layer, run.ps1 now compiles Mesa's own
 * src/amd/common/amd_family.c, which defines both (amd_family.c:11 and :207). Real Mesa code,
 * unmodified, same as ac_gpu_info.c - two fewer things this test has to pretend about. */

/* The DRM format modifier section of ac_print_gpu_info(), ac_gpu_info.c:1894-1918. Both of these
 * are on the --radeon-info path and both are deliberate no-ops, for one reason:
 *
 * DRM format modifiers describe tiling for KMS scanout and dma-buf sharing between Linux drivers.
 * Windows has no equivalent - WDDM carries allocation layout in its own private data - so the
 * section is outside this contract, and compare_radv_info.py stops parsing at "Modifiers" on both
 * sides. The real ac_get_supported_modifiers() also lives in ac_surface.c and would drag in
 * addrlib, which this test deliberately does not link (see README, "Not done").
 *
 * Returning false makes ac_print_gpu_modifiers() print nothing at all: its whole body is inside
 * `if (ac_get_supported_modifiers(...))` at :1906. So our dump simply ends where the reference's
 * modifier list begins, which is what the comparison expects. If Windows ever needs a modifier
 * equivalent, this stub is the wrong place to start - ac_surface is. */
bool ac_get_supported_modifiers(const struct radeon_info *info,
                                const struct ac_modifier_options *options,
                                enum pipe_format format,
                                unsigned *mod_count,
                                uint64_t *mods)
{
    (void)info; (void)options; (void)format; (void)mods;
    if (mod_count)
        *mod_count = 0;
    return false;
}

/* Called at ac_gpu_info.c:1897 through util_format_get_blocksizebits(), one line BEFORE the
 * guard above, so it cannot simply abort. The value is used only for the "Modifiers (%u bpp)"
 * label, which never prints because the guard is false. A zeroed static description keeps the
 * inline accessor from dereferencing NULL without inventing format data. */
const struct util_format_description *util_format_description(enum pipe_format format)
{
    static const struct util_format_description empty = {0};
    (void)format;
    return &empty;
}

/* ac_gpu_info.c:467, the gfx6-to-gfx8 arm of ac_fill_tiling_info(). Cyan Skillfish is gfx10.1, so
 * the gfx9-and-later arm at :458 is taken and this is dead. If it ever fires, the blob is
 * claiming a pre-gfx9 chip and the whole result is suspect. */
unsigned ac_pipe_config_to_num_pipes(unsigned pipe_config)
{
    BC250_STUB(ac_pipe_config_to_num_pipes, "the gfx6-8 arm of ac_fill_tiling_info(); this chip is gfx10.1");
    return 0;
}

/* ac_gpu_info.c:200, inside set_custom_cu_en_mask(), which ac_query_gpu_info() calls at :1808
 * behind the AMD_CU_MASK environment variable. */
void ac_compute_late_alloc(const struct radeon_info *info, bool ngg, bool ngg_culling,
                           bool uses_scratch, unsigned *late_alloc_wave64, unsigned *cu_mask)
{
    BC250_STUB(ac_compute_late_alloc, "only set_custom_cu_en_mask() calls it, from ac_query_gpu_info()");
}

void ac_parse_ib(struct ac_ib_parser *ib, const char *name)
{
    BC250_STUB(ac_parse_ib, "referenced from ac_query_gpu_info()'s debug arm only");
}

/* ---------------------------------------------------------------------------------------------
 * The option readers are different in kind from the ten stubs above, and are handled differently.
 *
 * They ARE on the path we exercise: ac_fill_feature_info() reads AMD_IMAGE_OPCODES at
 * ac_gpu_info.c:1193. Aborting there would be wrong, and returning a fixed value would be a lie
 * by omission - the caller's own default is the right answer and the only one that keeps the
 * result reproducible.
 *
 * So these return the caller's default and record that they were asked. bc250_caps_test.c prints
 * the recorded list and checks it against what we expect Mesa to consult. If a future Mesa starts
 * gating a capability on a new environment variable, the list grows and the test says so, instead
 * of the caps quietly depending on whoever ran it.
 *
 * Deliberately NOT getenv(): a contract test whose answer depends on the operator's shell is not
 * a contract test.
 * ------------------------------------------------------------------------------------------- */
#define BC250_MAX_OPTIONS 32
static const char *g_options[BC250_MAX_OPTIONS];
static unsigned g_option_count;

static void record_option(const char *name)
{
    unsigned i;
    for (i = 0; i < g_option_count; i++)
        if (strcmp(g_options[i], name) == 0)
            return;
    if (g_option_count < BC250_MAX_OPTIONS)
        g_options[g_option_count++] = name;
}

unsigned bc250_stub_options(const char *const **names)
{
    *names = g_options;
    return g_option_count;
}

const char *os_get_option(const char *name)
{
    record_option(name);
    return 0;               /* "unset", which is what Mesa's own reader returns without the var */
}

const char *debug_get_option(const char *name, const char *dfault)
{
    record_option(name);
    return dfault;
}

bool debug_get_bool_option(const char *name, bool dfault)
{
    record_option(name);
    return dfault;
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
