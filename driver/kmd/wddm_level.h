/* wddm_level.h - the WDDM feature level that DXGK_DRIVERCAPS.WDDMVersion reports (ADR 0019 stage B4, BD-307).
 *
 * Why it exists. The level is a decision of the start, not of the build: the lab must try 2.1, 2.2, 2.4 ... 2.9
 * on one KMD, and go back to 2.0, without a rebuild. The level comes from the setting WddmVersionCap of the
 * driver settings state store (driver/state/catalog.json; `bc250kmd_cli state set WddmVersionCap 0x2900`). The
 * catalog admits only the values of Bc250WddmLevels and refuses the others at the write. This header turns the
 * setting into the level of one start, and says where the level came from:
 *
 *   DEFAULT  the setting has no stored value: the default of the catalog (its "code" value, or the package
 *            value "factory" when the INF writes one), BC250_WDDM_LEVEL_DEFAULT in this tree (2.0).
 *   STATE    the stored value, unchanged.
 *   CLAMP    the stored value, lowered, for the reason in Clamp:
 *            INVALID    not one of Bc250WddmLevels (the catalog refuses it, so only a damaged store gets here):
 *                       BC250_WDDM_LEVEL_FLOOR is used;
 *            INTERFACE  above what DXGKRNL_INTERFACE.Version admits (each interface version admits the WDDM
 *                       level of the same release, d3dukmdt.h:40-51) or above BC250_WDDM_LEVEL_MAX;
 *            OS         the OS build is below BC250_WDDM_LEVEL_MIN_OS_BUILD: no dxgkrnl older than 22621 was
 *                       read, so its list of accepted levels is not known, and the start reports 2.0.
 * The default changes in one place with two spellings: catalog.json "code" of WddmVersionCap and
 * BC250_WDDM_LEVEL_DEFAULT below. tools/state/check_settings.py fails the build when the two differ (the literal
 * default of the GuardReadSetting call in wddm.c).
 *
 * Where the numbers come from. dxgkrnl 22621.6197/6199 and 26100.9549 accept 0x1200, 0x1300, 0x2000, 0x2100 ...
 * 0x2900, 0x3000, 0x3100 (and 26100 also 0x3200) in DXGADAPTER::Initialize; any other value fails the start
 * with "Miniport returned unknown WDDM version" (static reading, scratch/t38/wddm-b4/PLAN.md section 2). 2.3 is
 * left out of the set on purpose: the level only exists as a step to 2.4, and dxgkrnl applies an AMD-only
 * workaround bit at exactly 2.3 (InitializeDriverWorkarounds) whose meaning is not resolved.
 * 2.9 is the top because the interface this binary is compiled at is 2.9 (DXGKDDI_INTERFACE_VERSION_WDDM2_9).
 *
 * The constants are numbers here, so that the host test needs no WDK; wddm.c asserts each one against the
 * DXGKDDI_WDDMv2_* and DXGKDDI_INTERFACE_VERSION_WDDM2_* constants of the WDK at compile time. No kernel call,
 * no lock: the host test (driver/kmd/test/wddm_level_test.c) drives every case.
 */
#pragma once

#define BC250_WDDM_LEVEL_SETTING     L"WddmVersionCap"

#define BC250_WDDM_LEVEL_2_0         0x2000u
#define BC250_WDDM_LEVEL_2_1         0x2100u
#define BC250_WDDM_LEVEL_2_2         0x2200u
#define BC250_WDDM_LEVEL_2_3         0x2300u    /* only as the interface ceiling of a 2.3 interface */
#define BC250_WDDM_LEVEL_2_4         0x2400u
#define BC250_WDDM_LEVEL_2_5         0x2500u
#define BC250_WDDM_LEVEL_2_6         0x2600u
#define BC250_WDDM_LEVEL_2_7         0x2700u
#define BC250_WDDM_LEVEL_2_8         0x2800u
#define BC250_WDDM_LEVEL_2_9         0x2900u

#define BC250_WDDM_LEVEL_DEFAULT     0x2000u   /* = catalog.json WddmVersionCap "code" (check_settings.py) */
#define BC250_WDDM_LEVEL_FLOOR       BC250_WDDM_LEVEL_2_0   /* the 2.0 contract this binary always keeps */
#define BC250_WDDM_LEVEL_MAX         BC250_WDDM_LEVEL_2_9
#define BC250_WDDM_LEVEL_MIN_OS_BUILD 22621u

/* DXGKDDI_INTERFACE_VERSION_WDDM2_0 ... WDDM2_9, d3dukmdt.h:40-51 of WDK 10.0.26100. */
#define BC250_WDDM_INTERFACE_2_0     0x5023u
#define BC250_WDDM_INTERFACE_2_1     0x6003u
#define BC250_WDDM_INTERFACE_2_2     0x700Au
#define BC250_WDDM_INTERFACE_2_3     0x8001u
#define BC250_WDDM_INTERFACE_2_4     0x9006u
#define BC250_WDDM_INTERFACE_2_5     0xA00Bu
#define BC250_WDDM_INTERFACE_2_6     0xB004u
#define BC250_WDDM_INTERFACE_2_7     0xC004u
#define BC250_WDDM_INTERFACE_2_8     0xD001u
#define BC250_WDDM_INTERFACE_2_9     0xE003u

/* The levels a start can report, in rising order. driver/state/catalog.json WddmVersionCap "allowed" is the same
 * list (tools/wddm_contract_check reads this array: every rule on WDDMVersion holds at each of these levels). */
static const unsigned long Bc250WddmLevels[] = {
    BC250_WDDM_LEVEL_2_0, BC250_WDDM_LEVEL_2_1, BC250_WDDM_LEVEL_2_2, BC250_WDDM_LEVEL_2_4, BC250_WDDM_LEVEL_2_5,
    BC250_WDDM_LEVEL_2_6, BC250_WDDM_LEVEL_2_7, BC250_WDDM_LEVEL_2_8, BC250_WDDM_LEVEL_2_9,
};
#define BC250_WDDM_LEVEL_SET_COUNT (sizeof(Bc250WddmLevels) / sizeof(Bc250WddmLevels[0]))

enum bc250_wddm_level_source {
    BC250_WDDM_LEVEL_FROM_DEFAULT = 0,
    BC250_WDDM_LEVEL_FROM_STATE = 1,
    BC250_WDDM_LEVEL_FROM_CLAMP = 2,
};

enum bc250_wddm_level_clamp {
    BC250_WDDM_CLAMP_NONE = 0,
    BC250_WDDM_CLAMP_INVALID = 1,
    BC250_WDDM_CLAMP_INTERFACE = 2,
    BC250_WDDM_CLAMP_OS = 3,
};

typedef struct BC250_WDDM_LEVEL {
    unsigned long Level;        /* what DXGK_DRIVERCAPS.WDDMVersion reports */
    unsigned long Requested;    /* the stored setting, or the default when there is none */
    unsigned long Ceiling;      /* the highest level this start admits (interface, OS, BC250_WDDM_LEVEL_MAX) */
    unsigned long Source;       /* enum bc250_wddm_level_source */
    unsigned long Clamp;        /* enum bc250_wddm_level_clamp */
} BC250_WDDM_LEVEL;

static __inline const char* Bc250WddmLevelSourceName(unsigned long Source)
{
    return Source == BC250_WDDM_LEVEL_FROM_STATE ? "state" : Source == BC250_WDDM_LEVEL_FROM_CLAMP ? "clamp" : "default";
}

static __inline const char* Bc250WddmLevelClampName(unsigned long Clamp)
{
    return Clamp == BC250_WDDM_CLAMP_INVALID ? "invalid" : Clamp == BC250_WDDM_CLAMP_INTERFACE ? "interface"
         : Clamp == BC250_WDDM_CLAMP_OS ? "os" : "none";
}

static __inline int Bc250WddmLevelKnown(unsigned long Level)
{
    unsigned long i;
    for (i = 0; i < BC250_WDDM_LEVEL_SET_COUNT; i++)
        if (Bc250WddmLevels[i] == Level) return 1;
    return 0;
}

/* The WDDM level of the release whose interface version is InterfaceVersion; 0 below 2.0. */
static __inline unsigned long Bc250WddmLevelOfInterface(unsigned long InterfaceVersion)
{
    if (InterfaceVersion >= BC250_WDDM_INTERFACE_2_9) return BC250_WDDM_LEVEL_2_9;
    if (InterfaceVersion >= BC250_WDDM_INTERFACE_2_8) return BC250_WDDM_LEVEL_2_8;
    if (InterfaceVersion >= BC250_WDDM_INTERFACE_2_7) return BC250_WDDM_LEVEL_2_7;
    if (InterfaceVersion >= BC250_WDDM_INTERFACE_2_6) return BC250_WDDM_LEVEL_2_6;
    if (InterfaceVersion >= BC250_WDDM_INTERFACE_2_5) return BC250_WDDM_LEVEL_2_5;
    if (InterfaceVersion >= BC250_WDDM_INTERFACE_2_4) return BC250_WDDM_LEVEL_2_4;
    if (InterfaceVersion >= BC250_WDDM_INTERFACE_2_3) return BC250_WDDM_LEVEL_2_3;
    if (InterfaceVersion >= BC250_WDDM_INTERFACE_2_2) return BC250_WDDM_LEVEL_2_2;
    if (InterfaceVersion >= BC250_WDDM_INTERFACE_2_1) return BC250_WDDM_LEVEL_2_1;
    if (InterfaceVersion >= BC250_WDDM_INTERFACE_2_0) return BC250_WDDM_LEVEL_2_0;
    return 0;
}

/* Present: the setting has a stored value (Stored). Default: the effective value without one (the catalog default).
 * InterfaceVersion: DXGKRNL_INTERFACE.Version of this start. OsBuild: RtlGetVersion's dwBuildNumber. The result
 * never goes below BC250_WDDM_LEVEL_FLOOR: this binary needs the 2.0 contract (FlipOnVSyncMmIo, GetNodeMetadata)
 * whatever the setting says. */
static __inline BC250_WDDM_LEVEL Bc250WddmLevelResolve(int Present, unsigned long Stored, unsigned long Default,
                                                      unsigned long InterfaceVersion, unsigned long OsBuild)
{
    BC250_WDDM_LEVEL r;
    unsigned long ceiling, clampWhy, i;

    r.Requested = Present ? Stored : Default;
    r.Source = Present ? BC250_WDDM_LEVEL_FROM_STATE : BC250_WDDM_LEVEL_FROM_DEFAULT;
    r.Clamp = BC250_WDDM_CLAMP_NONE;
    r.Level = r.Requested;

    ceiling = Bc250WddmLevelOfInterface(InterfaceVersion);
    if (ceiling > BC250_WDDM_LEVEL_MAX) ceiling = BC250_WDDM_LEVEL_MAX;
    clampWhy = BC250_WDDM_CLAMP_INTERFACE;
    if (OsBuild < BC250_WDDM_LEVEL_MIN_OS_BUILD) { ceiling = BC250_WDDM_LEVEL_FLOOR; clampWhy = BC250_WDDM_CLAMP_OS; }
    if (ceiling < BC250_WDDM_LEVEL_FLOOR) ceiling = BC250_WDDM_LEVEL_FLOOR;
    r.Ceiling = ceiling;

    if (!Bc250WddmLevelKnown(r.Requested)) {
        r.Level = BC250_WDDM_LEVEL_FLOOR;
        r.Source = BC250_WDDM_LEVEL_FROM_CLAMP;
        r.Clamp = BC250_WDDM_CLAMP_INVALID;
        return r;
    }
    if (r.Requested > ceiling) {
        /* The highest level of the set at or below the ceiling. The set starts at the floor, which is at or
         * below every ceiling, so the loop always finds one. */
        r.Level = BC250_WDDM_LEVEL_FLOOR;
        for (i = 0; i < BC250_WDDM_LEVEL_SET_COUNT; i++)
            if (Bc250WddmLevels[i] <= ceiling) r.Level = Bc250WddmLevels[i];
        r.Source = BC250_WDDM_LEVEL_FROM_CLAMP;
        r.Clamp = clampWhy;
    }
    return r;
}
