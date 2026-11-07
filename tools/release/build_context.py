"""Content-based build context. No persistent timestamp cache and no build/tool probes.

The only optional executed probe is vswhere, bounded to 15 seconds. The receipt
is evidence of the enumerated inputs, not proof of a hermetic build. Tests inject
their environment/executable paths and disable discovery; they never scan real
tool installations.
"""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys


SCHEMA = 2
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
    "Host OS build and Windows update revision are recorded; arbitrary system runtime DLLs are not inventoried.",
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


def host_identity():
    result = {"platform": sys.platform, "version": platform.version(), "machine": platform.machine()}
    if sys.platform == "win32":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion",
                                0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
                result["build"] = str(winreg.QueryValueEx(key, "CurrentBuildNumber")[0])
                result["update_revision"] = int(winreg.QueryValueEx(key, "UBR")[0])
        except OSError as exc:
            result.update(state="unreadable", error=type(exc).__name__)
    return result


def discover_reference_closure(base, declaration, witnesses):
    """Read literal C includes and local generator imports without executing them.

    All C branches are included. External/generated names must be audited in the
    declaration; unknown names or macro includes refuse the context. Directory
    guards detect include-search changes during this capture, not reuse identity.
    """
    base=Path(os.path.abspath(base))
    selected=set()
    def inside(path):
        path=Path(os.path.abspath(path))
        if not path.is_relative_to(base): raise ValueError('reference include escapes '+str(base))
        return path
    def guard(directory):
        directory=inside(directory)
        if directory != base:guard(directory.parent)
        try:
            info=directory.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info,'st_file_attributes',0)&0x400:
                raise ValueError('linked reference directory '+str(directory))
            stamp=(info.st_mtime_ns,info.st_ino)
        except FileNotFoundError:stamp=None
        witnesses.setdefault(str(directory),{'directory_guard':stamp})
    def exists(path):
        path=inside(path);guard(path.parent)
        try:info=path.lstat()
        except FileNotFoundError:return False
        if stat.S_ISLNK(info.st_mode) or getattr(info,'st_file_attributes',0)&0x400:
            raise ValueError('linked required reference input '+str(path))
        return stat.S_ISREG(info.st_mode)
    def read(path):
        path=inside(path)
        if not exists(path):raise ValueError('missing required reference input '+str(path))
        data=path.read_bytes();selected.add(str(path))
        witnesses[str(path)]={'sha256':hashlib.sha256(data).hexdigest().upper()}
        return data.decode('utf-8')
    includes=[inside(base/p) for p in declaration['include_dirs']]
    todo=[inside(base/p) for p in declaration['sources']]
    visited=set()
    while todo:
        path=todo.pop()
        if path in visited:continue
        visited.add(path)
        text=re.sub(r'/\*.*?\*/','',read(path),flags=re.S)
        for directive in re.findall(r'^\s*#\s*include\s+(.+)',text,re.M):
            match=re.match(r'[<"]([^>"]+)[>"]',directive)
            if not match:raise ValueError('macro include needs audit: '+str(path)+': '+directive)
            name=match[1]
            candidates=[inside(path.parent/name),*[inside(p/name) for p in includes]]
            found=next((p for p in candidates if exists(p)),None)
            if found is not None:todo.append(found)
            elif name not in declaration['external_or_generated']:
                raise ValueError('unresolved reference include needs audit: '+name)
    todo=[inside(base/p) for p in declaration.get('generators',[])]
    visited=set()
    while todo:
        path=todo.pop()
        if path in visited:continue
        visited.add(path)
        tree=ast.parse(read(path),filename=str(path))
        names=[]
        for node in ast.walk(tree):
            if isinstance(node,ast.Import):names.extend(a.name for a in node.names)
            elif isinstance(node,ast.ImportFrom):
                if node.level:raise ValueError('relative generator import needs audit: '+str(path))
                if node.module:names.append(node.module)
        for name in names:
            candidate=inside(path.parent/(name.replace('.','/')+'.py'))
            if exists(candidate):todo.append(candidate)
            elif name not in declaration['external_python']:
                raise ValueError('generator import needs audit: '+name)
    for path in declaration.get('data',[]):read(base/path)
    return selected


def select_recipe_inputs(manifest, root, tools, env, declarations, repo=None, witnesses=None):
    """Resolve audited defaults for the exact recipe revisions; unknowns fail closed.

    Declarations are maintained with recipes, not flags a build caller must invent.
    A declaration covers implicit workspace inputs. Explicit {root}/{tool} paths
    are added independently, so an argument override cannot disappear from identity.
    """
    selected, problems, used_tools = {}, [], set()
    witnesses={} if witnesses is None else witnesses
    closure_cache={}
    rules = {(r['recipe'], r['commit']): r for r in declarations.get('recipes', [])}
    binding_cache={}
    def resolve(value):
        def tool(match):
            name=match[1].lower(); used_tools.add(name)
            value=tools.get(name) or env.get('BC250_TOOL_'+name.upper())
            if not value and name=='kits': value=str(Path(root)/'toolchain/nuget')
            if not value: value=shutil.which(name,path=env.get('PATH',''))
            if not value: raise ValueError('unresolved tool '+name)
            return value
        value=re.sub(r'\{tool:([^}]+)\}',tool,str(value)).replace('{root}',str(root))
        if '{' in value or '}' in value: raise ValueError('unresolved input '+value)
        return str(Path(value).absolute())
    for row in manifest.get('rows',[]):
        commit=row.get('recipe_commit') or (row['commit'] if row['repo']==manifest['recipe_repository'] else manifest['recipe_commit'])
        if not re.fullmatch(r'[0-9a-fA-F]{40}',commit):
            problems.append({'path':str(commit),'state':'invalid-recipe-revision'});continue
        rule=rules.get((row['recipe'],commit))
        if rule is None and repo is not None:
            # A new source commit may reuse the audit only when the recipe and
            # its declared helper trees are byte-identical to an audited revision.
            for candidate in declarations.get('recipes',[]):
                if candidate['recipe'] != row['recipe'] or not candidate.get('bindings'): continue
                paths=tuple(sorted(candidate['bindings']))
                key=(commit,paths)
                if key not in binding_cache:
                    done=subprocess.run(['git','-C',str(repo),'rev-parse',*[commit+':'+p for p in paths]],capture_output=True,text=True,timeout=15)
                    values=done.stdout.splitlines()
                    binding_cache[key]=dict(zip(paths,values)) if done.returncode==0 and len(values)==len(paths) else None
                if binding_cache[key]==candidate['bindings']:
                    rule=candidate;break
        if not rule:
            problems.append({'path':row['recipe']+'@'+commit,'state':'recipe-inputs-not-audited'})
            continue
        args=row.get('args',[])
        options={str(args[i]).lower():args[i+1] for i in range(len(args)-1) if str(args[i]).startswith('-') and not str(args[i+1]).startswith('-')}
        for name in ('-root','-qualityworkspace'):
            if name in options and resolve(options[name]) != str(Path(root).absolute()):
                problems.append({'path':row['id']+': '+name,'state':'workspace-override-needs-input-audit'})
        for name in ('-python','-ninja','-vsinstall'):
            if name in options:
                raw=options[name]
                if name!='-vsinstall' and not Path(raw).is_absolute() and '{' not in raw:
                    raw=shutil.which(raw,path=env.get('PATH','')) or raw
                path=resolve(raw)
                if name=='-vsinstall':
                    selected[str(Path(path)/'VC')]=True;selected[str(Path(path)/'MSBuild')]=True
                else:selected[path]=True
        if rule.get('workspace_from_kits'):
            raw=options.get('-kits','{root}/toolchain/nuget')
            if Path(resolve(raw)).parent.parent != Path(root).absolute():
                problems.append({'path':row['id']+': -Kits','state':'kits-derived-workspace-needs-input-audit'})
        for item in rule.get('inputs',[]):
            try:
                raw=options.get('-'+item.get('parameter','').lower(),item['path'])
                path=resolve(raw)
                if item.get('override_append') and '-'+item.get('parameter','').lower() in options:
                    path=str(Path(path)/item['override_append'])
                # A compiler bin override also consumes adjacent builtin headers/libs.
                for _ in range(item.get('parent',0)): path=str(Path(path).parent)
                required=item.get('required',True)
                paths=[str(Path(path)/suffix) for suffix in item['children']] if item.get('children') else [path]
                for path in paths:selected[path]=selected.get(path,False) or required
            except ValueError as exc:
                problems.append({'path':row['id']+': '+str(exc),'state':'unresolved-recipe-input'})
        for closure in rule.get('reference_closures',[]):
            try:
                key=canonical(closure)
                if key not in closure_cache:
                    closure_cache[key]=discover_reference_closure(resolve(closure['path']),closure,witnesses)
                for path in closure_cache[key]:selected[path]=True
            except (ValueError,OSError,SyntaxError) as exc:
                problems.append({'path':row['id']+': '+str(exc),'state':'unresolved-reference-closure'})
        for raw in args:
            # Root alone is workspace location, not a request to hash the whole workspace.
            if str(raw).replace('\\','/').startswith(('{root}/toolchain/','{root}/ref/','{tool:')):
                try:
                    path=resolve(raw); selected[path]=True
                    # A declared directory bundle may intentionally select only
                    # consumed SDK packages beneath the argument's base path.
                    for item in rule.get('inputs',[]):
                        if item.get('children') and raw==options.get('-'+item.get('parameter','').lower()):
                            selected.pop(path,None);break
                except ValueError as exc:
                    problems.append({'path':row['id']+': '+str(exc),'state':'unresolved-recipe-input'})
    return selected, problems, used_tools


def capture_context(root, repo, tools=None, *, environ=None, executables=None, discover_vs=True,
                    manifest=None, declarations=None):
    """Return a deterministic receipt; all content hashing is memoized only within this call.

    `executables`, when supplied, completely replaces executable discovery. An
    empty mapping and `discover_vs=False` keep fixtures independent of the host.
    `complete` means enumerated required inputs were readable, not hermeticity.
    """
    root, repo = Path(root).absolute(), Path(repo).absolute()
    env = {k.upper(): str(v) for k, v in (os.environ if environ is None else environ).items()}
    tools = {k.lower(): str(v) for k, v in (tools or {}).items()}
    scope=None
    witnesses={}
    if manifest is not None:
        if declarations is None:
            declarations=json.loads((repo/'tools/release/build-context-inputs.json').read_text(encoding='utf-8'))
        selected, missing, used_tools=select_recipe_inputs(manifest,root,tools,env,declarations,repo,witnesses)
        scope={'kind':'audited-recipes','declaration_schema':declarations.get('schema'),
               'paths':dict(sorted(selected.items()))}
        if missing:
            body={'schema':SCHEMA,'complete':False,'inputs':{'scope':scope},'incomplete_inputs':missing,
                  'limits':LIMITS+['Add a reviewed declaration for the named recipe revision before planning.']}
            return dict(body,fingerprint=hashlib.sha256(canonical(body)).hexdigest().upper())
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

    defaults = ({name:record(name,required) for name,required in scope['paths'].items()} if scope is not None else
                {"toolchain": record(root / "toolchain"), "python_modules": record(root / "scratch/py"),
                "shader_tools": record(root / "scratch/glslang/bin"),
                "ags_headers": record(root / "ref/AGS_SDK-v6.3.0"),
                "adl_headers": record(root / "ref/display-library__WARN-EULA-in-docs-only-headers-MIT/include"),
                "ddi_contract": record(root / "ref/ddi-display")})
    helpers = {name: record(repo / name, True) for name in RUNTIME_HELPERS}
    def record_tool(name,value):
        members=[p for p in scope['paths'] if Path(p).is_relative_to(expanded(value))] if scope is not None and name=='kits' and name in used_tools else []
        return ({'path':str(expanded(value)),'state':'selected-bundle','members':[record(p,True) for p in members]}
                if members else record(value,True))
    tool_inputs = {name:record_tool(name,value) for name,value in sorted(tools.items())}
    selected_env = {k: env[k] for k in sorted(env) if k in ENVIRONMENT or
                    (k.startswith("BC250_TOOL_") and (scope is None or k[11:].lower() in used_tools))}
    environment_inputs = {}
    for name in PATH_ENVIRONMENT:
        if env.get(name):
            # Windows build paths use semicolons; POSIX fixture paths use the native separator.
            environment_inputs[name] = [record(v, True) for v in env[name].split(os.pathsep) if v.strip()]
    for name in sorted(env):
        if name.startswith("BC250_TOOL_") and env[name] and (scope is None or name[11:].lower() in used_tools):
            environment_inputs[name] = [record_tool(name[11:].lower(), env[name])]

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

    host = host_identity()
    inputs = {"host_os": host, "defaults": defaults, "helpers": helpers, "tools": tool_inputs, "environment": selected_env,
              "environment_inputs": environment_inputs, "executables": executable_inputs, "visual_studio": visual_studio}
    if scope is not None: inputs['scope']=scope
    bad_states = {"unreadable", "unsupported-link", "unsupported-file-type", "changed-during-read",
                  "incomplete-directory", "probe-failed"}

    def sound(value):
        if isinstance(value, dict):
            return value.get("state") not in bad_states and all(sound(v) for v in value.values())
        return not isinstance(value, list) or all(sound(v) for v in value)

    complete = sound(inputs) and all(item["state"] in ("file", "directory") for item in required)
    problems = list(fingerprinter.problems)
    for path,witness in witnesses.items():
        if 'sha256' in witness:
            changed=record(path,True).get('sha256') != witness['sha256']
        else:
            try:
                info=Path(path).lstat();stamp=(info.st_mtime_ns,info.st_ino)
            except FileNotFoundError:stamp=None
            except OSError:stamp='unreadable'
            changed=stamp != witness['directory_guard']
        if changed:
            complete=False;problems.append({'path':path,'state':'reference-changed-during-capture'})
    if host.get("state") == "unreadable":
        problems.append({"path": "Windows CurrentVersion build/update revision", "state": "unreadable"})
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
    parser.add_argument("--manifest")
    parser.add_argument("--declarations")
    args = parser.parse_args()
    tools = {}
    for item in args.tool:
        name, separator, path = item.partition("=")
        if not separator or not name or not path:
            parser.error("--tool needs name=path")
        tools[name] = path
    manifest=json.loads(Path(args.manifest).read_text(encoding='utf-8')) if args.manifest else None
    declarations=json.loads(Path(args.declarations).read_text(encoding='utf-8')) if args.declarations else None
    receipt = capture_context(args.root, args.repo, tools, manifest=manifest, declarations=declarations)
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
