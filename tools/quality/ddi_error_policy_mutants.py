"""Negative controls for ddi_error_policy.py: every mutation below must fail that gate.

A gate nobody tests is a comment with a shebang. BD-071 was a status the runtime does not allow from
CreateShaderResourceView, and the first version of the gate that refuses that status let the same
defect back in one level down, in a helper that declared a class of its own. So each entry here
reintroduces one defect the gate claims to refuse, on a copy of the sources, and this script fails if
the gate passes any of them. It also fails if the unmutated copy does not pass.

The anchors are exact source text. When an anchor stops matching, this script says SETUP FAILED and
names the file: move the anchor to the line that carries the same meaning, and keep the mutation.

It reads sources only: no GPU, no lab, no build.
"""
import argparse
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

GATE = Path(__file__).resolve().with_name('ddi_error_policy.py')

# name -> (file, anchor(s), replacement(s), the rule that must catch it)
MUTANTS = {
    # The defect itself: a plane slice refused inside the WDDM 2.0 create entry. The status here is
    # one the entry's page allows, so the status rules cannot see it. The plane rule can.
    'plane-refusal-legal-status': (
        'ddi-wddm2.cpp',
        '    D3D11DDIARG_CREATESHADERRESOURCEVIEW d{}; UINT plane=0;\n    convert_wddm2_srv(*s,d,plane);',
        '    if (s->ResourceDimension==D3D10DDIRESOURCE_TEXTURE2D && s->Tex2D.PlaneSlice) {\n'
        '        reject(h,DdiErrorClass::out_of_memory); return;\n    }\n'
        '    D3D11DDIARG_CREATESHADERRESOURCEVIEW d{}; UINT plane=0;\n    convert_wddm2_srv(*s,d,plane);',
        'the plane rule'),
    # The same refusal one level down, in the shared implementation, where no PlaneSlice is in sight.
    'plane-refusal-in-implementation': (
        'ddi-srv.cpp',
        '        SrvRequest request{}; HRESULT hr=plan_srv(*desc,layers,samples,plane,request);',
        '        if (plane && plane!=ddi_plane_from_view_format) { refuse(); return; }\n'
        '        SrvRequest request{}; HRESULT hr=plan_srv(*desc,layers,samples,plane,request);',
        'the plane rule'),
    # A named helper that declares a class of its own and is reached from an entry of another class.
    # The report site then looks legal for the class the helper named, and the runtime still sees a
    # status the entry may not report. This is BD-071 one level down.
    'helper-declares-another-class': (
        'ddi-srv.cpp',
        ['namespace {\nDeviceOwner &owner(D3D10DDI_HDEVICE h)',
         '        SrvRequest request{}; HRESULT hr=plan_srv(*desc,layers,samples,plane,request);'],
        ['namespace {\nvoid probe_refuse(DeviceOwner &o) { report_ddi_error(o,E_INVALIDARG,DdiErrorClass::check_invalid_arg); }\n'
         'DeviceOwner &owner(D3D10DDI_HDEVICE h)',
         '        if (!layers) { probe_refuse(o); return; }\n'
         '        SrvRequest request{}; HRESULT hr=plan_srv(*desc,layers,samples,plane,request);'],
        'the helper rule'),
    # A report site inside an entry whose class is not the baseline, with the class left out, so the
    # default class reports device removal from a page that forbids it.
    'site-without-its-class': (
        'ddi-format.cpp',
        'if (!out || !owner.device()) { report_ddi_error(owner,E_INVALIDARG,DdiErrorClass::check_invalid_arg); return; }',
        'if (!out || !owner.device()) { report_ddi_error(owner,E_INVALIDARG); return; }',
        'the explicit-class rule'),
    # A helper that takes its class as a parameter and carries a status one of its callers forbids.
    'parameterised-helper-illegal-status': (
        'ddi-wddm2.cpp',
        'report_ddi_error(owner(h),D3DDDIERR_DEVICEREMOVED,policy); },policy);',
        'report_ddi_error(owner(h),E_OUTOFMEMORY,policy); },policy);',
        'the helper rule'),
    # The same status hidden in that helper's signature, which is where BD-071 shipped.
    'status-in-helper-signature': (
        'ddi-wddm2.cpp',
        'void reject(D3D10DDI_HDEVICE h,DdiErrorClass policy) {',
        'void reject(D3D10DDI_HDEVICE h,DdiErrorClass policy=DdiErrorClass::removed_only,HRESULT hidden=E_INVALIDARG) {\n'
        '    (void)hidden;',
        'the helper rule'),
    # A capability-check entry that reports device removal, which its page forbids by name.
    'check-entry-reports-removal': (
        'ddi-table.cpp',
        'void APIENTRY deferred_sizes(D3D10DDI_HDEVICE,UINT *count,D3D11DDI_HANDLESIZE *) {\n    if(count)*count=0;\n}',
        'void APIENTRY deferred_sizes(D3D10DDI_HDEVICE h,UINT *count,D3D11DDI_HANDLESIZE *) {\n'
        '    if(count)*count=0;\n'
        '    auto *storage=static_cast<DdiDeviceHandle *>(h.pDrvPrivate);\n'
        '    if(storage && storage->owner) report_ddi_error(*storage->owner,D3DDDIERR_DEVICEREMOVED);\n}',
        'the capability-check rule'),
    # A direct pfnSetErrorCb, which is how a status gets past every class.
    'direct-callback': (
        'ddi-srv.cpp',
        '        } else if (!view) report_ddi_error(o,D3DDDIERR_DEVICEREMOVED,DdiErrorClass::out_of_memory);',
        '        } else if (!view) { auto &rt=o.runtime(); rt.UMCallbacks.pfnSetErrorCb(rt.hRTCoreLayer,E_INVALIDARG); }',
        'the one-door rule'),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sources', type=Path, default=None, help='driver/umd/dxvk')
    ap.add_argument('--reference', type=Path, default=None, help='ref/ddi-display/d3d10umddi.md')
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[2]
    sources = args.sources or repo / 'driver' / 'umd' / 'dxvk'
    reference = args.reference
    if reference is None:
        roots = [Path(os.environ['BC250_ROOT'])] if os.environ.get('BC250_ROOT') else []
        roots.append(repo.parent)
        for root in roots:
            candidate = root / 'ref' / 'ddi-display' / 'd3d10umddi.md'
            if candidate.is_file():
                reference = candidate
                break
    if reference is None or not reference.is_file():
        raise SystemExit('FAIL: no reference; pass --reference ref/ddi-display/d3d10umddi.md')

    failures, caught = [], 0
    with tempfile.TemporaryDirectory(prefix='ddi-policy-mutants-') as work:
        copy = Path(work) / 'dxvk'
        copy.mkdir()
        for name in sorted(os.listdir(sources)):
            if name.endswith(('.cpp', '.h')):
                shutil.copy2(sources / name, copy / name)

        def gate():
            done = subprocess.run([sys.executable, str(GATE), '--sources', str(copy),
                                   '--reference', str(reference)], capture_output=True, text=True)
            return done.returncode, done.stdout + done.stderr

        code, output = gate()
        if code != 0:
            failures.append('the unmutated copy does not pass the gate: ' + output.strip())
        for name, (source, old, new, rule) in sorted(MUTANTS.items()):
            path = copy / source
            text = io.open(path, encoding='utf-8', newline='').read()
            edits = list(zip(old, new)) if isinstance(old, list) else [(old, new)]
            mutated, broken = text, False
            for one_old, one_new in edits:
                wanted = one_old.replace('\n', '\r\n') if '\r\n' in text else one_old
                replacement = one_new.replace('\n', '\r\n') if '\r\n' in text else one_new
                if mutated.count(wanted) != 1:
                    failures.append('%s: SETUP FAILED, %d matches in %s. Move the anchor to the line '
                                    'that carries the same meaning.'
                                    % (name, mutated.count(wanted), source))
                    broken = True
                    break
                mutated = mutated.replace(wanted, replacement)
            if broken:
                continue
            io.open(path, 'w', encoding='utf-8', newline='').write(mutated)
            code, output = gate()
            io.open(path, 'w', encoding='utf-8', newline='').write(text)
            if code == 0:
                failures.append('%s: the gate passed this mutation, which %s must refuse.' % (name, rule))
            else:
                caught += 1

    if failures:
        print('FAIL: DDI error policy mutants')
        for f in failures:
            print('  ' + f)
        return 1
    print('PASS: %d mutations, each refused by the DDI error policy gate' % caught)
    return 0


if __name__ == '__main__':
    sys.exit(main())
