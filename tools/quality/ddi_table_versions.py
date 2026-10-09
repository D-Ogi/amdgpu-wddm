"""Check the D3D11 shell's newer DDI tables against the WDK headers, entry by entry (BD-099).

The WDDM 2.2 device table and the DXGI 1.6.1 table are built by copying the table of the interface
below and replacing what changed (driver/umd/dxvk/ddi-wddm22.cpp). That is only correct while the
headers say the two tables really do agree entry for entry. A C++ static_assert can compare offsets
and the types of named members, but it cannot enumerate the members of a structure, so it cannot
state "and nothing else changed". This gate reads the headers and states exactly that.

It fails when
  1. a structure is not found, or its entry count is not the count this gate names,
  2. an entry the newer table inherits sits at another index, or has another name or type, so a
     memcpy of the older table would install the wrong entry,
  3. the entries that do change are not the ones named below,
  4. the table builder in the shell assigns an entry that did not change, or leaves a changed entry
     with the copied pointer.

It reads headers and sources only: no GPU, no lab, no build. Variants: d3d10umddi.h declares a
structure once per D3D11DDI_MINOR_HEADER_VERSION step, oldest first, and the build takes the newest.
This gate therefore reads the last declaration of each name, and the entry counts of rule 1 catch a
wrong choice at once.
"""
import argparse
from pathlib import Path
import re
import sys

# The WDDM 2.0 -> WDDM 2.2 device table (d3d10umddi.h). WDDM 2.1 appended the two sync tokens and
# WDDM 2.2 the four shader-cache sessions; the relocation entry carries the new table, so its type
# changes while its index stays.
DEVICE_OLD = "D3DWDDM2_0DDI_DEVICEFUNCS"
DEVICE_NEW = "D3DWDDM2_2DDI_DEVICEFUNCS"
DEVICE_OLD_COUNT = 168
DEVICE_NEW_COUNT = 174
DEVICE_RETYPED = {"pfnRelocateDeviceFuncs": "PFND3DWDDM2_2DDI_RELOCATEDEVICEFUNCS"}
DEVICE_APPENDED = ["pfnAcquireResource", "pfnReleaseResource", "pfnCalcPrivateShaderCacheSessionSize",
                   "pfnCreateShaderCacheSession", "pfnDestroyShaderCacheSession", "pfnSetShaderCacheSession"]
# The DXGI 1.4 -> DXGI 1.6.1 base table (dxgiddi.h). One entry is replaced by its successor at the
# same index, the two Present entries take the 1_6_1 arguments, and Reclaim1 is appended.
DXGI_OLD = "DXGI1_4_DDI_BASE_FUNCTIONS"
DXGI_NEW = "DXGI1_6_1_DDI_BASE_FUNCTIONS"
DXGI_OLD_COUNT = 21
DXGI_NEW_COUNT = 22
DXGI_RETYPED = {
    "pfnOfferResources1": ("pfnOfferResources", "DXGI_DDI_ARG_OFFERRESOURCES1*"),
    "pfnPresent1": ("pfnPresent1", "DXGI1_6_1_DDI_ARG_PRESENT*"),
    "pfnPresentMultiplaneOverlay1": ("pfnPresentMultiplaneOverlay1", "DXGI1_6_1_DDI_ARG_PRESENTMULTIPLANEOVERLAY*"),
}
DXGI_APPENDED = ["pfnReclaimResources1"]
# The shell's builders, and the entries each one may assign. Everything else is copied.
BUILDERS = [
    ("ddi-wddm22.cpp", "make_wddm2_2_device_table", set(DEVICE_RETYPED) | set(DEVICE_APPENDED)),
    ("ddi-wddm22.cpp", "make_dxgi1_6_1_device_table", {"pfnPresent1", "pfnPresentMultiplaneOverlay1"}),
    ("ddi-dxgi-resources.cpp", "install_dxgi1_6_1_resource_ddi", {"pfnOfferResources1", "pfnReclaimResources1"}),
]
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
# A member of a device-function table: one typedef name and one entry name.
DEVICE_MEMBER = re.compile(r"^\s*(PFN[A-Z0-9_]+)\s+(pfn[A-Za-z0-9_]+)\s*;", re.M)
# A member of a DXGI base table, which declares the pointer in place.
DXGI_MEMBER = re.compile(r"\*\s*(pfn[A-Za-z0-9_]+)\s*\)\s*\(\s*([^)]*?)\s*\)\s*;")


def struct_body(text, name):
    """The body of the last `typedef struct <name> { ... } <name>;` in text, comments removed.

    A structure under `#ifdef D3D10PSGP` adds entries no shipping runtime uses. The shell builds its
    tables without that macro, so those blocks are dropped here as well.
    """
    starts = [m.end() for m in re.finditer(r"typedef\s+struct\s+_?%s\b" % re.escape(name), text)]
    if not starts:
        return None
    body = None
    for start in starts:
        open_brace = text.find("{", start)
        close = text.find("}", open_brace)
        while close != -1 and not re.match(r"\s*_?%s\s*;" % re.escape(name), text[close+1:close+80]):
            close = text.find("}", close + 1)
        if open_brace == -1 or close == -1:
            continue
        body = text[open_brace+1:close]
    if body is None:
        return None
    body = COMMENT.sub(" ", body)
    return re.sub(r"#ifdef\s+D3D10PSGP.*?#endif", " ", body, flags=re.S)


def device_entries(text, name):
    body = struct_body(text, name)
    return None if body is None else DEVICE_MEMBER.findall(body)


def dxgi_entries(text, name):
    body = struct_body(text, name)
    return None if body is None else DXGI_MEMBER.findall(body)


def compare(kind, old, new, old_count, new_count, retyped, appended, failures):
    """Rules 1-3 for one pair of tables. retyped maps a new entry name to what it replaces."""
    if old is None or new is None:
        names = (DXGI_OLD, DXGI_NEW) if kind == "DXGI" else (DEVICE_OLD, DEVICE_NEW)
        failures.append("%s: %s or %s is not in the header" % (kind, names[0], names[1]))
        return
    for label, entries, expected in ((kind + " older", old, old_count), (kind + " newer", new, new_count)):
        if len(entries) != expected:
            failures.append("%s: %d entries, this gate expects %d. A header change needs this gate "
                            "and the shell's table read again." % (label, len(entries), expected))
    if len(new) != len(old) + len(appended):
        failures.append("%s: the newer table appends %d entries, this gate expects %d"
                        % (kind, len(new) - len(old), len(appended)))
        return
    for index, (old_entry, new_entry) in enumerate(zip(old, new)):
        old_name, old_type = old_entry[1], old_entry[0]
        new_name, new_type = new_entry[1], new_entry[0]
        if kind == "DXGI":  # the DXGI members carry (name, argument type); the device members (type, name)
            old_name, old_type = old_entry[0], old_entry[1]
            new_name, new_type = new_entry[0], new_entry[1]
        change = retyped.get(new_name)
        if change is None:
            if new_name != old_name or new_type != old_type:
                failures.append("%s index %d: %s %s became %s %s, which this gate does not know. The "
                                "newer table copies that entry, so the copy would be wrong."
                                % (kind, index, old_type, old_name, new_type, new_name))
            continue
        # A row is either the new type alone, where the entry keeps its name, or the pair (older name,
        # new type), where the newer table replaces one entry by its successor at the same index.
        want_old, want_type = (new_name, change) if isinstance(change, str) else change
        if old_name != want_old:
            failures.append("%s index %d: %s is named as the successor of %s, but the older table has "
                            "%s there" % (kind, index, new_name, want_old, old_name))
        if want_type is not None and new_type != want_type:
            failures.append("%s index %d: %s has type %s, this gate expects %s"
                            % (kind, index, new_name, new_type, want_type))
        if new_type == old_type and new_name == old_name:
            failures.append("%s index %d: %s is named as changed, but the header gives it the same name "
                            "and type as before. Remove it from this gate." % (kind, index, new_name))
    for offset, name in enumerate(appended):
        entry = new[len(old) + offset]
        got = entry[1] if kind != "DXGI" else entry[0]
        if got != name:
            failures.append("%s: appended entry %d is %s, this gate expects %s" % (kind, offset, got, name))


def builder_body(text, function):
    """The body of a function definition, by brace counting from its signature."""
    m = re.search(r"\b%s\s*\([^;{]*\)\s*\{" % re.escape(function), text)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(text) and depth:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return COMMENT.sub(" ", text[m.end():i-1])


def check_builder(name, text, function, expected, failures):
    """Rule 4 for one builder: the entries it assigns are exactly the ones that changed."""
    body = builder_body(text, function)
    if body is None:
        failures.append("%s: %s is not defined there" % (name, function))
        return
    assigned = set(re.findall(r"\b(?:t|table)\.(pfn[A-Za-z0-9_]+)\s*=", body))
    for entry in sorted(assigned - expected):
        failures.append("%s: %s assigns %s, which the headers say the newer table inherits unchanged"
                        % (name, function, entry))
    for entry in sorted(expected - assigned):
        failures.append("%s: %s leaves %s with the copied pointer, which the headers say changed"
                        % (name, function, entry))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kits", type=Path, default=None, help="toolchain/nuget (default: the workspace next to the repo)")
    ap.add_argument("--sources", type=Path, default=None, help="driver/umd/dxvk")
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[2]
    sources = args.sources or repo / "driver" / "umd" / "dxvk"
    kits = args.kits
    if kits is None:
        import os
        roots = [Path(os.environ["BC250_ROOT"])] if os.environ.get("BC250_ROOT") else []
        roots.append(repo.parent)
        for root in roots:
            if (root / "toolchain" / "nuget").is_dir():
                kits = root / "toolchain" / "nuget"
                break
    if kits is None or not kits.is_dir():
        raise SystemExit("FAIL: no kits directory (pass --kits)")
    umddi = next(iter(sorted(kits.glob("microsoft.windows.wdk.*/c/Include/*/um/d3d10umddi.h"))), None)
    dxgiddi = next(iter(sorted(kits.glob("microsoft.windows.sdk.cpp*/c/Include/*/um/dxgiddi.h"))), None)
    if umddi is None or dxgiddi is None:
        raise SystemExit("FAIL: d3d10umddi.h or dxgiddi.h is not under " + str(kits))
    failures = []
    umddi_text = umddi.read_text(encoding="utf-8", errors="replace")
    dxgi_text = dxgiddi.read_text(encoding="utf-8", errors="replace")
    device_old = device_entries(umddi_text, DEVICE_OLD)
    device_new = device_entries(umddi_text, DEVICE_NEW)
    compare("device", device_old, device_new, DEVICE_OLD_COUNT, DEVICE_NEW_COUNT,
            DEVICE_RETYPED, DEVICE_APPENDED, failures)
    dxgi_old = dxgi_entries(dxgi_text, DXGI_OLD)
    dxgi_new = dxgi_entries(dxgi_text, DXGI_NEW)
    compare("DXGI", dxgi_old, dxgi_new, DXGI_OLD_COUNT, DXGI_NEW_COUNT, DXGI_RETYPED, DXGI_APPENDED, failures)
    # Rule 4: the shell assigns exactly the entries that changed.
    for name, function, expected in BUILDERS:
        path = sources / name
        if not path.is_file():
            failures.append("%s: no such source" % name)
            continue
        check_builder(name, path.read_text(encoding="utf-8", errors="replace"), function, expected, failures)
    print("%s: device %s->%s (%d entries, %d changed, %d appended), DXGI %s->%s (%d entries), %d builders"
          % ("FAIL" if failures else "PASS", DEVICE_OLD, DEVICE_NEW, DEVICE_NEW_COUNT,
             len(DEVICE_RETYPED), len(DEVICE_APPENDED), DXGI_OLD, DXGI_NEW, DXGI_NEW_COUNT, len(BUILDERS)))
    print("  headers: %s, %s" % (umddi, dxgiddi))
    for line in failures:
        print("  " + line)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
