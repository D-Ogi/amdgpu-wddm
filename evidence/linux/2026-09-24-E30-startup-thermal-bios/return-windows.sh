set -eu
python3 - <<'PY'
from pathlib import Path
import os,hashlib
found=[]
for p in Path('/media').iterdir():
 if (p/'bc250/diag.py').is_file() and (p/'efi/boot/bootx64.efi').is_file():found.append(p)
print('matching_media='+str(len(found)))
assert len(found)==1
p=found[0];dev=os.stat(p).st_dev
sysdev=(Path('/sys/dev/block')/('%d:%d'%(os.major(dev),os.minor(dev)))).resolve()
assert (sysdev/'partition').is_file()
parent=sysdev.parent
size=int((parent/'size').read_text())*512
assert '/usb' in str(parent) and size==64160400896
print('usb_bus_verified=true size_bytes='+str(size))
for rel,expected in [('efi/boot/bootx64.efi','840e9a73f25507362e2a06cc8234fb51bef8bd536122275194270c4fa7552f6c'),('boot/grub/grub.cfg','380176a8f3aa952ba4cd2fb001f3b365a460be6f986cf5679b680ab4c70e68b6')]:
 digest=hashlib.sha256((p/rel).read_bytes()).hexdigest()
 print(rel+'_sha256='+digest)
 assert digest==expected
assert not (p/'efi/boot/bootx64.off').exists()
print('return_identity_preflight_passed')
PY

media=''
for p in /media/*; do
 [ -f "$p/bc250/diag.py" ] && [ -f "$p/efi/boot/bootx64.efi" ] && media="$p"
done
mount -o remount,rw "$media"
mv "$media/efi/boot/bootx64.efi" "$media/efi/boot/bootx64.off"
sync
mount -o remount,ro "$media"
echo windows_fallback_restored
printf 'return_request_utc='; date -u +%Y-%m-%dT%H:%M:%SZ
sync
reboot
