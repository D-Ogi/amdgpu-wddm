"""Configure/build the pinned M12 clvk tree after applying clvk-windows.patch."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sources', type=Path, required=True)
    ap.add_argument('--build', type=Path)
    ap.add_argument('--vulkan-include', type=Path)
    ap.add_argument('--vulkan-library', type=Path)
    ap.add_argument('--cmake', default='cmake')
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--verify-only', action='store_true')
    args = ap.parse_args()
    here = Path(__file__).resolve().parent
    config = json.loads((here / 'clvk-source.json').read_text())
    sources = args.sources.resolve()
    for entry in config['sources']:
        path = sources / entry['directory']
        actual = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != entry['commit']:
            raise RuntimeError(f"Source revision mismatch: {entry['name']}")
    for name, expected in config['patched_clvk_sha256'].items():
        actual = hashlib.sha256((sources / 'clvk' / name).read_bytes().replace(b'\r\n', b'\n')).hexdigest()
        if actual != expected:
            raise RuntimeError(f'Patched clvk file mismatch: {name}')
    patch_hash = hashlib.sha256((here / 'clvk-windows.patch').read_bytes().replace(b'\r\n', b'\n')).hexdigest()
    if patch_hash != config['patch_sha256']:
        raise RuntimeError('Patch identity mismatch')
    print('Pinned source revisions and patched clvk files verified', flush=True)
    if args.verify_only:
        return
    if args.build is None or args.jobs < 1:
        ap.error('--build and positive --jobs are required for a build')
    if os.name == 'nt' and (args.vulkan_include is None or args.vulkan_library is None):
        ap.error('Windows requires --vulkan-include and --vulkan-library')
    opts = {
        'CMAKE_BUILD_TYPE': 'Release', 'CLVK_ENABLE_ASSERTIONS': 'ON',
        'CLVK_CLSPV_ONLINE_COMPILER': 'ON', 'CLVK_BUILD_TESTS': 'ON',
        'CLVK_BUILD_STATIC_TESTS': 'OFF', 'CLVK_ENABLE_SPIRV_IL': 'ON',
        'LLVM_PARALLEL_LINK_JOBS': '1', 'SKIP_CLSPV_INSTALL': 'ON',
        'OPENCL_HEADERS_SOURCE_DIR': sources / 'OpenCL-Headers',
        'SPIRV_HEADERS_SOURCE_DIR': sources / 'SPIRV-Headers-clvk',
        'SPIRV_TOOLS_SOURCE_DIR': sources / 'SPIRV-Tools',
        'CLSPV_SOURCE_DIR': sources / 'clspv',
        'LLVM_SPIRV_SOURCE': sources / 'SPIRV-LLVM-Translator',
        'CLSPV_LLVM_SOURCE_DIR': sources / 'llvm/llvm',
        'CLSPV_CLANG_SOURCE_DIR': sources / 'llvm/clang',
        'CLSPV_LIBCLC_SOURCE_DIR': sources / 'llvm/libclc',
    }
    if os.name == 'nt':
        opts.update(CMAKE_C_COMPILER='cl', CMAKE_CXX_COMPILER='cl')
    if args.vulkan_include:
        opts['Vulkan_INCLUDE_DIR'] = args.vulkan_include.resolve()
    if args.vulkan_library:
        opts['Vulkan_LIBRARY'] = args.vulkan_library.resolve()
    build = args.build.resolve()
    command = [args.cmake, '-S', str(sources / 'clvk'), '-B', str(build), '-G', 'Ninja']
    command += [f'-D{k}={v.as_posix() if isinstance(v, Path) else v}' for k, v in opts.items()]
    subprocess.run(command, check=True)
    subprocess.run([args.cmake, '--build', str(build), '--parallel', str(args.jobs)], check=True)


if __name__ == '__main__':
    main()
