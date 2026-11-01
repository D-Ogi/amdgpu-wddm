#!/usr/bin/env python3
"""D3 control: the same windows as d3-l28-irq.py (enable step, 5 s modetest, 1 s settle) with no register write."""
import subprocess, time


def irqs():
    return sum(int(x) for line in open('/proc/interrupts') if 'amdgpu' in line for x in line.split()[1:] if x.isdigit())


i0 = irqs(); time.sleep(0.001); i1 = irqs()
p = subprocess.Popen(["modetest", "-M", "amdgpu", "-s", "64@58:1920x1200", "-P", "45@58:1920x1200@XR24"],
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
try:
    p.wait(timeout=5)
except subprocess.TimeoutExpired:
    p.kill(); p.wait()
i2 = irqs(); time.sleep(1); i3 = irqs()
print(f"CONTROL after_enable_delta={i1 - i0} during_modetest={i2 - i1} total={i3 - i0}")
