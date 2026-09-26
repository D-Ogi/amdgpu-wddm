set -eu
printf 'utc='; date -u +%Y-%m-%dT%H:%M:%SZ
printf 'kernel='; uname -r
printf 'alpine='; cat /etc/alpine-release
printf 'boot_epoch='; awk '$1=="btime" {print $2}' /proc/stat
[ ! -d /sys/module/amdgpu ] || { echo unexpected_amdgpu_loaded; exit 2; }
echo amdgpu_loaded=false
command -v python3
command -v flashrom || true
command -v modprobe
modprobe k10temp
python3 - <<'PY'
from pathlib import Path
import json,os,hashlib
for p in Path('/sys/class/hwmon').glob('hwmon*'):
 name=(p/'name').read_text().strip()
 if name!='k10temp':continue
 for q in p.glob('temp*_input'):
  v=int(q.read_text());print(json.dumps({'sensor':name,'channel':q.name,'millidegrees':v}),flush=True)
  assert v<85000
media=[p for p in Path('/media').iterdir() if (p/'bc250/diag.py').is_file()]
assert len(media)==1
p=media[0];print('diagnostic_media='+str(p))
for rel in ['boot/grub/grub.cfg','efi/boot/bootx64.efi']:
 print(rel+'_sha256='+hashlib.sha256((p/rel).read_bytes()).hexdigest())
gpus=[p for p in Path('/sys/bus/pci/devices').iterdir() if (p/'vendor').read_text().strip()=='0x1002' and (p/'device').read_text().strip()=='0x13fe']
assert len(gpus)==1
g=gpus[0];print('gpu='+g.name+' driver_bound='+str((g/'driver').exists()))
print('bar5_resource='+ (g/'resource').read_text().splitlines()[5])
print('efivars_available='+str(Path('/sys/firmware/efi/efivars').is_dir()))
print('AmdSetup_available='+str(Path('/sys/firmware/efi/efivars/AmdSetup-3a997502-647a-4c82-998e-52ef9486a247').is_file()))
PY
echo e30_preflight_complete
