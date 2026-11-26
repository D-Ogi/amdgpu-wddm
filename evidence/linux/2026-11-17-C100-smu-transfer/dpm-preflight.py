"""Read-only operator preflight; no mailbox, power action or resource mapping."""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import subprocess
import sys

STOCK = Path('/root/bc250-t001-stock-observer-r1')
assert hashlib.sha256((STOCK/'SHA256.json').read_bytes()).hexdigest() == '4a9db0421597a0ed9978dafa90c9f7837c34428fe817bf3c0110b2d5ac3b0602'
for name, digest in json.loads((STOCK/'SHA256.json').read_text())['files'].items():
    assert Path(name).name == name
    assert hashlib.sha256((STOCK/name).read_bytes()).hexdigest() == digest
sys.path.insert(0, str(STOCK))
spec = importlib.util.spec_from_file_location('stock_preflight', STOCK/'linux_observer.py')
stock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stock)
dev, sensor, info = stock.preflight()
assert not Path('/sys/firmware/acpi/tables/IVRS').exists(), 'IVRS present'
iommu = Path('/sys/class/iommu')
assert not iommu.exists() or not any(iommu.iterdir()), 'active IOMMU'
assert not (dev/'iommu_group').exists(), 'GPU IOMMU group present'
kmsg = subprocess.run(['dmesg'], check=True, capture_output=True, text=True, timeout=5).stdout.lower()
for line in kmsg.splitlines():
    assert 'amd-vi' not in line, 'AMD-Vi initialization requires review'
    if 'iommu' in line:
        assert any(s in line for s in ('iommu: default domain type:', 'iommu: dma domain tlb invalidation policy:')), 'unreviewed IOMMU log'
patterns = ('native_activation.py', 'observe.py', 'linux_observer.py', 'unlock.py', 'smu-tool', 'ryzenadj')
conflicts = []
for proc in Path('/proc').iterdir():
    if not proc.name.isdigit() or int(proc.name) == os.getpid():
        continue
    try:
        args = (proc/'cmdline').read_bytes().split(b'\0')
    except (FileNotFoundError, ProcessLookupError):
        continue
    matched = [p for p in patterns if any(p.encode() in arg for arg in args)]
    if matched:
        conflicts.append({'pid': int(proc.name), 'tools': matched})
assert not conflicts, 'competing SMU process: ' + json.dumps(conflicts)
boot = Path('/media/usb')
pins = {
    'boot/grub/grub.cfg': '97a5906372446889b7af9ace4047fa80220b90c2fd27fc11e775b87a32852455',
    'boot/grub/grubenv': 'd824be8cf1612cac8cab3f7d4a166e72d02740a0ddba55eefc4bf533e2cb59a9',
    'efi/boot/bootx64.efi': '2b707cc62b030bb3c6dfe375763da6d4ba4342c02bb953ea0ab3236ef067bf48',
}
for name, digest in pins.items():
    assert hashlib.sha256((boot/name).read_bytes()).hexdigest() == digest, name
print(json.dumps({'pass': True, 'identity': info, 'sensor': str(sensor), 'iommu_absent': True,
                  'competing_smu_processes': conflicts, 'next_boot_original_default6': True,
                  'kernel_release': os.uname().release, 'hardware_commands_issued': False}, sort_keys=True))
