"""Enforce the error contract of the D3D11 shell's DDI entries against the sources.

A DDI entry with a void return reports a failure through pfnSetErrorCb, and each entry's reference page
names the statuses the runtime accepts from that entry. A status outside that set is not an error
report: the runtime treats it as a driver bug, logs it to Dr. Watson and removes the device on purpose
(ref/windows-driver-docs/windows-driver-docs-pr/display/handling-errors.md). BD-071 was that mistake
twice over: CreateShaderResourceView reported E_INVALIDARG for a plane slice, and CreateResource
reported E_NOTIMPL for the decoder bind flag. Both ended the device of a conforming application.

The shell states the allowed set per entry in driver/umd/dxvk/ddi-error-policy.h as an error class, and
every report carries that class. This gate checks the code against the table below, and the table
against the local Microsoft reference. It reads sources only: no GPU, no lab, no build.

It fails when
  1. a literal status in a report_ddi_error call is not one its error class allows,
  2. one function carries two different error classes,
  3. the class in force in an entry's implementation is not the class this table gives for the entry,
  4. a row here names an entry that no device table assigns,
  5. pfnSetErrorCb is called anywhere but in report_ddi_error, outside the allow-list below,
  6. a status this table allows is not named on the entry's reference page, or the page forbids it
     (skipped, with a note, when the local reference is not available).
"""
import argparse
from pathlib import Path
import re
import sys

# The error classes of ddi-error-policy.h, with the statuses each one admits. Keep both sides equal:
# the C++ enum is the implementation of this table.
ALLOWED = {
    'removed_only': {'D3DDDIERR_DEVICEREMOVED'},
    'out_of_memory': {'D3DDDIERR_DEVICEREMOVED', 'E_OUTOFMEMORY'},
    'unsupported': {'D3DDDIERR_DEVICEREMOVED', 'E_OUTOFMEMORY', 'DXGI_DDI_ERR_UNSUPPORTED'},
    'non_exclusive': {'D3DDDIERR_DEVICEREMOVED', 'E_OUTOFMEMORY', 'DXGI_DDI_ERR_NONEXCLUSIVE'},
    'still_drawing': {'D3DDDIERR_DEVICEREMOVED', 'DXGI_DDI_ERR_WASSTILLDRAWING'},
    'invalid_arg': {'E_INVALIDARG'},
    'invalid_arg_oom': {'E_INVALIDARG', 'E_OUTOFMEMORY'},
    'fail_or_invalid_arg': {'E_FAIL', 'E_INVALIDARG'},
    'unsupported_check': {'E_INVALIDARG', 'DXGI_DDI_ERR_UNSUPPORTED'},
    'nothing': set(),
}
# A void-return entry with no row reports device removal and nothing else: that is the general rule of
# handling-errors.md, and it is what every Set, Draw, Clear, Copy, Destroy and Flush page states.
DEFAULT_CLASS = 'removed_only'
# Every status a DDI entry may ever report. A literal outside this set is never an error report.
LEGAL_STATUSES = set()
for _s in ALLOWED.values():
    LEGAL_STATUSES |= _s

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
    # The check-type entries. Their pages state that they may never report device removal: the runtime
    # calls them after the device is gone and expects an answer.
    'pfnCheckFormatSupport': ('fail_or_invalid_arg', 'PFND3D10DDI_CHECKFORMATSUPPORT'),
    'pfnCheckMultisampleQualityLevels': ('invalid_arg', 'PFND3DWDDM1_3DDI_CHECKMULTISAMPLEQUALITYLEVELS'),
    'pfnCheckCounter': ('unsupported_check', 'PFND3D10DDI_CHECKCOUNTER'),
    'pfnCheckCounterInfo': ('nothing', 'PFND3D10DDI_CHECKCOUNTERINFO'),
    # GenMips: "can set E_FAIL if the base resource was not created with the appropriate flags or can
    # set E_INVALIDARG if the MIP type was incorrectly specified".
    'pfnGenMips': ('fail_or_invalid_arg', 'PFND3D10DDI_GENMIPS'),
    # The tile entries (WDDM 1.3). Their pages name E_INVALIDARG for a missing argument, and
    # ResizeTilePool adds E_OUTOFMEMORY for the page tables.
    'pfnUpdateTileMappings': ('invalid_arg', 'PFND3DWDDM1_3DDI_UPDATETILEMAPPINGS'),
    'pfnCopyTileMappings': ('invalid_arg', 'PFND3DWDDM1_3DDI_COPYTILEMAPPINGS'),
    'pfnGetMipPacking': ('invalid_arg', 'PFND3DWDDM1_3DDI_GETMIPPACKING'),
    'pfnResizeTilePool': ('invalid_arg_oom', 'PFND3DWDDM1_3DDI_RESIZETILEPOOL'),
}
# pfnSetErrorCb outside report_ddi_error. One door is the rule; this is the exception and its reason.
CALLBACK_ALLOWED = {
    ('ddi-entry.h', 'report_ddi_error'): 'the one door: it applies the entry class before it reports',
    ('runtime-bridge.cpp', 'Bc250HostLost'): 'the submission path, whose callers (draw, flush, map, '
                                             'present) all allow device removal; it reports nothing else',
}
STATUS_TOKEN = re.compile(r'\b(?:E_[A-Z0-9_]+|S_OK|S_FALSE|D3DDDIERR_[A-Z0-9_]+|DXGI_DDI_ERR_[A-Z0-9_]+|DXGI_ERROR_[A-Z0-9_]+)\b')


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
        for m in re.finditer(r'cannot call[^.]*?set the (?:<b>)?([A-Z_0-9]+)', section):
            forbidden.add(m.group(1))
        pages[name] = (named, forbidden)
    return pages


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sources', type=Path, default=None, help='driver/umd/dxvk (default: next to this script)')
    ap.add_argument('--reference', type=Path, default=None, help='ref/ddi-display/d3d10umddi.md')
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
    files = sorted(list(sources.glob('*.cpp')) + list(sources.glob('*.h')))
    if not files:
        raise SystemExit('FAIL: no sources under ' + str(sources))
    failures, rows, assigned, sites = [], [], {}, 0

    parsed = {}
    for path in files:
        if path.name.endswith('-test.cpp'):
            continue
        parsed[path] = Source(path)

    # Rules 1 and 2: the literal statuses of every report site, against the class in force.
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
            allowed = LEGAL_STATUSES if parameterised else ALLOWED[cls or DEFAULT_CLASS]
            for token in sorted(set(STATUS_TOKEN.findall(status))):
                if token in ('S_OK', 'S_FALSE'):
                    continue
                if token not in allowed:
                    failures.append('%s:%d: %s reports %s, which its class %s does not allow (%s). '
                                    'Fix the status, or state the entry class if the page allows it.'
                                    % (path.name, source.line(m.start()), holder, token,
                                       cls or DEFAULT_CLASS, ', '.join(sorted(allowed)) or 'no status'))

    # Rule 1b: a reporting function's own signature. A status hidden in a default argument reaches the
    # runtime exactly like a literal at the report site: that is where BD-071's E_INVALIDARG sat.
    for path, source in parsed.items():
        for span in source.funcs:
            if owning(source.funcs, span[1]) != span or span[0] == 'report_ddi_error':
                continue
            cls, reports, parameterised = class_in_force(source, span)
            if not reports or isinstance(cls, list):
                continue
            allowed = LEGAL_STATUSES if parameterised else ALLOWED[cls or DEFAULT_CLASS]
            for token in sorted(set(STATUS_TOKEN.findall(header_of(source, span)))):
                if token in ('S_OK', 'S_FALSE') or token in allowed:
                    continue
                failures.append('%s:%d: the signature of %s carries %s, which its class %s does not '
                                'allow (%s). A default argument reports like any other status.'
                                % (path.name, source.line(span[1]), span[0], token, cls or DEFAULT_CLASS,
                                   ', '.join(sorted(allowed)) or 'no status'))

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
        for span in source.funcs:
            if owning(source.funcs, span[1]) != span:
                continue
            cls, reports, parameterised = class_in_force(source, span)
            if reports and not parameterised and not isinstance(cls, list):
                local.setdefault(span[0], set()).add(cls or DEFAULT_CLASS)

    # Rule 3: each table entry's implementation carries the class this table gives the entry.
    for path, source in parsed.items():
        for m in re.finditer(r'\b(?:t|table)\.(pfn[A-Za-z0-9_]+)\s*=\s*([A-Za-z_]\w*)', source.text):
            entry, impl = m.group(1), m.group(2)
            if impl == 'nullptr':
                continue
            assigned.setdefault(entry, []).append((path.name, impl))
            expected = POLICY.get(entry, (DEFAULT_CLASS, None))[0]
            spans = source.spans(impl)
            if len(spans) != 1:
                if impl not in ('table', 'Unexpected'):
                    rows.append((entry, expected, path.name + ':' + impl,
                                 'not resolved (%d definitions)' % len(spans)))
                continue
            cls, reports, parameterised = class_in_force(source, spans[0])
            note = ''
            if cls is None and not reports:
                # One level of delegation: the entry may be a wrapper around the reporting function.
                body = source.text[spans[0][1]:spans[0][2]]
                local = reporting[path]
                delegates = {c for name in set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', body))
                             if name != impl and name in local for c in local[name]}
                if len(delegates) != 1:
                    rows.append((entry, expected, path.name + ':' + impl, 'reports nothing'))
                    continue
                cls, reports, note = delegates.pop(), True, ' (delegated)'
            if isinstance(cls, list):
                continue  # already reported by rule 2
            actual = cls or DEFAULT_CLASS
            rows.append((entry, expected, path.name + ':' + impl, 'class ' + actual + note))
            if parameterised:
                continue
            if actual != expected:
                failures.append('%s: %s=%s is class %s, but the table says %s for that entry (%s). '
                                'Change the code or the table, whichever the reference page supports.'
                                % (path.name, entry, impl, actual, expected,
                                   POLICY.get(entry, (None, 'general rule of handling-errors.md'))[1]))
    for m in re.finditer(r'UNEXPECTED\((pfn[A-Za-z0-9_]+)\)', '\n'.join(s.text for s in parsed.values())):
        assigned.setdefault(m.group(1), []).append(('(UNEXPECTED)', 'Unexpected'))

    # Rule 4: no stale row.
    for entry in sorted(POLICY):
        if entry not in assigned:
            failures.append('POLICY names %s, which no device table assigns. Remove the row or fix '
                            'the entry name.' % entry)

    # Rule 6: the table against the local Microsoft reference.
    checked = 0
    if reference is None:
        print('note: ref/ddi-display/d3d10umddi.md not found; the table was not cross-checked')
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
                elif status not in named:
                    failures.append('%s: %s allows %s, which %s does not name' % (entry, cls, status, page))
            for status in sorted(forbidden & ALLOWED[cls]):
                failures.append('%s: %s is forbidden by %s' % (entry, status, page))

    if args.list:
        for entry, expected, impl, actual in sorted(rows):
            print('%-44s %-20s %-44s %s' % (entry, expected, impl, actual))
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
