set -eu
python3 - <<'PY'
from pathlib import Path
for p in Path('/sys/bus/pci/devices').iterdir():
 if (p/'class').read_text().strip().startswith('0x0601'):
  print('isa_bridge='+p.name+' '+(p/'vendor').read_text().strip()+':'+(p/'device').read_text().strip())
PY
apk add --no-cache --repository https://dl-cdn.alpinelinux.org/alpine/v3.24/main --repository https://dl-cdn.alpinelinux.org/alpine/v3.24/community flashrom
flashrom --version
flashrom --help
