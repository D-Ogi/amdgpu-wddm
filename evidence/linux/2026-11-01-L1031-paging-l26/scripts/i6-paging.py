#!/usr/bin/env python3
"""L26/X1: make amdgpu evict VRAM to GTT (debugfs amdgpu_evict_vram, ttm eviction = SDMA paging copies through
amdgpu_copy_buffer), with the VRAM manager state before and after. The copy_buf kprobe records each copy size."""
import glob
import time

d = sorted(glob.glob('/sys/kernel/debug/dri/*/amdgpu_evict_vram'))[0].rsplit('/', 1)[0]


def dump(name):
    try:
        return open(f'{d}/{name}').read()
    except OSError as e:
        return f'ERR {e}\n'


for label in ('before', 'evict', 'after'):
    if label == 'evict':
        t0 = time.monotonic()
        print('== evict_vram result:', dump('amdgpu_evict_vram').strip(), f'({time.monotonic() - t0:.3f} s)')
        continue
    print(f'== {label} amdgpu_vram_mm (tail)')
    print(''.join(dump('amdgpu_vram_mm').splitlines(True)[-12:]))
    print(f'== {label} amdgpu_gtt_mm (tail)')
    print(''.join(dump('amdgpu_gtt_mm').splitlines(True)[-6:]))
