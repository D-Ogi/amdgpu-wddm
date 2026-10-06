"""Enforce the error contract of the D3D11 shell's DDI entries against the sources.

A DDI entry with a void return reports a failure through pfnSetErrorCb, and each entry's reference page
names the statuses the runtime accepts from that entry. A status outside that set is not an error
report: the runtime treats it as a driver bug, logs it to Dr. Watson and removes the device on purpose
(ref/windows-driver-docs/windows-driver-docs-pr/display/handling-errors.md). BD-071 was that mistake
twice over: CreateShaderResourceView reported E_INVALIDARG for a plane slice, and CreateResource
reported E_NOTIMPL for the decoder bind flag. Both ended the device of a conforming application.

The shell states the allowed set per entry in driver/umd/dxvk/ddi-error-policy.h as an error class, and
every report carries that class. This gate checks the code against the table below, the table against
the local Microsoft reference, and the path BD-071 lived on. It reads sources only: no GPU, no lab, no
build.

It fails when
  1. a literal status in a report_ddi_error call is not one its error class allows,
  1b. the signature of a reporting function carries such a status, where a default argument hides it,
  1c. a reporting helper declares a class that the entries reaching it do not have, or a helper that
      takes its class as a parameter carries a status one of those entries may not report,
  2. one function carries two different error classes,
  2b. a report site inside an entry whose class is not the baseline leaves the class out, so the
      default class reports in its place,
  3. the class in force in an entry's implementation is not the class this table gives for the entry,
     or that implementation cannot be found,
  4. a row here names an entry that no device table assigns,
  5. pfnSetErrorCb is called anywhere but in report_ddi_error, outside the allow-list below,
  6. a status this table allows is not named on the entry's reference page, or the page forbids it,
  7. an entry's class carries device removal although its reference page forbids that entry to report
     it, which is what a capability-check page states,
  8. a view create entry on the plane path refuses a plane of its own, instead of carrying it to the
     engine. That is the shape of BD-071 itself.

Rules 6 and 7 read the local reference. Without it the gate fails, unless --allow-missing-reference
says the caller knows the reference is absent.
"""
import argparse
from pathlib import Path
import re
import sys

# The error classes of ddi-error-policy.h, with the statuses each one admits. Keep both sides equal:
# the C++ enum is the implementation of this table.
REMOVED = 'D3DDDIERR_DEVICEREMOVED'
ALLOWED = {
    'removed_only': {REMOVED},
    'out_of_memory': {REMOVED, 'E_OUTOFMEMORY'},
    'unsupported': {REMOVED, 'E_OUTOFMEMORY', 'DXGI_DDI_ERR_UNSUPPORTED'},
    'non_exclusive': {REMOVED, 'E_OUTOFMEMORY', 'DXGI_DDI_ERR_NONEXCLUSIVE'},
    'still_drawing': {REMOVED, 'DXGI_DDI_ERR_WASSTILLDRAWING'},
    'invalid_arg': {REMOVED, 'E_INVALIDARG'},
    'invalid_arg_oom': {REMOVED, 'E_INVALIDARG', 'E_OUTOFMEMORY'},
    'fail_or_invalid_arg': {REMOVED, 'E_FAIL', 'E_INVALIDARG'},
    'check_invalid_arg': {'E_INVALIDARG'},
    'check_fail_or_invalid_arg': {'E_FAIL', 'E_INVALIDARG'},
    'check_unsupported': {'E_INVALIDARG', 'DXGI_DDI_ERR_UNSUPPORTED'},
    'nothing': set(),
}
# AllowDeviceRemoved is the baseline of handling-errors.md: a void-return entry with no row may report
# device removal and nothing else, which is what every Set, Draw, Clear, Copy, Destroy and Flush page
# states. A capability-check entry is the exception, because the same page states that "The driver
# cannot return D3DDDIERR_DEVICEREMOVED for any check-type function", so a Check entry with no row
# reports nothing at all.
BASELINE_CLASS = 'removed_only'
CHECK_CLASS = 'nothing'
# Every status a DDI entry may ever report. A literal outside this set is never an error report.
LEGAL_STATUSES = set()
for _s in ALLOWED.values():
    LEGAL_STATUSES |= _s


def default_class(entry):
    """The class of an entry with no row: the baseline, or nothing for a capability-check entry."""
    return CHECK_CLASS if entry.startswith('pfnCheck') else BASELINE_CLASS


# entry -> (class, reference page). Only the entries whose page names something other than device
# removal need a row. The page is the topic name in ref/ddi-display/d3d10umddi.md, which carries the
# exact sentence the row is derived from.
POLICY = {
    # Every Create* entry but CreateResource and CreateQuery: "The driver might run out of memory.
    # Therefore, the driver can pass E_OUTOFMEMORY or D3DDDIERR_DEVICEREMOVED".
    'pfnCreateBlendState': ('out_of_memory', 'PFND3D10DDI_CREATEBLENDSTATE'),
    'pfnCreateDepthStencilState': ('out_of_memory', 'PFND3D10DDI_CREATEDEPTHSTENCILSTATE'),
    'pfnCreateDepthStencilView': ('out_of_memory', 'PFND3D11DDI_CREATEDEPTHSTENCILVIEW'),
    'pfnCreateElementLayout': ('out_of_memory', 'PFND3D10DDI_CREATEELEMENTLAYOUT'),
    'pfnCreateVertexShader': ('out_of_memory', 'PFND3D10DDI_CREATEVERTEXSHADER'),
    'pfnCreateGeometryShader': ('out_of_memory', 'PFND3D10DDI_CREATEGEOMETRYSHADER'),
    'pfnCreateGeometryShaderWithStreamOutput': ('out_of_memory', 'PFND3D10DDI_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT'),
    'pfnCreatePixelShader': ('out_of_memory', 'PFND3D10DDI_CREATEPIXELSHADER'),
    'pfnCreateHullShader': ('out_of_memory', 'PFND3D11DDI_CREATEHULLSHADER'),
    'pfnCreateDomainShader': ('out_of_memory', 'PFND3D11DDI_CREATEDOMAINSHADER'),
    'pfnCreateComputeShader': ('out_of_memory', 'PFND3D11DDI_CREATECOMPUTESHADER'),
    'pfnCreateRasterizerState': ('out_of_memory', 'PFND3D10DDI_CREATERASTERIZERSTATE'),
    'pfnCreateRenderTargetView': ('out_of_memory', 'PFND3D10DDI_CREATERENDERTARGETVIEW'),
    'pfnCreateSampler': ('out_of_memory', 'PFND3D10DDI_CREATESAMPLER'),
    'pfnCreateShaderResourceView': ('out_of_memory', 'PFND3D11DDI_CREATESHADERRESOURCEVIEW'),
    'pfnCreateUnorderedAccessView': ('out_of_memory', 'PFND3D11DDI_CREATEUNORDEREDACCESSVIEW'),
    'pfnOpenResource': ('out_of_memory', 'PFND3D10DDI_OPENRESOURCE'),
    # "the driver can pass E_OUTOFMEMORY, D3DDDIERR_DEVICEREMOVED, or DXGI_DDI_ERR_UNSUPPORTED": a
    # resource request this driver cannot serve fails that one call instead of ending the device.
    'pfnCreateResource': ('unsupported', 'PFND3D11DDI_CREATERESOURCE'),
    # Counters are exclusive, so a second user gets DXGI_DDI_ERR_NONEXCLUSIVE.
    'pfnCreateQuery': ('non_exclusive', 'PFND3D10DDI_CREATEQUERY'),
    # The map family and QueryGetData: device removal, and DXGI_DDI_ERR_WASSTILLDRAWING with
    # D3D10_DDI_MAP_FLAG_DONOTWAIT. The ResourceMap page covers the whole family by name.
    'pfnResourceMap': ('still_drawing', 'PFND3D10DDI_RESOURCEMAP'),
    'pfnStagingResourceMap': ('still_drawing', 'PFND3D10DDI_RESOURCEMAP'),
    'pfnDynamicIABufferMapDiscard': ('still_drawing', 'PFND3D10DDI_RESOURCEMAP'),
    'pfnDynamicIABufferMapNoOverwrite': ('still_drawing', 'PFND3D10DDI_RESOURCEMAP'),
    'pfnDynamicConstantBufferMapDiscard': ('still_drawing', 'PFND3D10DDI_RESOURCEMAP'),
    'pfnDynamicConstantBufferMapNoOverwrite': ('still_drawing', 'PFND3D10DDI_RESOURCEMAP'),
    'pfnDynamicResourceMapDiscard': ('still_drawing', 'PFND3D10DDI_RESOURCEMAP'),
    'pfnQueryGetData': ('still_drawing', 'PFND3D10DDI_QUERYGETDATA'),
    # The capability-check entries. Their class carries no device removal, because the runtime calls
    # them after the device is gone and expects an answer.
    'pfnCheckFormatSupport': ('check_fail_or_invalid_arg', 'PFND3D10DDI_CHECKFORMATSUPPORT'),
    'pfnCheckMultisampleQualityLevels': ('check_invalid_arg', 'PFND3DWDDM1_3DDI_CHECKMULTISAMPLEQUALITYLEVELS'),
    'pfnCheckCounter': ('check_unsupported', 'PFND3D10DDI_CHECKCOUNTER'),
    'pfnCheckCounterInfo': ('nothing', 'PFND3D10DDI_CHECKCOUNTERINFO'),
    'pfnCheckDeferredContextHandleSizes': ('nothing', 'PFND3D11DDI_CHECKDEFERREDCONTEXTHANDLESIZES'),
    # GenMips: "can set E_FAIL if the base resource was not created with the appropriate flags or can
    # set E_INVALIDARG if the MIP type was incorrectly specified". It is not a check-type entry, so
    # device removal stays in its class.
    'pfnGenMips': ('fail_or_invalid_arg', 'PFND3D10DDI_GENMIPS'),
    # The tile entries (WDDM 1.3). Their pages name E_INVALIDARG for a missing argument, and
    # ResizeTilePool adds E_OUTOFMEMORY for the page tables. None of them is a check-type entry.
    'pfnUpdateTileMappings': ('invalid_arg', 'PFND3DWDDM1_3DDI_UPDATETILEMAPPINGS'),
    'pfnCopyTileMappings': ('invalid_arg', 'PFND3DWDDM1_3DDI_COPYTILEMAPPINGS'),
    'pfnGetMipPacking': ('invalid_arg', 'PFND3DWDDM1_3DDI_GETMIPPACKING'),
    'pfnResizeTilePool': ('invalid_arg_oom', 'PFND3DWDDM1_3DDI_RESIZETILEPOOL'),
}
# pfnSetErrorCb outside report_ddi_error. One door is the rule; this is the exception and its reason.
CALLBACK_ALLOWED = {
    ('ddi-entry.h', 'report_ddi_error'): 'the one door: it applies the entry class before it reports',
    ('runtime-bridge.cpp', 'Bc250HostLost'): 'the host bridge detects a lost device inside the engine, '
                                             'under whichever entry is running; it passes the status '
                                             'through ddi_class_status and the entry class that '
                                             'enter_context put on the thread, so it reports nothing '
                                             'where the entry may report nothing',
}
# A helper whose class is a parameter and whose status literals it filters itself. The exemption holds
# only while the body still calls ddi_status_allowed, which the gate checks.
GUARDED_HELPERS = {
    ('ddi-entry.h', 'enter_context'): 'it chooses between E_OUTOFMEMORY and device removal through '
                                      'ddi_status_allowed, so the class filters the literal already',
}
# Rule 8. The create entries on the plane path. A planar resource is viewed one plane at a time, and
# the plane must reach the engine: these entries may not compare a plane or read a PlaneSlice, because
# refusing a plane here is BD-071. Every plane decision belongs to plan_srv, plan_rtv and plan_uav,
# which are pure functions that the host test covers, so these entries carry the plane and nothing
# else. A comparison against ddi_plane_from_view_format belongs to the planner too.
PLANE_BLIND = {
    ('ddi-wddm2.cpp', 'srv_create'), ('ddi-wddm2.cpp', 'rtv_create'), ('ddi-wddm2.cpp', 'uav_create'),
    ('ddi-srv.cpp', 'create_shader_resource_view'), ('ddi-rtv.cpp', 'create_render_target_view'),
    ('ddi-uav.cpp', 'create_unordered_access_view'),
}
PLANE_TEST = re.compile(r'!\s*plane\b|\bplane\s*(?:==|!=|<=|>=|<|>|&&|\|\||\?)'
                        r'|(?:==|!=|&&|\|\|)\s*plane\b|\bif\s*\(\s*plane\s*\)')
STATUS_TOKEN = re.compile(r'\b(?:E_[A-Z0-9_]+|S_OK|S_FALSE|D3DDDIERR_[A-Z0-9_]+|DXGI_DDI_ERR_[A-Z0-9_]+|DXGI_ERROR_[A-Z0-9_]+)\b')
NON_STATUS = ('S_OK', 'S_FALSE')


def strip_comments(text):
    """Blank out comments and string literals, keeping every byte offset and line break."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i+1] == '/':
            while i < n and text[i] != '\n':
                out[i] = ' '
                i += 1
        elif c == '/' and i + 1 < n and text[i+1] == '*':
            while i < n and not (text[i] == '*' and i + 1 < n and text[i+1] == '/'):
                if text[i] != '\n':
                    out[i] = ' '
                i += 1
            for j in range(i, min(i + 2, n)):
                out[j] = ' '
            i += 2
        elif c in '"\'':
            quote, out[i], i = c, ' ', i + 1
            while i < n and text[i] != quote:
                if text[i] == '\\':
                    out[i] = ' '
                    i += 1
                if i < n and text[i] != '\n':
                    out[i] = ' '
                i += 1
            if i < n:
                out[i] = ' '
                i += 1
        else:
            i += 1
    return ''.join(out)


NAME_BEFORE_PARENS = re.compile(r'([A-Za-z_]\w*)\s*\($')
# A block whose header ends in a parenthesis but belongs to control flow, not to a function.
KEYWORDS = {'if', 'else', 'for', 'while', 'switch', 'catch', 'do', 'return', 'sizeof', 'static_assert',
            'constexpr', 'decltype', 'noexcept', 'and', 'or', 'not'}
# A called name that is a keyword or a cast, not a function this gate should follow.
CALL_NOISE = KEYWORDS | {'static_cast', 'reinterpret_cast', 'const_cast', 'dynamic_cast'}
# Every free call in a body, including a call to a template with explicit arguments:
# create_state<A,B>(...) is one call to create_state, which the one-level delegation of rule 3 has to
# be able to follow. A member call is not one of these: `views.data()` and `owner->open()` name
# members of other types, not the shell's own data and open entries.
CALL_NAME = re.compile(r'(?<![A-Za-z0-9_.>:~])([A-Za-z_]\w*)\s*(?:<[^<>;{}()]*>\s*)?\(')


def functions(text):
    """Every function body in `text` as (name, start, end). Lambdas and control-flow blocks take the
    name of the function that contains them: the DDI entry owns its whole body."""
    found, stack = [], []
    i, n, header_start = 0, len(text), 0
    while i < n:
        c = text[i]
        if c == '{':
            header = text[header_start:i]
            name = None
            close = header.rstrip()
            if re.search(r'\)\s*(?:const|noexcept|override|final|\s)*$', close):
                depth, j = 0, len(close) - 1
                while j >= 0:
                    if close[j] == ')':
                        depth += 1
                    elif close[j] == '(':
                        depth -= 1
                        if depth == 0:
                            break
                    j -= 1
                m = NAME_BEFORE_PARENS.search(close[:j+1]) if j >= 0 else None
                name = m.group(1) if m and m.group(1) not in KEYWORDS else None
                if name is None:
                    for parent, _ in reversed(stack):
                        if parent:
                            name = parent
                            break
            stack.append((name, i))
            i += 1
            header_start = i
            continue
        if c == '}':
            if stack:
                name, start = stack.pop()
                if name:
                    found.append((name, start, i))
            i += 1
            header_start = i
            continue
        if c == ';':
            header_start = i + 1
        i += 1
    found.sort(key=lambda r: r[1])
    return found


def arguments(text, open_paren):
    """The top-level arguments of the call whose '(' is at open_paren, and the index after its ')'."""
    depth, i, n, start, args = 0, open_paren, len(text), open_paren + 1, []
    while i < n:
        c = text[i]
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
            if depth == 0:
                args.append(text[start:i])
                return args, i + 1
        elif c == ',' and depth == 1:
            args.append(text[start:i])
            start = i + 1
        i += 1
    return args, n


def owning(funcs, position):
    """The outermost function body holding `position`: a lambda inside an entry belongs to the entry."""
    best = None
    for span in funcs:
        if span[1] <= position <= span[2] and (best is None or span[1] < best[1]):
            best = span
    return best


class Source:
    def __init__(self, path):
        self.path = path
        self.raw = path.read_text(encoding='utf-8', errors='replace')
        self.text = strip_comments(self.raw)
        self.funcs = functions(self.text)
        self.lines = [0]
        for i, c in enumerate(self.text):
            if c == '\n':
                self.lines.append(i + 1)

    def line(self, position):
        lo, hi = 0, len(self.lines) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.lines[mid] <= position:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    def spans(self, name):
        """The top-level function bodies named `name` (a lambda carries its owner's name)."""
        out = []
        for span in self.funcs:
            if span[0] == name and owning(self.funcs, span[1]) == span:
                out.append(span)
        return out

    def top_level(self):
        for span in self.funcs:
            if owning(self.funcs, span[1]) == span:
                yield span

    def body(self, span):
        return self.text[span[1]:span[2]]


def header_of(source, span):
    _, start, _ = span
    begin = max(source.text.rfind(';', 0, start), source.text.rfind('}', 0, start),
                source.text.rfind('{', 0, start)) + 1
    return source.text[begin:start]


def class_in_force(source, span):
    """(class, reports, parameterised) for one function body: the single DdiErrorClass token in it, the
    fact that it can report at all, and whether its class comes from a parameter of its own."""
    _, start, end = span
    body = source.text[start:end]
    tokens = set(re.findall(r'DdiErrorClass::(\w+)', body))
    reports = 'report_ddi_error(' in body
    parameterised = 'DdiErrorClass' in header_of(source, span)
    if len(tokens) > 1:
        return sorted(tokens), reports, parameterised
    return (tokens.pop() if tokens else None), reports, parameterised


def reference_sets(path):
    """page -> (statuses named, statuses the page forbids)."""
    text = path.read_text(encoding='utf-8', errors='replace')
    pages = {}
    for section in re.split(r'\n## ', text):
        name = section.split('\n', 1)[0].strip().split(' ')[0]
        if not name.startswith('PFN'):
            continue
        named = set(STATUS_TOKEN.findall(section))
        # "ERR_UNSUPPORTED" and friends appear without the DXGI_DDI_ prefix on some pages.
        for short, full in (('ERR_UNSUPPORTED', 'DXGI_DDI_ERR_UNSUPPORTED'),
                            ('ERR_NONEXCLUSIVE', 'DXGI_DDI_ERR_NONEXCLUSIVE'),
                            ('ERR_WASSTILLDRAWING', 'DXGI_DDI_ERR_WASSTILLDRAWING')):
            if re.search(r'\b' + short + r'\b', section):
                named.add(full)
        forbidden = set()
        for m in re.finditer(r'cannot (?:call|return)[^.]*?(?:set the )?(?:<b>)?(D3DDDIERR_[A-Z_0-9]+)',
                             section):
            forbidden.add(m.group(1))
        pages[name] = (named, forbidden)
    return pages


def page_entry_key(page):
    """The entry name a reference page belongs to, as an upper-case key: PFND3D11DDI_CREATERESOURCE
    and the entry pfnCreateResource share CREATERESOURCE."""
    return page.split('_', 1)[1] if '_' in page else page


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sources', type=Path, default=None, help='driver/umd/dxvk (default: next to this script)')
    ap.add_argument('--reference', type=Path, default=None, help='ref/ddi-display/d3d10umddi.md')
    ap.add_argument('--allow-missing-reference', action='store_true',
                    help='report instead of failing when the local reference is absent')
    ap.add_argument('--list', action='store_true', help='print the entry table and exit')
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[2]
    sources = args.sources or repo / 'driver' / 'umd' / 'dxvk'
    reference = args.reference
    if reference is None:
        import os
        roots = [Path(os.environ['BC250_ROOT'])] if os.environ.get('BC250_ROOT') else []
        roots.append(repo.parent)
        for root in roots:
            candidate = root / 'ref' / 'ddi-display' / 'd3d10umddi.md'
            if candidate.is_file():
                reference = candidate
                break
    if reference is not None and not reference.is_file():
        raise SystemExit('FAIL: no reference at ' + str(reference))
    files = sorted(list(sources.glob('*.cpp')) + list(sources.glob('*.h')))
    if not files:
        raise SystemExit('FAIL: no sources under ' + str(sources))
    failures, rows, assigned, sites = [], [], {}, 0

    parsed = {}
    for path in files:
        if path.name.endswith('-test.cpp'):
            continue
        parsed[path] = Source(path)
    # A name a header declares is callable from every file; a name inside an anonymous namespace is
    # not, and half the shell's entries are called create, size, bind or map. The call graph below
    # follows a name across files only when a header declares it.
    exported = set()
    for path, source in parsed.items():
        if path.suffix == '.h':
            exported |= set(CALL_NAME.findall(source.text))

    # Every table assignment, for the entry class of a thin wrapper and for rules 3 and 4.
    assigned_impl = {}
    for path, source in parsed.items():
        for m in re.finditer(r'\b(?:t|table)\.(pfn[A-Za-z0-9_]+)\s*=\s*([A-Za-z_]\w*)', source.text):
            entry, impl = m.group(1), m.group(2)
            if impl == 'nullptr':
                continue
            assigned.setdefault(entry, []).append((path.name, impl))
            if impl not in ('table', 'Unexpected'):
                assigned_impl.setdefault((path, impl), set()).add(entry)
    for m in re.finditer(r'UNEXPECTED\((pfn[A-Za-z0-9_]+)\)', '\n'.join(s.text for s in parsed.values())):
        assigned.setdefault(m.group(1), []).append(('(UNEXPECTED)', 'Unexpected'))

    def entry_class(entry):
        return POLICY.get(entry, (default_class(entry), None))[0]

    # The class in force in each top-level function, and whether it reports at all.
    described = {}
    for path, source in parsed.items():
        for span in source.top_level():
            cls, reports, parameterised = class_in_force(source, span)
            described[(path, span[0])] = (cls, reports, parameterised, span)

    def effective_class(path, name):
        """The class a caller runs under, or None when this gate cannot tell."""
        record = described.get((path, name))
        if record is None:
            return None
        cls, _, parameterised, _ = record
        if parameterised or isinstance(cls, list):
            return None
        if cls:
            return cls
        entries = assigned_impl.get((path, name))
        if entries:
            classes = {entry_class(e) for e in entries}
            return classes.pop() if len(classes) == 1 else None
        return None

    # Rules 1, 2 and 2b: the literal statuses of every report site, against the class in force.
    for path, source in parsed.items():
        for m in re.finditer(r'\breport_ddi_error\s*\(', source.text):
            span = owning(source.funcs, m.start())
            holder = span[0] if span else '(file scope)'
            if holder == 'report_ddi_error':
                continue  # its own definition
            sites += 1
            call, _ = arguments(source.text, m.end() - 1)
            status = call[1] if len(call) > 1 else ''
            cls, _, parameterised = class_in_force(source, span) if span else (None, False, False)
            if isinstance(cls, list):
                failures.append('%s:%d: %s carries two error classes (%s): one entry has one class'
                                % (path.name, source.line(m.start()), holder, ', '.join(cls)))
                continue
            if cls and cls != BASELINE_CLASS and len(call) < 3:
                failures.append('%s:%d: %s reports without naming its class %s, so the default class '
                                '%s reports in its place. Pass the class at every site.'
                                % (path.name, source.line(m.start()), holder, cls, BASELINE_CLASS))
            allowed = LEGAL_STATUSES if parameterised else ALLOWED[cls or BASELINE_CLASS]
            for token in sorted(set(STATUS_TOKEN.findall(status))):
                if token in NON_STATUS:
                    continue
                if token not in allowed:
                    failures.append('%s:%d: %s reports %s, which its class %s does not allow (%s). '
                                    'Fix the status, or state the entry class if the page allows it.'
                                    % (path.name, source.line(m.start()), holder, token,
                                       cls or BASELINE_CLASS, ', '.join(sorted(allowed)) or 'no status'))

    # Rule 1b: a reporting function's own signature. A status hidden in a default argument reaches the
    # runtime exactly like a literal at the report site: that is where BD-071's E_INVALIDARG sat.
    for path, source in parsed.items():
        for span in source.top_level():
            if span[0] == 'report_ddi_error':
                continue
            cls, reports, parameterised = class_in_force(source, span)
            if not reports or isinstance(cls, list):
                continue
            allowed = LEGAL_STATUSES if parameterised else ALLOWED[cls or BASELINE_CLASS]
            for token in sorted(set(STATUS_TOKEN.findall(header_of(source, span)))):
                if token in NON_STATUS or token in allowed:
                    continue
                failures.append('%s:%d: the signature of %s carries %s, which its class %s does not '
                                'allow (%s). A default argument reports like any other status.'
                                % (path.name, source.line(span[1]), span[0], token, cls or BASELINE_CLASS,
                                   ', '.join(sorted(allowed)) or 'no status'))

    # Rule 1c: a reporting helper against the entries that reach it. A helper one level below an entry
    # can declare a class of its own, and then a status the entry may not report looks legal to rule 1
    # while the runtime still sees it. A helper that takes the class as a parameter must carry only
    # statuses every one of its callers allows.
    for path, source in parsed.items():
        for span in source.top_level():
            name = span[0]
            if name == 'report_ddi_error':
                continue
            cls, reports, parameterised = class_in_force(source, span)
            if not reports or isinstance(cls, list):
                continue
            callers = {}
            for other_path, other in parsed.items():
                if other_path is not path and name not in exported:
                    continue
                for caller in other.top_level():
                    if caller[0] == name and other_path is path:
                        continue
                    if name not in CALL_NAME.findall(other.body(caller)):
                        continue
                    reached = effective_class(other_path, caller[0])
                    if reached:
                        callers[(other_path.name, caller[0])] = reached
            if not callers:
                continue
            if parameterised:
                guard = GUARDED_HELPERS.get((path.name, name))
                if guard and 'ddi_status_allowed(' in source.body(span):
                    continue
                allowed = set.intersection(*[ALLOWED[c] for c in callers.values()])
                # The signature counts as well: a default argument reports like any other status, and
                # that is where BD-071's E_INVALIDARG sat. Rule 1b cannot see it here, because a
                # parameterised helper has no class of its own to compare against.
                carried = source.body(span) + header_of(source, span)
                for token in sorted(set(STATUS_TOKEN.findall(carried))):
                    if token in NON_STATUS or token in allowed or token not in LEGAL_STATUSES:
                        continue
                    failures.append('%s:%d: %s takes its class as a parameter and carries %s, which '
                                    '%s does not allow. Its callers are %s.'
                                    % (path.name, source.line(span[1]), name, token,
                                       ', '.join(sorted(set(callers.values()))),
                                       ', '.join('%s:%s' % k for k in sorted(callers))))
                continue
            declared = cls or BASELINE_CLASS
            wrong = {k: v for k, v in callers.items() if v != declared}
            if wrong:
                failures.append('%s:%d: %s declares class %s, but it is reached from %s. A helper '
                                'reports under its caller\'s page: take the class as a parameter, or '
                                'declare the class those entries have.'
                                % (path.name, source.line(span[1]), name, declared,
                                   ', '.join('%s:%s (%s)' % (k[0], k[1], v) for k, v in sorted(wrong.items()))))

    # Rule 5: pfnSetErrorCb has one door.
    for path, source in parsed.items():
        for m in re.finditer(r'pfnSetErrorCb\s*\(', source.text):
            span = owning(source.funcs, m.start())
            holder = span[0] if span else '(file scope)'
            if (path.name, holder) in CALLBACK_ALLOWED:
                continue
            failures.append('%s:%d: %s calls pfnSetErrorCb directly. Report through report_ddi_error, '
                            'or add the site to CALLBACK_ALLOWED with its reason.'
                            % (path.name, source.line(m.start()), holder))

    # The class of every reporting function, by name, for the one-level delegation below: a table entry
    # may be a thin wrapper around the implementation that reports (the D3D11.1 view creates are).
    # Names are local to a file: half the shell's entries are called create, size, bind or map.
    reporting = {}
    for path, source in parsed.items():
        local = reporting.setdefault(path, {})
        for span in source.top_level():
            cls, reports, parameterised = class_in_force(source, span)
            if reports and not parameterised and not isinstance(cls, list):
                local.setdefault(span[0], set()).add(cls or BASELINE_CLASS)

    # Rule 3: each table entry's implementation carries the class this table gives the entry.
    for path, source in parsed.items():
        for m in re.finditer(r'\b(?:t|table)\.(pfn[A-Za-z0-9_]+)\s*=\s*([A-Za-z_]\w*)', source.text):
            entry, impl = m.group(1), m.group(2)
            if impl in ('nullptr', 'table', 'Unexpected'):
                continue
            expected = entry_class(entry)
            home, spans = path, source.spans(impl)
            if not spans:
                # The implementation may live in another file, which is where the DXGI entries are.
                elsewhere = [(p, s.spans(impl)) for p, s in parsed.items() if p is not path and s.spans(impl)]
                if len(elsewhere) == 1 and len(elsewhere[0][1]) == 1:
                    home, spans = elsewhere[0][0], elsewhere[0][1]
            if len(spans) != 1:
                failures.append('%s: %s=%s has %d definitions this gate can find, so its error class '
                                'is unknown. Give the entry one definition, or name it in POLICY.'
                                % (path.name, entry, impl, len(spans)))
                continue
            home_source = parsed[home]
            cls, reports, parameterised = class_in_force(home_source, spans[0])
            note = '' if home is path else ' (in %s)' % home.name
            if cls is None and not reports:
                # One level of delegation: the entry may be a wrapper around the reporting function.
                body = home_source.body(spans[0])
                local = reporting[home]
                delegates = {c for name in set(CALL_NAME.findall(body))
                             if name != impl and name not in CALL_NOISE and name in local
                             for c in local[name]}
                if len(delegates) != 1:
                    rows.append((entry, expected, home.name + ':' + impl, 'reports nothing'))
                    continue
                cls, reports, note = delegates.pop(), True, note + ' (delegated)'
            if isinstance(cls, list):
                continue  # already reported by rule 2
            actual = cls or BASELINE_CLASS
            rows.append((entry, expected, home.name + ':' + impl, 'class ' + actual + note))
            if parameterised:
                continue
            if actual != expected:
                failures.append('%s: %s=%s is class %s, but the table says %s for that entry (%s). '
                                'Change the code or the table, whichever the reference page supports.'
                                % (path.name, entry, impl, actual, expected,
                                   POLICY.get(entry, (None, 'general rule of handling-errors.md'))[1]))

    # Rule 4: no stale row.
    for entry in sorted(POLICY):
        if entry not in assigned:
            failures.append('POLICY names %s, which no device table assigns. Remove the row or fix '
                            'the entry name.' % entry)

    # Rule 8: the plane reaches the engine.
    for path, source in parsed.items():
        for span in source.top_level():
            if (path.name, span[0]) not in PLANE_BLIND:
                continue
            body = source.body(span)
            if 'PlaneSlice' in body:
                failures.append('%s:%d: %s reads PlaneSlice. A view create entry carries the plane to '
                                'the engine and refuses nothing of its own: the plane belongs to '
                                'plan_srv, plan_rtv and plan_uav, which the host test covers (BD-071).'
                                % (path.name, source.line(span[1]), span[0]))
            for m in PLANE_TEST.finditer(body):
                failures.append('%s:%d: %s tests the plane. These entries carry the plane and decide '
                                'nothing about it: every plane decision belongs to the planner, which '
                                'the host test covers (BD-071).'
                                % (path.name, source.line(span[1] + m.start()), span[0]))
    for name in sorted({name for _, name in PLANE_BLIND}):
        if not any((p.name, s[0]) in PLANE_BLIND for p, src in parsed.items() for s in src.top_level()
                   if s[0] == name):
            failures.append('PLANE_BLIND names %s, which no source defines. Fix the name, so the '
                            'plane path keeps its gate.' % name)

    # Rules 6 and 7: the table and the classes against the local Microsoft reference.
    checked = 0
    if reference is None:
        message = 'ref/ddi-display/d3d10umddi.md not found; the table was not cross-checked'
        if not args.allow_missing_reference:
            failures.append(message + '. Pass --reference, or --allow-missing-reference.')
        else:
            print('note: ' + message)
    else:
        pages = reference_sets(reference)
        for entry, (cls, page) in sorted(POLICY.items()):
            if page not in pages:
                failures.append('%s names reference page %s, which %s does not contain'
                                % (entry, page, reference.name))
                continue
            named, forbidden = pages[page]
            checked += 1
            for status in sorted(ALLOWED[cls]):
                if status in forbidden:
                    failures.append('%s: %s allows %s, but %s states that the entry cannot report it'
                                    % (entry, cls, status, page))
                elif status == REMOVED:
                    continue  # the AllowDeviceRemoved baseline, which no page has to name
                elif status not in named:
                    failures.append('%s: %s allows %s, which %s does not name' % (entry, cls, status, page))
        # Rule 7 covers every assigned entry, with a row or without one: a page that forbids device
        # removal must not meet a class that carries it, and the class of a rowless entry is a default.
        for page, (_, forbidden) in sorted(pages.items()):
            if REMOVED not in forbidden:
                continue
            key = page_entry_key(page)
            for entry in sorted(assigned):
                if entry[3:].upper() != key:
                    continue
                cls = entry_class(entry)
                if REMOVED in ALLOWED[cls]:
                    failures.append('%s: class %s carries %s, but %s states that this entry cannot '
                                    'report it. A capability-check entry answers after the device is '
                                    'gone.' % (entry, cls, REMOVED, page))

    if args.list:
        for entry, expected, impl, actual in sorted(rows):
            print('%-44s %-24s %-48s %s' % (entry, expected, impl, actual))
        return 0
    if failures:
        print('FAIL: DDI error policy')
        for f in failures:
            print('  ' + f)
        return 1
    print('PASS: %d report sites, %d table entries, %d policy rows (%d cross-checked against %s)'
          % (sites, len(assigned), len(POLICY), checked, reference.name if reference else 'nothing'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
