"""The driver package's own settings against the release defaults table (BD-091).

    python inf_gates.py [--inf <bc250kmd.inf>] [--defaults <registry-defaults.json>] [--out <dir>]

Two files say what a driver setting's released value is: `driver/kmd/bc250kmd.inf`, whose AddReg
runs at every install of the package, and `tools/release/installer/registry-defaults.json`, which
the installer and the control application's reset read. Until 0.7.216.10 they disagreed: the INF
wrote 0 into every gate it names, with no NOCLOBBER, so an install the installer did not drive
(Windows Update, Device Manager, pnputil by hand) closed every one of them while EnableNativeSmu,
which the INF did not name at all, stayed 1. The next start asked for the SMU owner with no
register mapping and failed with Code 43 (StageHistory ... 34 91). The installer hid it by writing
the table again after pnputil; nothing held the INF itself to the table.

This gate does. For every `HKR, Parameters, <name>, <flags>, <value>` line of the INF:

  * the flags are 0x00010003 (FLG_ADDREG_TYPE_DWORD | FLG_ADDREG_NOCLOBBER), so a reinstall keeps
    the value the installer, the control application or an experiment set, unless the name is in
    UNCONDITIONAL below - a value that every install must write whatever is there;
  * the name is a release default of the table, and the value is that default, unless the name is
    in NOT_IN_TABLE below - a value the released configuration does not name.

And for every release default of the table: the INF writes it, or NOT_IN_INF below says why it
does not. Each of the three tables is a decision somebody took on purpose, with a reason, and a
line that no longer applies fails the gate as well, so none of them rots into a blanket excuse.

The gate also holds one deviation closed. The driver can write DriverVersion of the adapter's
software key, which the installation guideline forbids, and the setting that asks for it stays off
unless the control application turns it on. No install of the package may enable it, and no AddReg
line of the package may write a device property of that key. INSTALLATION_OWNED below has the ten
names and the citation (audit finding K1).
"""
import argparse
import json
import pathlib
import re
import sys

NOCLOBBER = 0x00010003          # FLG_ADDREG_TYPE_DWORD | FLG_ADDREG_NOCLOBBER
PLAIN_DWORD = 0x00010001        # FLG_ADDREG_TYPE_DWORD

# Written by every install whatever the key holds, so NOT with NOCLOBBER, and for that reason not a
# release default either: the installer's own table must never carry a value the INF overwrites.
UNCONDITIONAL = {
    "UnconfirmedStarts": "installing the package grants a fresh start budget to the boot-loop guard (guard.c)",
    "EnableHangBugcheck": "a test bugcheck somebody left armed is disarmed by installing a package (hang.c)",
}

# Named by the INF, absent from the released configuration: the value here is what a computer that
# has never had this driver gets, and NOCLOBBER keeps an operator's own choice over a reinstall.
NOT_IN_TABLE = {
    "EnableMmioWrite": "the release leaves the register write gate as it is; a fresh install gets it closed",
    "HwmonBasePort": "0 is the built-in window 0x0A20; an operator who pinned another one keeps it",
}

# A release default the INF does not write. The INF resets only the values it names, so a name left
# out here is a value no install of the package touches - which is what we want for everything the
# installer alone decides.
NOT_IN_INF = {
    "EnableNativePteCopy": "the installer writes it; no install of the package needs to change it",
    "EnableHandleIdentityProbe": "the installer writes it; no install of the package needs to change it",
    "EnableGpuPresentBlit": "a driver safety closure (BD-069): only the installer and the driver write it",
    "EnableCddDwmInterop": "a driver safety closure (BD-069): only the installer and the driver write it",
    "EnableScanoutAdmit": "the installer writes it; no install of the package needs to change it",
    "DpmMode": "a driver safety closure (BD-069): only the installer and the driver write it",
    "DpmMaxMHz": "a clock limit the installer and the control application own",
    "DpmIdleMHz": "a clock limit the installer and the control application own",
}

# The device properties of a device's software key that a driver must not modify, from
# `ref/windows-driver-docs/windows-driver-docs-pr/install/opening-a-device-s-software-key.md:33-46`
# (staging 110f60ea): "You must not modify the values of the following registry entries (device
# properties) in a device's software key". The same page: "Changing driver version or driver date
# might break Windows Update functionality". Its "installation-time only" note is about writes
# during an installation and exempts no write after it.
#
# This driver has one write of that kind, DriverVersion for the games that read it (C69, BD-104,
# driver/kmd/driver_version.c), and holds it closed: the setting that asks for it is absent after an
# install, and only the control application turns it on. The gate keeps it that way. No AddReg line
# of the package writes one of these properties, and neither the INF nor the release table names the
# setting, so no install of the package can enable the write (audit finding K1, 2026-10-10).
INSTALLATION_OWNED = (
    "DriverDate",
    "DriverDateData",
    "DriverDesc",
    "DriverVersion",
    "InfPath",
    "InfSection",
    "InfSectionExt",
    "MatchingDeviceId",
    "ProviderName",
    "EnumPropPages32",
)
RUNTIME_VERSION_SETTING = "ReportAmdDriverVersion"
SOFTWARE_KEY_PAGE = "install/opening-a-device-s-software-key.md:33-46"

LINE = re.compile(r"^\s*HKR\s*,\s*Parameters\s*,\s*([A-Za-z0-9_]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(.*?)\s*$")
# Any AddReg line, whatever root and subkey it writes: HKR, HKLM, HKCU, HKCR, HKU.
ADDREG = re.compile(r"^\s*HK(?:R|LM|CU|CR|U)\s*,[^,]*,\s*([A-Za-z0-9_]+)\s*,")


def read_inf(path):
    """Every HKR,Parameters line of the INF: name -> (flags, value text, line number)."""
    rows = {}
    duplicate = []
    for number, text in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        if text.lstrip().startswith(";"):
            continue
        m = LINE.match(text)
        if not m:
            continue
        name = m.group(1)
        if name in rows:
            duplicate.append("%s: written twice, lines %d and %d" % (name, rows[name][2], number))
        rows[name] = (int(m.group(2), 16), m.group(3), number)
    return rows, duplicate


def read_defaults(path):
    """defaults.parameters of registry-defaults.json: name -> value."""
    table = json.loads(path.read_text(encoding="utf-8"))
    parameters = table.get("defaults", {}).get("parameters")
    if not isinstance(parameters, dict) or not parameters:
        raise SystemExit("%s: defaults.parameters is missing or empty" % path)
    return parameters


def same_value(inf_text, default):
    """The INF writes decimal text; the table holds a JSON number."""
    if isinstance(default, bool) or not isinstance(default, int):
        return False, "the release default %r is not a REG_DWORD number" % (default,)
    try:
        written = int(inf_text, 0)
    except ValueError:
        return False, "the INF value %r is not a number" % (inf_text,)
    return written == default, "the INF writes %d, the release default is %d" % (written, default)


def value_names(node):
    """Every value name of the defaults document: the keys of its objects, at any depth."""
    if isinstance(node, dict):
        for name, child in node.items():
            yield name
            for deeper in value_names(child):
                yield deeper
    elif isinstance(node, list):
        for child in node:
            for deeper in value_names(child):
                yield deeper


def check_installation_state(inf_path, defaults_path):
    """The write the software-key page forbids stays out of the package (finding K1).

    No AddReg line writes one of the installation-owned device properties, and no file of the
    package names the setting that makes the driver write DriverVersion at an adapter start.
    """
    failures = []
    gate = pathlib.Path(__file__).name
    for number, text in enumerate(inf_path.read_text(encoding="ascii").splitlines(), 1):
        if text.lstrip().startswith(";"):
            continue
        match = ADDREG.match(text)
        if not match:
            continue
        name = match.group(1)
        if name in INSTALLATION_OWNED:
            failures.append("%s (line %d): an AddReg line of the package writes %s, a device property "
                            "that holds the installation state of the device. %s: \"You must not modify "
                            "the values of the following registry entries (device properties) in a "
                            "device's software key\". Remove the line"
                            % (name, number, name, SOFTWARE_KEY_PAGE))
        if name == RUNTIME_VERSION_SETTING:
            failures.append("%s (line %d): the INF turns the runtime DriverVersion write on. That write "
                            "deviates from %s and stays off unless the control application asks for it, "
                            "so no install of the package may name the setting (%s)"
                            % (name, number, SOFTWARE_KEY_PAGE, gate))
    document = json.loads(defaults_path.read_text(encoding="utf-8"))
    for name in sorted(set(value_names(document))):
        if name == RUNTIME_VERSION_SETTING:
            failures.append("%s: %s names it, so an install of the package would turn the runtime "
                            "DriverVersion write on. That write deviates from %s and belongs to the "
                            "control application alone (%s)"
                            % (name, defaults_path.name, SOFTWARE_KEY_PAGE, gate))
        elif name in INSTALLATION_OWNED:
            failures.append("%s: %s names a device property that holds the installation state of the "
                            "device. %s forbids a driver to modify it"
                            % (name, defaults_path.name, SOFTWARE_KEY_PAGE))
    return failures


def check(inf_path, defaults_path):
    """Every failure as one line, oldest decision first. An empty list is a pass."""
    rows, failures = read_inf(inf_path)
    if not rows:
        return ["%s: no HKR,Parameters line found" % inf_path]
    defaults = read_defaults(defaults_path)

    for name in sorted(rows):
        flags, text, number = rows[name]
        if name in UNCONDITIONAL:
            if flags != PLAIN_DWORD:
                failures.append("%s (line %d): written with 0x%08X, but it is in UNCONDITIONAL "
                                "(%s), which needs the plain 0x%08X"
                                % (name, number, flags, UNCONDITIONAL[name], PLAIN_DWORD))
            if name in defaults:
                failures.append("%s (line %d): in UNCONDITIONAL and a release default as well. Every "
                                "install would overwrite the table's value, so one of the two has to go"
                                % (name, number))
            continue
        if flags != NOCLOBBER:
            failures.append("%s (line %d): written with 0x%08X. A driver setting needs 0x%08X "
                            "(DWORD | NOCLOBBER), so a reinstall keeps what the installer, the control "
                            "application or an experiment set (BD-091). A value every install must write "
                            "belongs in UNCONDITIONAL of %s, with its reason"
                            % (name, number, flags, NOCLOBBER, pathlib.Path(__file__).name))
            continue
        if name in NOT_IN_TABLE:
            if name in defaults:
                failures.append("%s (line %d): in NOT_IN_TABLE (%s), but the release table does name it. "
                                "Remove the NOT_IN_TABLE line and let the gate compare the values"
                                % (name, number, NOT_IN_TABLE[name]))
            continue
        if name not in defaults:
            failures.append("%s (line %d): the INF writes it and defaults.parameters of %s does not name "
                            "it. Add it to the release table, or to NOT_IN_TABLE of %s with the reason "
                            "why the released configuration does not name it"
                            % (name, number, defaults_path.name, pathlib.Path(__file__).name))
            continue
        ok, detail = same_value(text, defaults[name])
        if not ok:
            failures.append("%s (line %d): %s" % (name, number, detail))

    for name in sorted(defaults):
        if name in rows:
            continue
        if name in NOT_IN_INF:
            continue
        failures.append("%s: a release default that the INF does not write. Add the AddReg line "
                        "(0x%08X), or add it to NOT_IN_INF of %s with the reason"
                        % (name, NOCLOBBER, pathlib.Path(__file__).name))

    for name in sorted(UNCONDITIONAL):
        if name not in rows:
            failures.append("%s: in UNCONDITIONAL, but the INF does not write it. Remove the line" % name)
    for name in sorted(NOT_IN_TABLE):
        if name not in rows:
            failures.append("%s: in NOT_IN_TABLE, but the INF does not write it. Remove the line" % name)
    for name in sorted(NOT_IN_INF):
        if name not in defaults:
            failures.append("%s: in NOT_IN_INF, but it is not a release default any more. Remove the line" % name)
        elif name in rows:
            failures.append("%s: in NOT_IN_INF, but the INF does write it now. Remove the line, so that "
                            "the gate compares its value with the table" % name)
    failures += check_installation_state(inf_path, defaults_path)
    return failures


def report(inf_path, defaults_path, rows, defaults):
    """One line per setting, for the gate's log."""
    out = ["%-28s %-10s %-8s %s" % ("setting", "INF flags", "INF", "release default")]
    for name in sorted(set(rows) | set(defaults)):
        flags = "0x%08X" % rows[name][0] if name in rows else "-"
        written = rows[name][1] if name in rows else "-"
        default = defaults[name] if name in defaults else "-"
        note = ""
        if name in UNCONDITIONAL:
            note = "  every install writes it: " + UNCONDITIONAL[name]
        elif name in NOT_IN_TABLE:
            note = "  not a release default: " + NOT_IN_TABLE[name]
        elif name in NOT_IN_INF:
            note = "  the INF does not write it: " + NOT_IN_INF[name]
        out.append("%-28s %-10s %-8s %s%s" % (name, flags, written, default, note))
    return "\n".join(out)


def main(argv=None):
    repo = pathlib.Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--inf", default=str(repo / "driver" / "kmd" / "bc250kmd.inf"))
    parser.add_argument("--defaults",
                        default=str(repo / "tools" / "release" / "installer" / "registry-defaults.json"))
    parser.add_argument("--out", help="directory for the report of every setting")
    arguments = parser.parse_args(argv)
    inf_path = pathlib.Path(arguments.inf)
    defaults_path = pathlib.Path(arguments.defaults)
    for path in (inf_path, defaults_path):
        if not path.is_file():
            print("FAIL %s does not exist" % path)
            return 1
    failures = check(inf_path, defaults_path)
    rows, _ = read_inf(inf_path)
    defaults = read_defaults(defaults_path)
    text = report(inf_path, defaults_path, rows, defaults)
    if arguments.out:
        directory = pathlib.Path(arguments.out)
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "inf-gates.txt").write_text(text + "\n", encoding="ascii")
    print(text)
    if failures:
        print()
        for line in failures:
            print("FAIL " + line)
        print("\n%d setting(s) disagree between %s and %s" % (len(failures), inf_path.name, defaults_path.name))
        return 1
    print("\nPASS %d INF setting(s), %d release default(s), all agreed" % (len(rows), len(defaults)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
