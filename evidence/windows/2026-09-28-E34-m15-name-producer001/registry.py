from pathlib import Path
import struct,math,json,pefile,capstone,hashlib,bisect
root=Path('P:/bc-250');out=root/'scratch/m15/name-selection001';out.mkdir(exist_ok=True)
pdb=root/'scratch/symbols/dxgkrnl.pdb/907FD0B67E572F54163D06DFD05487A71/dxgkrnl.pdb'
pepath=root/'scratch/target-binaries/22631/dxgkrnl.sys'
data=pdb.read_bytes();bs,_,_,ds,_,bm=struct.unpack_from('<6I',data,32)
blocks=struct.unpack_from('<'+'I'*math.ceil(ds/bs),data,bm*bs)
directory=b''.join(data[i*bs:(i+1)*bs] for i in blocks)[:ds]
n=struct.unpack_from('<I',directory)[0];sizes=struct.unpack_from('<'+'I'*n,directory,4);pos=4+4*n;streams=[]
for size in sizes:
 count=0 if size==0xffffffff else math.ceil(size/bs)
 blocks=struct.unpack_from('<'+'I'*count,directory,pos);pos+=4*count
 streams.append(b''.join(data[i*bs:(i+1)*bs] for i in blocks)[:size])
pe=pefile.PE(str(pepath));cv=[e for e in pe.DIRECTORY_ENTRY_DEBUG if e.struct.Type==2][0]
cvdata=pe.get_data(cv.struct.AddressOfRawData,cv.struct.SizeOfData)
assert cvdata[:4]==b'RSDS' and cvdata[4:20]==streams[1][12:28]
print('ages PE/PDB-info/DBI',struct.unpack_from('<I',cvdata,20)[0],struct.unpack_from('<I',streams[1],8)[0],struct.unpack_from('<I',streams[3],8)[0]); assert struct.unpack_from('<I',cvdata,20)[0]==struct.unpack_from('<I',streams[3],8)[0]
si=struct.unpack_from('<H',streams[3],20)[0];s=streams[si];pos=0;symbols=[]
while pos+4<=len(s):
 length,kind=struct.unpack_from('<HH',s,pos)
 if length<2:break
 record=s[pos:pos+length+2]
 if kind==0x110e:
  _,offset,segment=struct.unpack_from('<IIH',record,4)
  name=record[14:].split(b'\0')[0].decode('utf-8','replace')
  if 0<segment<=len(pe.sections):symbols.append((pe.sections[segment-1].VirtualAddress+offset,name))
 pos+=length+2
symbols.sort();addresses=[x[0] for x in symbols]
(out/'symbols.json').write_text(json.dumps(symbols))
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
def listing(needle):
 selected=[(a,n) for a,n in symbols if needle in n]
 lines=[]
 for address,name in selected:
  idx=bisect.bisect_right(addresses,address);end=min(address+0x1800,addresses[idx] if idx<len(addresses) else address+0x1800)
  unwind=[e.struct.EndAddress for e in pe.DIRECTORY_ENTRY_EXCEPTION if e.struct.BeginAddress==address]
  if unwind:end=min(end,unwind[0])
  lines.append(f'FUNCTION RVA {address:x} {name}')
  for ins in md.disasm(pe.get_data(address,end-address),address):
   annotation=''
   if ins.mnemonic in ['call','jmp'] and ins.op_str.startswith('0x'):
    target=int(ins.op_str,16);k=bisect.bisect_right(addresses,target)-1
    if k>=0:annotation=' ; '+symbols[k][1]+f'+{target-addresses[k]:x}'
   lines.append(f'{ins.address:08x} {ins.bytes.hex():24} {ins.mnemonic:8} {ins.op_str}{annotation}')
 return '\n'.join(lines)+'\n'


for a,n in symbols:
 if any(t.lower() in n.lower() for t in ['UmdDriver','UserModeDriver','QueryAdapterInfo']):print(hex(a),n)

for needle,filename in [('?InitializeDisplayUserModeDriverNames@','init-display.txt'),('?InitializeUserModeDriverNames@','init-names.txt'),('?DxgkQueryAdapterInfoImpl@@','query.txt'),('?PostProcessUMDFileName@@','postprocess.txt')]:
 (out/filename).write_text(listing(needle))


needles=['UserModeDriverName','UserModeDriverNameWoW']
strings={}
for needle in needles:
 data=needle.encode('utf-16le')+b'\0\0';pos=0
 while True:
  pos=pe.__data__.find(data,pos)
  if pos<0:break
  strings[pe.get_rva_from_offset(pos)]=needle;pos+=len(data)
md.detail=True;lines=[]
for section in pe.sections:
 if not section.Characteristics & 0x20000000:continue
 md.skipdata=True
 for ins in md.disasm(section.get_data(),section.VirtualAddress):
  if not ins.id:continue
  for op in ins.operands:
   if op.type==capstone.CS_OP_MEM and op.mem.base==capstone.x86.X86_REG_RIP:
    target=ins.address+ins.size+op.mem.disp
    if target in strings:
     k=bisect.bisect_right(addresses,ins.address)-1
     lines.append(f'{ins.address:x}: {ins.mnemonic} {ins.op_str}; {strings[target]} at {target:x}; {symbols[k]}')
(out/'registry-xrefs.txt').write_text('\n'.join(lines))
print('\n'.join(lines))

(out/'get-adapter-info.txt').write_text(listing('DpiGetAdapterInfo'))

targets={a:n for a,n in symbols if n=='DpiGetAdapterInfo'}
lines=[]
for section in pe.sections:
 if not section.Characteristics & 0x20000000:continue
 raw=section.get_data();base=section.VirtualAddress
 for i in range(len(raw)-5):
  if raw[i]!=0xe8:continue
  target=base+i+5+struct.unpack_from('<i',raw,i+1)[0]
  if target not in targets:continue
  k=bisect.bisect_right(addresses,base+i)-1
  lines.append(f'{base+i:x} -> {target:x}; {symbols[k]}')
(out/'producer-call-candidates.txt').write_text('\n'.join(lines));print('\n'.join(lines))
(out/'retrieve-string.txt').write_text(listing('DxgkRetrieveStringFromRegistry'))
