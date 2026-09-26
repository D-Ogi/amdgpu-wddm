set -eu
python3 - <<'PY'
from pathlib import Path
import hashlib,struct,json,time
p=Path('/sys/firmware/efi/efivars/AmdSetup-3a997502-647a-4c82-998e-52ef9486a247')
data=p.read_bytes();assert len(data)>=4
out=Path('/tmp/e30-private');out.mkdir(mode=0o700,exist_ok=True)
f=out/'AmdSetup.efivar';assert not f.exists();f.write_bytes(data);f.chmod(0o600)
print(json.dumps({'variable':'AmdSetup','efivar_bytes':len(data),'payload_bytes':len(data)-4,'attributes':struct.unpack_from('<I',data)[0],'sha256_efivar':hashlib.sha256(data).hexdigest(),'sha256_payload':hashlib.sha256(data[4:]).hexdigest(),'utc_epoch':time.time()}))
PY
printf 'apk_installed_flashrom='; apk info -e flashrom || true
apk policy flashrom || true
printf 'spi_driver='; test -d /sys/module/spi_amd && echo loaded || true
printf 'mtd_device_count='; find /sys/class/mtd -mindepth 1 -maxdepth 1 2>/dev/null | wc -l
printf 'tools_check_done='; date -u +%Y-%m-%dT%H:%M:%SZ
