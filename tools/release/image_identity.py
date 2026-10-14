"""Windows SHA-256 catalog image identity, also used by the D3D11 loader.

This identifies code independently of its embedded certificate. It does not
verify signer trust; signtool and the package catalog enforce that separately.
PROVENANCE: Microsoft WDK mscat.h, CryptCATAdminCalcHashFromFileHandle2.
"""
import ctypes
from ctypes import wintypes
import os
import argparse
import json
from pathlib import Path


def image_digest(path):
    if os.name != 'nt':
        raise OSError('Windows catalog hashing is required')
    import msvcrt
    # Catalog hashing accepts non-PE input too. This contract does not.
    with Path(path).open('rb') as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:2] != b'MZ':
            raise ValueError(f'{path}: not a PE image')
        stream.seek(int.from_bytes(header[60:64], 'little'))
        if stream.read(4) != b'PE\0\0':
            raise ValueError(f'{path}: invalid PE header')
        stream.seek(0)
        dll = ctypes.WinDLL('wintrust', use_last_error=True)
        acquire = dll.CryptCATAdminAcquireContext2
        acquire.argtypes = [ctypes.POINTER(wintypes.HANDLE), ctypes.c_void_p,
                            wintypes.LPCWSTR, ctypes.c_void_p, wintypes.DWORD]
        acquire.restype = wintypes.BOOL
        calc = dll.CryptCATAdminCalcHashFromFileHandle2
        calc.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD),
                         ctypes.c_void_p, wintypes.DWORD]
        calc.restype = wintypes.BOOL
        release = dll.CryptCATAdminReleaseContext
        release.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        release.restype = wintypes.BOOL
        context = wintypes.HANDLE()
        if not acquire(ctypes.byref(context), None, 'SHA256', None, 0):
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            size = wintypes.DWORD(32)
            digest = (ctypes.c_ubyte * 32)()
            if not calc(context, msvcrt.get_osfhandle(stream.fileno()), ctypes.byref(size), digest, 0):
                raise ctypes.WinError(ctypes.get_last_error())
            if size.value != 32:
                raise ValueError('unexpected catalog SHA-256 size')
            return bytes(digest).hex().upper()
        finally:
            release(context, 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--write', type=Path)
    group.add_argument('--check', type=Path)
    args = parser.parse_args()
    try:
        root = args.directory.resolve(strict=True)
        images = sorted(p for p in root.rglob('*') if p.is_file() and p.suffix.lower() in ('.dll', '.exe', '.sys'))
        if not images:
            raise ValueError('empty image identity set')
        actual = {p.relative_to(root).as_posix(): image_digest(p) for p in images}
        if args.write:
            args.write.write_text(json.dumps(actual, indent=2) + '\n', encoding='utf-8')
        elif actual != json.loads(args.check.read_text(encoding='utf-8')):
            raise ValueError('signing changed an image or the image set')
        print(f'PASS {len(images)} PE image identities')
    except (OSError, ValueError) as exc:
        parser.exit(1, f'image identity failed: {exc}\n')
