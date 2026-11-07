"""Content-based build context. No persistent timestamp cache and no build/tool probes.

The only optional executed probe is vswhere, bounded to 15 seconds. The receipt
is evidence of the enumerated inputs, not proof of a hermetic build. Tests inject
their environment/executable paths and disable discovery; they never scan real
tool installations.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys


SCHEMA = 1
RUNTIME_HELPERS = (
    "tools/release/build_release_set.py", "tools/release/build-release-set.ps1",
    "tools/release/BuildTree.psm1", "tools/release/provenance.py",
    "tools/release/symbols.py", "tools/release/build_context.py",
    "tools/build/versionstamp.py",
)
EXECUTABLES = (
    "python", "pwsh", "git", "clang-cl", "cl", "link", "lib", "dotnet",
    "cmake", "ninja", "meson", "glslangValidator", "win_flex", "win_bison",
    "llvm-config", "lld-link", "llvm-lib", "rc", "mt", "csc",
)
ENVIRONMENT = (
    "PATH", "PATHEXT", "INCLUDE", "LIB", "LIBPATH", "CL", "_CL_", "LINK", "_LINK_",
    "CC", "CXX", "CC_LD", "CXX_LD", "AR", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS",
    "PYTHONPATH", "PYTHONHOME", "PYTHONNOUSERSITE", "LLVM_CONFIG", "LLVM_DIR", "CMAKE_PREFIX_PATH",
    "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR", "VSINSTALLDIR", "VCTOOLSINSTALLDIR", "VCTOOLSVERSION",
    "WINDOWSSDKDIR", "WINDOWSSDKVERSION", "WINDOWSSDKVERBINPATH", "UCRTVERSION", "UNIVERSALCRTSDKDIR",
    "FRAMEWORKDIR", "FRAMEWORKDIR64", "FRAMEWORKVERSION", "FRAMEWORKVERSION64", "DOTNET_ROOT",
    "DOTNET_ROOT(X86)", "MESON_PACKAGE_CACHE_DIR", "CMAKE_TOOLCHAIN_FILE",
)
PATH_ENVIRONMENT = (
    "INCLUDE", "LIB", "LIBPATH", "PYTHONPATH", "PYTHONHOME", "LLVM_CONFIG", "LLVM_DIR",
    "CMAKE_PREFIX_PATH", "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR", "VSINSTALLDIR", "VCTOOLSINSTALLDIR",
    "WINDOWSSDKDIR", "WINDOWSSDKVERBINPATH", "UNIVERSALCRTSDKDIR", "FRAMEWORKDIR", "FRAMEWORKDIR64",
    "DOTNET_ROOT", "DOTNET_ROOT(X86)", "CMAKE_TOOLCHAIN_FILE",
)
LIMITS = [
    "Covers the named content roots, helper files, environment and resolved executables; not a hermetic dependency proof.",
    "Does not trace arbitrary recipe file access, registry discovery, network responses or undeclared ambient dependencies.",
    "PATH order is recorded; unrelated PATH directories are not recursively scanned.",
    "Missing optional installations are recorded as absent; unreadable, linked or changing declared inputs make complete false.",
]


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


class Fingerprinter:
    def __init__(self):
        self.memo = {}
        self.problems = []

    def capture(self, path):
        path = Path(os.path.abspath(path))
        key = os.path.normcase(str(path))
        if key in self.memo:
            return self.memo[key]
        result = {"path": str(path)}
        try:
            before = path.lstat()
            if stat.S_ISLNK(before.st_mode) or getattr(before, "st_file_attributes", 0) & 0x400:
                result.update(state="unsupported-link")
            elif path.is_file():
                digest = hashlib.sha256()
                with path.open("rb") as stream:
                    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                        digest.update(chunk)
                after = path.stat()
                if (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
                    result.update(state="changed-during-read")
                else:
                    result.update(state="file", sha256=digest.hexdigest().upper(), bytes=after.st_size, files=1)
            elif path.is_dir():
                children = []
                total_bytes = total_files = 0
                complete = True
                for child in sorted(path.iterdir(), key=lambda p: p.name):
                    # Interpreter products are derived from the source bytes, not tool inputs.
                    if child.name.lower() == "__pycache__" or child.suffix.lower() == ".pyc":
                        continue
                    item = self.capture(child)
                    # Root paths are separately recorded; the tree digest describes names and content.
                    children.append((child.name, {k: v for k, v in item.items() if k != "path"}))
                    total_bytes += item.get("bytes", 0)
                    total_files += item.get("files", 0)
                    complete = complete and item["state"] in ("file", "directory")
                after = path.stat()
                if (before.st_mtime_ns, before.st_ino) != (after.st_mtime_ns, after.st_ino):
                    complete = False
                    self.problems.append({"path": str(path), "state": "directory-changed-during-read"})
                result.update(state="directory" if complete else "incomplete-directory",
                              sha256=hashlib.sha256(canonical(children)).hexdigest().upper(),
                              bytes=total_bytes, files=total_files)
            else:
                result.update(state="unsupported-file-type")
        except FileNotFoundError:
            result.update(state="absent")
        except OSError as exc:
            result.update(state="unreadable", error=type(exc).__name__)
        if result["state"] not in ("file", "directory", "absent", "incomplete-directory"):
            self.problems.append({k: result[k] for k in ("path", "state", "error") if k in result})
        self.memo[key] = result
        return result


def capture_context(root, repo, tools=None, *, environ=None, executables=None, discover_vs=True):
    """Return a deterministic receipt; all content hashing is memoized only within this call.

    `executables`, when supplied, completely replaces executable discovery. An
    empty mapping and `discover_vs=False` keep fixtures independent of the host.
    `complete` means enumerated required inputs were readable, not hermeticity.
    """
    root, repo = Path(root).absolute(), Path(repo).absolute()
    env = {k.upper(): str(v) for k, v in (os.environ if environ is None else environ).items()}
    tools = {k.lower(): str(v) for k, v in (tools or {}).items()}
    fingerprinter = Fingerprinter()
    required = []

    def expanded(value):
        value = re.sub(r"%([^%]+)%", lambda m: env.get(m[1].upper(), m[0]), value)
        path = Path(value.strip().strip('"'))
        return path if path.is_absolute() else repo / path

    def record(value, required_input=False):
        item = fingerprinter.capture(expanded(str(value)))
        if required_input:
            required.append(item)
        return item

    defaults = {"toolchain": record(root / "toolchain"), "python_modules": record(root / "scratch/py"),
                "shader_tools": record(root / "scratch/glslang/bin"),
                "ags_headers": record(root / "ref/AGS_SDK-v6.3.0"),
                "adl_headers": record(root / "ref/display-library__WARN-EULA-in-docs-only-headers-MIT/include"),
                "ddi_contract": record(root / "ref/ddi-display/d3d10umddi.md")}
    helpers = {name: record(repo / name, True) for name in RUNTIME_HELPERS}
    tool_inputs = {name: record(value, True) for name, value in sorted(tools.items())}
    selected_env = {k: env[k] for k in sorted(env) if k in ENVIRONMENT or k.startswith("BC250_TOOL_")}
    environment_inputs = {}
    for name in PATH_ENVIRONMENT:
        if env.get(name):
            # Windows build paths use semicolons; POSIX fixture paths use the native separator.
            environment_inputs[name] = [record(v, True) for v in env[name].split(os.pathsep) if v.strip()]
    for name in sorted(env):
        if name.startswith("BC250_TOOL_") and env[name]:
            environment_inputs[name] = [record(env[name], True)]

    if executables is None:
        resolved = {name: shutil.which(name, path=env.get("PATH", "")) for name in EXECUTABLES}
        resolved["python"] = sys.executable
    else:
        resolved = executables
    executable_inputs = {name: record(value, True) if value else {"state": "not-resolved"}
                         for name, value in sorted(resolved.items())}

    visual_studio = {"state": "discovery-disabled"}
    if discover_vs:
        install = env.get("VSINSTALLDIR")
        probe = tools.get("vswhere") or env.get("BC250_TOOL_VSWHERE")
        if not probe and env.get("PROGRAMFILES(X86)"):
            probe = str(Path(env["PROGRAMFILES(X86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe")
        visual_studio = {"state": "not-discovered"}
        if probe:
            visual_studio["probe"] = record(probe)
        if not install and probe and visual_studio["probe"]["state"] == "file":
            try:
                result = subprocess.run([str(expanded(probe)), "-latest", "-products", "*", "-requires",
                                         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                                        capture_output=True, text=True, timeout=15, check=False)
                lines = [s.strip() for s in result.stdout.splitlines() if s.strip()]
                if result.returncode != 0 or len(lines) > 1:
                    visual_studio["state"] = "probe-failed"
                elif lines:
                    install = lines[0]
            except (OSError, subprocess.TimeoutExpired):
                visual_studio["state"] = "probe-failed"
        if install:
            # Fixed tool subdirectories, not an enumeration of unrelated owner files.
            visual_studio.update(state="discovered", vc=record(expanded(install) / "VC", True),
                                 msbuild=record(expanded(install) / "MSBuild", True))
        if env.get("PROGRAMFILES(X86)"):
            visual_studio["windows_kits"] = record(Path(env["PROGRAMFILES(X86)"]) / "Windows Kits/10")

    inputs = {"defaults": defaults, "helpers": helpers, "tools": tool_inputs, "environment": selected_env,
              "environment_inputs": environment_inputs, "executables": executable_inputs, "visual_studio": visual_studio}
    bad_states = {"unreadable", "unsupported-link", "unsupported-file-type", "changed-during-read",
                  "incomplete-directory", "probe-failed"}

    def sound(value):
        if isinstance(value, dict):
            return value.get("state") not in bad_states and all(sound(v) for v in value.values())
        return not isinstance(value, list) or all(sound(v) for v in value)

    complete = sound(inputs) and all(item["state"] in ("file", "directory") for item in required)
    problems = list(fingerprinter.problems)
    for item in required:
        if item["state"] == "absent":
            problems.append({"path": item["path"], "state": "required-input-absent"})
    if visual_studio["state"] == "probe-failed":
        problems.append({"path": visual_studio["probe"]["path"], "state": "probe-failed"})
    # Exact leaf paths make a refused context actionable without suppressing unreadable inputs.
    problems = [json.loads(item) for item in sorted({canonical(p).decode("ascii") for p in problems})]
    body = {"schema": SCHEMA, "complete": complete, "inputs": inputs,
            "incomplete_inputs": problems, "limits": LIMITS}
    return dict(body, fingerprint=hashlib.sha256(canonical(body)).hexdigest().upper())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--tool", action="append", default=[])
    parser.add_argument("--write", required=True)
    args = parser.parse_args()
    tools = {}
    for item in args.tool:
        name, separator, path = item.partition("=")
        if not separator or not name or not path:
            parser.error("--tool needs name=path")
        tools[name] = path
    receipt = capture_context(args.root, args.repo, tools)
    dest = Path(args.write)
    dest.parent.mkdir(parents=True, exist_ok=True)
    # The receipt is captured before planning and is never used if this command fails.
    dest.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(f"build context: {receipt['fingerprint']} complete={receipt['complete']}")
    for problem in receipt["incomplete_inputs"]:
        print(f"context input: {problem['state']}: {problem['path']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
