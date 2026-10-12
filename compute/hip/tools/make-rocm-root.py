#!/usr/bin/env python3
"""Assemble a ROCm-shaped root out of this repository's HIP tree (M16, route B, step 3).

A HIP program expects one directory that holds four things at the places ROCm puts them:

    <root>/include/hip/...          the headers clang's HIP wrapper and the program include
    <root>/include/hipblas/...      the hipBLAS interface ggml-hip includes
    <root>/amdgcn/bitcode/*.bc      the device library clang links the device pass against
    <root>/lib/cmake/<pkg>/...      the CMake packages find_package(hip|hipblas|rocblas) reads
    <root>/lib/*.lib                the import libraries the host side links

clang finds the first three with --rocm-path=<root>; CMake finds the fourth with
CMAKE_PREFIX_PATH=<root> or ROCM_PATH=<root>. This script builds that root by copying, so the
result is a plain directory with no link and no dependency on this checkout's layout.

    python make-rocm-root.py --out <root> [--bitcode <dir>] [--lib <dir>]

The device library bitcode is not in this repository: it is built from AMD's ROCm device library
sources (MIT) and its own build directory is named on the command line.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HIP_DIR = os.path.dirname(HERE)              # compute/hip
INCLUDE_DIR = os.path.join(HIP_DIR, "include")
CMAKE_DIR = os.path.join(HIP_DIR, "cmake")

PACKAGES = {
    "hip": ("hip-config.cmake", "hip-config-version.cmake"),
    "hipblas": ("hipblas-config.cmake",),
    "rocblas": ("rocblas-config.cmake",),
}


def copy_tree(src: str, dst: str) -> int:
    count = 0
    for root, _dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        target = os.path.join(dst, rel) if rel != "." else dst
        os.makedirs(target, exist_ok=True)
        for name in files:
            shutil.copyfile(os.path.join(root, name), os.path.join(target, name))
            count += 1
    return count


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="the root to assemble")
    ap.add_argument("--bitcode", help="a directory of ROCm device library .bc files")
    ap.add_argument("--lib", help="a directory that holds amdhip64.lib and bc250hipblas.lib")
    args = ap.parse_args()

    root = os.path.abspath(args.out)
    os.makedirs(root, exist_ok=True)

    headers = copy_tree(INCLUDE_DIR, os.path.join(root, "include"))
    print(f"include: {headers} headers")

    for package, files in PACKAGES.items():
        target = os.path.join(root, "lib", "cmake", package)
        os.makedirs(target, exist_ok=True)
        for name in files:
            src = os.path.join(CMAKE_DIR, name)
            if not os.path.exists(src):
                print(f"missing CMake file {src}", file=sys.stderr)
                return 2
            shutil.copyfile(src, os.path.join(target, name))
        print(f"lib/cmake/{package}: {len(files)} files")

    if args.bitcode:
        target = os.path.join(root, "amdgcn", "bitcode")
        os.makedirs(target, exist_ok=True)
        n = 0
        for name in sorted(os.listdir(args.bitcode)):
            if name.endswith(".bc"):
                shutil.copyfile(os.path.join(args.bitcode, name), os.path.join(target, name))
                n += 1
        print(f"amdgcn/bitcode: {n} bitcode files")
        if n == 0:
            print("no .bc file found; the device pass will need -nogpulib", file=sys.stderr)
    else:
        print("amdgcn/bitcode: skipped (--bitcode not given)")

    if args.lib:
        target = os.path.join(root, "lib")
        os.makedirs(target, exist_ok=True)
        n = 0
        for name in sorted(os.listdir(args.lib)):
            if name.lower().endswith(".lib") or name.lower().endswith(".dll"):
                shutil.copyfile(os.path.join(args.lib, name), os.path.join(target, name))
                n += 1
        print(f"lib: {n} binaries")
    else:
        print("lib: skipped (--lib not given); find_package(hip) will refuse this root")

    print(f"\nroot: {root}")
    print("  clang: --rocm-path=<root>")
    print("  cmake: -DCMAKE_PREFIX_PATH=<root>  (or ROCM_PATH=<root> in the environment)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
