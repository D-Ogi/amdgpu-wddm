set -eu
[ ! -d /sys/module/amdgpu ] || exit 2
python3 -u - <<'PY'
from pathlib import Path
import sys,json,struct,time,threading,hashlib
sys.path.insert(0,'/media/usb/bc250')
import diag
REGS={'mmTHM_TCON_CUR_TMP': 366592, 'mmGRBM_STATUS': 32784, 'mmGB_ADDR_CONFIG': 39160, 'mmRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE': 14220}
g=diag.find_gpu();assert g and diag.read_text(g+'/vendor')=='0x1002' and diag.read_text(g+'/device')=='0x13fe'
assert not Path(g+'/driver').exists()
with open(g+'/config','rb') as f:cfg=f.read(64)
assert struct.unpack_from('<H',cfg,4)[0]&2
bar=diag.Bar(g,5,readonly=True);assert bar.size==0x80000
print('read_only_bar5_mapped=true',flush=True)
for name in ['mmRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE','mmGB_ADDR_CONFIG','mmGRBM_STATUS']:
 value=bar.r32(REGS[name]);print(json.dumps({'register':name,'byte_offset':REGS[name],'raw':value}),flush=True)
 if name=='mmRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE':assert value==8192
paths=[p/'temp1_input' for p in Path('/sys/class/hwmon').glob('hwmon*') if (p/'name').read_text().strip()=='k10temp']
assert len(paths)==1
stop=threading.Event()
def cpu_load():
 data=b'x'*(512*1024);end=time.monotonic()+6
 while not stop.is_set() and time.monotonic()<end:hashlib.sha256(data).digest()
threads=[threading.Thread(target=cpu_load,daemon=True) for _ in range(2)]
for t in threads:t.start()
for i in range(60):
 before=int(paths[0].read_text());assert before<85000
 raw=bar.r32(REGS['mmTHM_TCON_CUR_TMP'])
 decoded=((raw>>21)&0x7ff)*125-(49000 if raw&0x80000 else 0)
 after=int(paths[0].read_text());assert after<85000
 print(json.dumps({'sample':i,'monotonic_ns':time.monotonic_ns(),'utc_epoch':time.time(),'smn_k10temp_before_mc':before,'bar_thm_raw':raw,'bar_thm_decoded_mc':decoded,'smn_k10temp_after_mc':after}),flush=True)
 if raw in (0,0xffffffff):
  print('BAR_thermal_unusable_in_this_state=true',flush=True);break
 time.sleep(0.25)
stop.set()
for t in threads:t.join()
print('thermal_comparison_complete',flush=True)
PY
