"""The version resource of the kernel-mode driver compiles and carries the numbers it is given (BD-166).

`driver/kmd/bc250kmd.rc` takes every number and string from `version-stamp.h`, which the KMD build writes from
the quad the release builder maps. This check compiles the resource with the kit's own `rc.exe` and reads the
compiled bytes back, so a typo in the resource, a dropped string or a wrong field order fails here in a second
instead of in a signed package. It also holds the refusal: a resource built without the numbers must not compile,
because a driver that reports 0.0.0.0 is the fault the resource exists to close (Counter-Strike 2 on the D3D11
path, BD-166).

    python test_kmd_version_rc.py --kits P:\\BC-250\\toolchain\\nuget [--kit-version 10.0.26100.0] [--out <dir>]
"""

import argparse
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

QUAD = (40, 7, 216, 102)
RELEASE = "0.7.216.102-tester.31"
COMPANY = "amdgpu-wddm"
DESCRIPTION = "amdgpu-wddm kernel-mode driver"
PRIVATE_BUILD = "lab"
SIGNATURE = struct.pack("<I", 0xFEEF04BD)
VS_FF_PRIVATEBUILD = 0x00000008
VFT_DRV = 0x00000003
VFT2_DRV_SYSTEM = 0x00000007

HEADER = """// written by this check, as driver/kmd/build.ps1 writes it in a real build
#define BC250_VER_FILEVERSION       {0},{1},{2},{3}
#define BC250_VER_FILEVERSION_STR   "{0}.{1}.{2}.{3}"
#define BC250_VER_PRODUCTVERSION_STR "{4}"
#define BC250_VER_COMPANY           "{5}"
#define BC250_VER_DESCRIPTION       "{6}"
"""


def compile_resource(rc, includes, resource, header_text, out):
    out.mkdir(parents=True, exist_ok=True)
    (out / "version-stamp.h").write_text(header_text, encoding="ascii")
    res = out / "bc250kmd.res"
    if res.exists():
        res.unlink()
    command = [str(rc), "/nologo", "/fo", str(res)]
    for one in [out] + includes:
        command += ["/I", str(one)]
    command.append(str(resource))
    environment = dict(os.environ)
    environment["INCLUDE"] = ""  # the include path of this check only, never the host's
    done = subprocess.run(command, capture_output=True, text=True, env=environment)
    return done, res


def fixed_info(data):
    """FILEVERSION, PRODUCTVERSION, FILEFLAGS, FILETYPE and FILESUBTYPE, where VS_FIXEDFILEINFO puts them."""
    at = data.find(SIGNATURE)
    if at < 0:
        raise ValueError("no VS_FIXEDFILEINFO signature in the compiled resource")
    # dwSignature, dwStrucVersion, then the two version pairs, the flags mask, the flags, the OS, the type and
    # the subtype, in that order.
    numbers = struct.unpack_from("<9I", data, at + 8)

    def quad(high, low):
        return (high >> 16, high & 0xFFFF, low >> 16, low & 0xFFFF)

    return {"file_version": quad(numbers[0], numbers[1]), "product_version": quad(numbers[2], numbers[3]),
            "flags": numbers[5], "type": numbers[7], "subtype": numbers[8]}


def build_wiring_problems(build_script):
    """What driver/kmd/build.ps1 lacks for this resource to reach the driver: the header written by
    versionstamp.py (never a number of its own), the rc.exe call that compiles the .rc against that header, and
    the compiled .res named among the link inputs."""
    if not build_script.is_file():
        return ["no " + str(build_script)]
    text = build_script.read_text(encoding="utf-8")
    problems = []
    for what, needle in (
            ("writes version-stamp.h with versionstamp.py rc-header", "versionstamp.py') rc-header"),
            ("passes the release version it was given", "--version $PackageVersion"),
            ("passes the flavour", "--flavour $Flavour"),
            ("compiles bc250kmd.rc with rc.exe", "rc.exe"),
            ("names the compiled resource", "bc250kmd.res"),
            ("includes bc250kmd.rc in that call", "bc250kmd.rc"),
            ("gives the resource to the link", "@($res | Where-Object { $_ })")):
        if needle not in text:
            problems.append("driver/kmd/build.ps1 no longer {} ({!r} is gone)".format(what, needle))
    # The numbers belong to versionstamp.py alone: a quad written into the recipe would be a second source of the
    # version, and the two would part on the next release.
    if re.search(r"BC250_VER_FILEVERSION\s", text):
        problems.append("driver/kmd/build.ps1 writes version macros itself: versionstamp.py rc-header owns them")
    return problems


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kits", required=True, help="the NuGet kit root (toolchain\\nuget)")
    parser.add_argument("--kit-version", default="10.0.26100.0")
    parser.add_argument("--out", default="", help="where to write the header and the compiled resource")
    parser.add_argument("--resource", default="", help="the .rc file; driver/kmd/bc250kmd.rc of this repository")
    args = parser.parse_args(argv)

    sdk = pathlib.Path(args.kits) / "microsoft.windows.sdk.cpp" / "c"
    rc = sdk / "bin" / args.kit_version / "x64" / "rc.exe"
    includes = [sdk / "Include" / args.kit_version / "um", sdk / "Include" / args.kit_version / "shared"]
    resource = pathlib.Path(args.resource) if args.resource else (
        pathlib.Path(__file__).resolve().parents[2] / "driver" / "kmd" / "bc250kmd.rc")
    for path in [rc, resource] + includes:
        if not path.exists():
            print("FAIL missing " + str(path))
            return 1

    out = pathlib.Path(args.out) if args.out else pathlib.Path(tempfile.mkdtemp(prefix="kmdverrc-"))
    out.mkdir(parents=True, exist_ok=True)
    problems = []

    header = HEADER.format(*QUAD, RELEASE, COMPANY, DESCRIPTION)
    done, res = compile_resource(rc, includes, resource, header, out)
    if done.returncode or not res.exists():
        print("FAIL rc.exe refused the resource (exit {})".format(done.returncode))
        print((done.stdout + done.stderr).strip()[:2000])
        return 1
    data = res.read_bytes()

    try:
        info = fixed_info(data)
    except ValueError as exc:
        print("FAIL " + str(exc))
        return 1
    file_version = info["file_version"]
    if file_version != QUAD:
        problems.append("FILEVERSION is {} and not {}".format(file_version, QUAD))
    if info["product_version"] != QUAD:
        problems.append("PRODUCTVERSION is {} and not {}".format(info["product_version"], QUAD))
    if (info["type"], info["subtype"]) != (VFT_DRV, VFT2_DRV_SYSTEM):
        problems.append("FILETYPE and FILESUBTYPE are {} and not a system driver ({}, {})".format(
            (info["type"], info["subtype"]), VFT_DRV, VFT2_DRV_SYSTEM))
    if info["flags"] & VS_FF_PRIVATEBUILD:
        problems.append("the release flavour marks itself a private build (FILEFLAGS {:#x})".format(info["flags"]))
    if PRIVATE_BUILD.encode("utf-16-le") in data:
        problems.append("the release flavour carries the PrivateBuild string {!r}".format(PRIVATE_BUILD))

    wanted = {
        "FileVersion": "{}.{}.{}.{}".format(*QUAD),
        "ProductVersion": RELEASE,
        "CompanyName": COMPANY,
        "ProductName": COMPANY,
        "FileDescription": DESCRIPTION,
        "InternalName": "bc250kmd",
        "OriginalFilename": "bc250kmd.sys",
    }
    for key, value in sorted(wanted.items()):
        for text in (key, value):
            if text.encode("utf-16-le") not in data:
                problems.append("the compiled resource has no {} {!r}".format(key, text))
    # Translation is not text: the block holds the language and the code page as two words (US English, Unicode),
    # the pair tools/build/versionstamp.py writes into every user-mode file.
    if "Translation".encode("utf-16-le") not in data or struct.pack("<HH", 0x409, 1200) not in data:
        problems.append("the compiled resource has no Translation 0x409, 1200")

    # The lab flavour of the same release: the same numbers, and a private build mark that the release flavour
    # must not have. One release, two flavours, and nothing but this mark to tell their files apart.
    lab_header = header + "#define BC250_VER_LAB 1\n"
    lab_done, lab_res = compile_resource(rc, includes, resource, lab_header, out / "lab")
    if lab_done.returncode or not lab_res.exists():
        problems.append("rc.exe refused the lab flavour (exit {}): {}".format(
            lab_done.returncode, (lab_done.stdout + lab_done.stderr).strip()[:400]))
    else:
        lab = lab_res.read_bytes()
        lab_info = fixed_info(lab)
        if lab_info["file_version"] != QUAD:
            problems.append("the lab flavour reports {} and not the release number {}".format(
                lab_info["file_version"], QUAD))
        if not lab_info["flags"] & VS_FF_PRIVATEBUILD:
            problems.append("the lab flavour does not set VS_FF_PRIVATEBUILD (FILEFLAGS {:#x})".format(
                lab_info["flags"]))
        if "PrivateBuild".encode("utf-16-le") not in lab or PRIVATE_BUILD.encode("utf-16-le") not in lab:
            problems.append("the lab flavour has no PrivateBuild {!r}".format(PRIVATE_BUILD))

    # The build has to use what compiles here. A resource that no link takes in is a file nobody reads, and that
    # is how this started: the hunks of driver/kmd/build.ps1 were written once and a train merge took the other
    # side of the file, leaving the .rc behind with nothing building it. So the recipe is read here as text.
    problems += build_wiring_problems(resource.with_name("build.ps1"))

    # The refusal: without the numbers the resource must not compile.
    short = "\n".join(line for line in header.splitlines() if "BC250_VER_COMPANY" not in line) + "\n"
    refused, _ = compile_resource(rc, includes, resource, short, out)
    if not refused.returncode:
        problems.append("rc.exe accepted a resource with no BC250_VER_COMPANY; the #error does not hold")

    for one in problems:
        print("FAIL " + one)
    print("{}: {} B compiled, FILEVERSION {}.{}.{}.{}, {} check(s) failed".format(
        "FAIL" if problems else "PASS", len(data), *file_version, len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
