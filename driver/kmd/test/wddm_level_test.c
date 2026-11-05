/* Host control of the WDDM level of a start (wddm_level.h, ADR 0019 stage B4, BD-307). The rule is a pure function
 * of the stored WddmVersionCap setting, the catalog default, DXGKRNL_INTERFACE.Version and the OS build, so every
 * case runs here with made-up inputs in place of dxgkrnl and the state store.
 *
 * The cases: no stored value gives the default with source default; every level of the set is reported as stored
 * with source state; a value outside the set gives 2.0 with clamp invalid; an interface of an older release lowers
 * the level to the highest level of the set it admits (2.3 interface -> 2.2, because 2.3 is not in the set); an OS
 * build below 22621 gives 2.0 with clamp os; nothing goes above 2.9 or below 2.0. The set must be the catalog's
 * "allowed" list of WddmVersionCap, in rising order (the runner reads driver/state/catalog.json and passes it).
 */
#include <stdio.h>
#include <stdlib.h>
#include "wddm_level.h"

#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

#define IFACE_29 BC250_WDDM_INTERFACE_2_9
#define OS_22631 22631u
#define OS_26100 26100u

int main(int argc, char** argv)
{
    unsigned long checks = 0, failures = 0, i;
    BC250_WDDM_LEVEL r;

    /* ---- the set: rising, 2.0 first, 2.9 last, no 2.3, equal to the catalog's allowed list ------------------- */
    CHECK(BC250_WDDM_LEVEL_SET_COUNT == 9u);
    CHECK(Bc250WddmLevels[0] == BC250_WDDM_LEVEL_FLOOR);
    CHECK(Bc250WddmLevels[BC250_WDDM_LEVEL_SET_COUNT - 1] == BC250_WDDM_LEVEL_MAX);
    for (i = 1; i < BC250_WDDM_LEVEL_SET_COUNT; i++) CHECK(Bc250WddmLevels[i - 1] < Bc250WddmLevels[i]);
    CHECK(!Bc250WddmLevelKnown(BC250_WDDM_LEVEL_2_3));
    CHECK(Bc250WddmLevelKnown(BC250_WDDM_LEVEL_DEFAULT));
    if (argc > 1) {
        /* argv[1..]: the catalog's allowed list, decimal */
        CHECK((unsigned long)(argc - 1) == BC250_WDDM_LEVEL_SET_COUNT);
        for (i = 1; i < (unsigned long)argc && i <= BC250_WDDM_LEVEL_SET_COUNT; i++)
            CHECK(strtoul(argv[i], NULL, 10) == Bc250WddmLevels[i - 1]);
    }

    /* ---- no stored value: the catalog default, source default ---------------------------------------------- */
    r = Bc250WddmLevelResolve(0, 0, BC250_WDDM_LEVEL_DEFAULT, IFACE_29, OS_22631);
    CHECK(r.Level == 0x2000u && r.Source == BC250_WDDM_LEVEL_FROM_DEFAULT && r.Clamp == BC250_WDDM_CLAMP_NONE);
    CHECK(r.Requested == 0x2000u && r.Ceiling == 0x2900u);
    /* The default moves with the catalog: a package default of 2.9 is reported as 2.9, still source default. */
    r = Bc250WddmLevelResolve(0, 0, 0x2900u, IFACE_29, OS_26100);
    CHECK(r.Level == 0x2900u && r.Source == BC250_WDDM_LEVEL_FROM_DEFAULT);
    /* A stored value equal to the default is source state, not default: the log must tell the two apart. */
    r = Bc250WddmLevelResolve(1, 0x2000u, 0x2000u, IFACE_29, OS_22631);
    CHECK(r.Level == 0x2000u && r.Source == BC250_WDDM_LEVEL_FROM_STATE);

    /* ---- every level of the set, stored: reported as stored on 22631 and on 26100 ------------------------- */
    for (i = 0; i < BC250_WDDM_LEVEL_SET_COUNT; i++) {
        r = Bc250WddmLevelResolve(1, Bc250WddmLevels[i], BC250_WDDM_LEVEL_DEFAULT, IFACE_29, OS_22631);
        CHECK(r.Level == Bc250WddmLevels[i] && r.Source == BC250_WDDM_LEVEL_FROM_STATE);
        CHECK(r.Clamp == BC250_WDDM_CLAMP_NONE);
        r = Bc250WddmLevelResolve(1, Bc250WddmLevels[i], BC250_WDDM_LEVEL_DEFAULT, IFACE_29, OS_26100);
        CHECK(r.Level == Bc250WddmLevels[i] && r.Source == BC250_WDDM_LEVEL_FROM_STATE);
    }

    /* ---- values outside the set: 2.0, clamp invalid -------------------------------------------------------- */
    {
        static const unsigned long bad[] = { 0u, 0x1300u, 0x2105u, 0x2106u, 0x2300u, 0x2950u, 0x3000u, 0x3200u,
                                             0xFFFFFFFFu };
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            r = Bc250WddmLevelResolve(1, bad[i], BC250_WDDM_LEVEL_DEFAULT, IFACE_29, OS_22631);
            CHECK(r.Level == BC250_WDDM_LEVEL_FLOOR && r.Source == BC250_WDDM_LEVEL_FROM_CLAMP);
            CHECK(r.Clamp == BC250_WDDM_CLAMP_INVALID && r.Requested == bad[i]);
        }
        /* A damaged package default is clamped in the same way. */
        r = Bc250WddmLevelResolve(0, 0, 0x3100u, IFACE_29, OS_22631);
        CHECK(r.Level == BC250_WDDM_LEVEL_FLOOR && r.Clamp == BC250_WDDM_CLAMP_INVALID);
    }

    /* ---- the interface ceiling (DXGKRNL_INTERFACE.Version) ------------------------------------------------- */
    r = Bc250WddmLevelResolve(1, 0x2900u, BC250_WDDM_LEVEL_DEFAULT, BC250_WDDM_INTERFACE_2_8, OS_22631);
    CHECK(r.Level == 0x2800u && r.Source == BC250_WDDM_LEVEL_FROM_CLAMP && r.Clamp == BC250_WDDM_CLAMP_INTERFACE);
    CHECK(r.Ceiling == 0x2800u);
    r = Bc250WddmLevelResolve(1, 0x2600u, BC250_WDDM_LEVEL_DEFAULT, BC250_WDDM_INTERFACE_2_5, OS_22631);
    CHECK(r.Level == 0x2500u && r.Clamp == BC250_WDDM_CLAMP_INTERFACE);     /* R42: > 2.5 needs > 0xA00B */
    r = Bc250WddmLevelResolve(1, 0x2400u, BC250_WDDM_LEVEL_DEFAULT, BC250_WDDM_INTERFACE_2_3, OS_22631);
    CHECK(r.Level == 0x2200u && r.Clamp == BC250_WDDM_CLAMP_INTERFACE);     /* 2.3 is not in the set */
    CHECK(r.Ceiling == 0x2300u);
    r = Bc250WddmLevelResolve(1, 0x2900u, BC250_WDDM_LEVEL_DEFAULT, BC250_WDDM_INTERFACE_2_0, OS_22631);
    CHECK(r.Level == 0x2000u && r.Clamp == BC250_WDDM_CLAMP_INTERFACE);
    r = Bc250WddmLevelResolve(1, 0x2900u, BC250_WDDM_LEVEL_DEFAULT, 0x4003u, OS_22631);   /* below 2.0 */
    CHECK(r.Level == 0x2000u && r.Ceiling == 0x2000u && r.Clamp == BC250_WDDM_CLAMP_INTERFACE);
    /* A newer interface (3.x) does not raise the ceiling above what this binary implements. */
    r = Bc250WddmLevelResolve(1, 0x2900u, BC250_WDDM_LEVEL_DEFAULT, 0x17005u, OS_26100);
    CHECK(r.Level == 0x2900u && r.Ceiling == 0x2900u && r.Clamp == BC250_WDDM_CLAMP_NONE);
    /* A level at or below the ceiling is not clamped. */
    r = Bc250WddmLevelResolve(1, 0x2200u, BC250_WDDM_LEVEL_DEFAULT, BC250_WDDM_INTERFACE_2_3, OS_22631);
    CHECK(r.Level == 0x2200u && r.Source == BC250_WDDM_LEVEL_FROM_STATE);

    /* ---- the OS ceiling: no dxgkrnl below 22621 was read ---------------------------------------------------- */
    r = Bc250WddmLevelResolve(1, 0x2900u, BC250_WDDM_LEVEL_DEFAULT, IFACE_29, 22000u);
    CHECK(r.Level == 0x2000u && r.Source == BC250_WDDM_LEVEL_FROM_CLAMP && r.Clamp == BC250_WDDM_CLAMP_OS);
    r = Bc250WddmLevelResolve(1, 0x2900u, BC250_WDDM_LEVEL_DEFAULT, IFACE_29, 0u);      /* RtlGetVersion failed */
    CHECK(r.Level == 0x2000u && r.Clamp == BC250_WDDM_CLAMP_OS);
    r = Bc250WddmLevelResolve(1, 0x2900u, BC250_WDDM_LEVEL_DEFAULT, IFACE_29, BC250_WDDM_LEVEL_MIN_OS_BUILD);
    CHECK(r.Level == 0x2900u && r.Clamp == BC250_WDDM_CLAMP_NONE);
    /* The default 2.0 under the OS ceiling is not a clamp. */
    r = Bc250WddmLevelResolve(0, 0, BC250_WDDM_LEVEL_DEFAULT, IFACE_29, 19045u);
    CHECK(r.Level == 0x2000u && r.Source == BC250_WDDM_LEVEL_FROM_DEFAULT);

    /* ---- the names the log prints -------------------------------------------------------------------------- */
    CHECK(Bc250WddmLevelSourceName(BC250_WDDM_LEVEL_FROM_DEFAULT)[0] == 'd');
    CHECK(Bc250WddmLevelSourceName(BC250_WDDM_LEVEL_FROM_STATE)[0] == 's');
    CHECK(Bc250WddmLevelSourceName(BC250_WDDM_LEVEL_FROM_CLAMP)[0] == 'c');
    CHECK(Bc250WddmLevelClampName(BC250_WDDM_CLAMP_OS)[0] == 'o');

    /* ---- whatever the inputs, the level is a member of the set ---------------------------------------------- */
    {
        static const unsigned long ifaces[] = { 0u, 0x4003u, BC250_WDDM_INTERFACE_2_0, BC250_WDDM_INTERFACE_2_1,
            BC250_WDDM_INTERFACE_2_2, BC250_WDDM_INTERFACE_2_3, BC250_WDDM_INTERFACE_2_4, BC250_WDDM_INTERFACE_2_5,
            BC250_WDDM_INTERFACE_2_6, BC250_WDDM_INTERFACE_2_7, BC250_WDDM_INTERFACE_2_8, BC250_WDDM_INTERFACE_2_9,
            0x17005u };
        unsigned long s, k;
        for (k = 0; k < sizeof(ifaces) / sizeof(ifaces[0]); k++)
            for (s = 0x1F00u; s <= 0x2A00u; s += 0x40u) {
                r = Bc250WddmLevelResolve(1, s, BC250_WDDM_LEVEL_DEFAULT, ifaces[k], OS_22631);
                CHECK(Bc250WddmLevelKnown(r.Level) && r.Level <= r.Ceiling && r.Level <= BC250_WDDM_LEVEL_MAX);
            }
    }

    printf("wddm_level: %lu checks, %lu failures\n", checks, failures);
    return failures ? 1 : 0;
}
