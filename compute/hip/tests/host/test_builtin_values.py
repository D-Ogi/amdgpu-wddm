"""Compile device builtin contracts without opening a GPU and check IR witnesses."""
import argparse
import json
import pathlib
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--clang', required=True)
    parser.add_argument('--include', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--negative', action='store_true')
    args = parser.parse_args()
    here = pathlib.Path(__file__).resolve().parent
    output = pathlib.Path(args.out)
    output.mkdir(parents=True, exist_ok=True)
    compiler = [
        args.clang, '-x', 'hip', '--offload-arch=gfx1013', '--offload-device-only',
        '-nogpuinc', '-nogpulib', '-O2', '-std=c++17', '-I', args.include,
    ]
    result = subprocess.run(
        compiler + ['-S', '-emit-llvm', str(here / 'builtin_values.hip'),
                    '-o', str(output / 'builtins.ll')],
        text=True, capture_output=True,
    )
    (output / 'compile.txt').write_text(result.stdout + result.stderr)
    if result.returncode:
        expected_failure = (
            args.negative and 'atomicExch<float>' in result.stderr
            and 'pointer to integer or pointer' in result.stderr
        )
        if expected_failure:
            print('negative control: old header refused implemented float atomicExch')
            return 0
        raise SystemExit('builtin compilation failed; see compile.txt')

    ir = (output / 'builtins.ll').read_text()
    expected = json.loads((here / 'builtin_values.json').read_text())
    failures = []
    for name, value in expected.items():
        body = re.search(r'define[^\n]*' + name + r'[^\n]*\{(.*?)\n\}', ir, re.S)
        signed = value if value < 2**31 else value - 2**32
        if not body or not re.search(r'store i32 ' + str(signed) + r',', body.group(1)):
            failures.append(name)
    if 'atomicrmw xchg' not in ir or 'syncscope("agent")' not in ir:
        failures.append('atomic-agent')
    # System operation has no narrowed syncscope between its operand and ordering.
    if not re.search(r'atomicrmw add[^\n]*, i32 1 monotonic', ir):
        failures.append('atomic-system')
    print(f'builtin contracts: {len(expected)} values, {len(failures)} failures: {failures}')
    if args.negative:
        if failures:
            return 0
        raise SystemExit('negative control unexpectedly passed')

    # Device link also rejects unsupported variadic printf. An unused helper
    # remains legal because it contributes no reachable device reference.
    admission = [
        ('printf', 'printf("unsupported");', '__bc250_device_printf_requires_hostcall_service'),
        ('sleep', '__nanosleep(100);', '__bc250_nanosleep_unsupported'),
        ('unused', '', None),
    ]
    for name, body, diagnostic in admission:
        source = output / (name + '.hip')
        helper = ('__device__ inline void helper(){printf("x");__nanosleep(1);}\n'
                  if name == 'unused' else '')
        source.write_text('#include <hip/hip_runtime.h>\n' + helper
                          + '__global__ void admission(){' + body + '}\n')
        result = subprocess.run(
            compiler + [str(source), '-o', str(output / (name + '.co'))],
            text=True, capture_output=True,
        )
        (output / (name + '.txt')).write_text(result.stdout + result.stderr)
        if diagnostic:
            if result.returncode == 0 or diagnostic not in result.stderr:
                failures.append(name)
        elif result.returncode != 0:
            failures.append(name)
    if failures:
        print('FAIL ' + str(failures))
    else:
        print('unsupported admission: two reachable refusals, unused helper accepted')
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())
