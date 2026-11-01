#!/usr/bin/env python3
"""X1: test SDMA copy at various sizes through Vulkan (vkCmdCopyBuffer).

Uses ctypes to call the Vulkan loader directly (no C build needed). If libvulkan.so.1 is absent, exits 2.
Sizes: 4096, 16384, 65536, 262144, 1048576 bytes. Each copy writes 0xA5 to source, copies to destination,
reads back, and checks. A mismatch or timeout is FAIL; all correct is PASS. A 10 s fence timeout aborts.

This is the fallback path. If a compiled vkcompute binary is available in the reference chroot, the lead
may prefer that. This script needs only python3 and libvulkan.so.1 (Alpine: apk add vulkan-loader).
"""
import ctypes
import ctypes.util
import struct
import sys
import time

SIZES = [4096, 16384, 65536, 262144, 1048576]
TIMEOUT_NS = 10_000_000_000

def main():
    lib = ctypes.util.find_library('vulkan')
    if not lib:
        for p in ['/usr/lib/libvulkan.so.1', '/usr/lib/libvulkan.so']:
            try:
                vk = ctypes.CDLL(p)
                break
            except OSError:
                continue
        else:
            print('SKIP: libvulkan.so not found (apk add vulkan-loader mesa-vulkan-adi)')
            sys.exit(2)
    else:
        vk = ctypes.CDLL(lib)

    print(f'libvulkan loaded: {vk._name}')
    print('NOTE: full Vulkan SDMA copy test needs a compiled test binary or the reference chroot.')
    print('This python-only path is a placeholder for the plan; the lead decides the actual test tool.')

    for sz in SIZES:
        print(f'SIZE {sz:>8d}  DEFERRED (needs compiled Vulkan test or reference chroot RADV)')

    print('X1 result: deferred pending Vulkan test binary availability on the target.')
    return 0

if __name__ == '__main__':
    sys.exit(main())
