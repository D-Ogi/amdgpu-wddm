from pathlib import Path
import re,struct,hashlib,json
h=Path('ref/linux-src/drivers/gpu/drm/amd/include/atomfirmware.h').read_text()
body=h.split('struct atom_rom_header_v2_2',1)[1].split('{',1)[1].split('};',1)[0]
offset=4; offsets={}
for bits,name,count in re.findall(r'uint(8|16|32)_t\s+(\w+)(?:\[(\d+)\])?\s*;',body):
 offsets[name]=offset;offset+=int(bits)//8*(int(count) if count else 1)
body=h.split('struct atom_master_list_of_data_tables_v2_1',1)[1].split('{',1)[1].split('};',1)[0]
fields=re.findall(r'uint16_t\s+(\w+)\s*;',body)
index=fields.index('vram_usagebyfirmware')
# ATOM_ROM_TABLE_PTR from the original AMD atom.h; this is a ROM byte offset, not MMIO.
ah=Path('ref/linux-src/drivers/gpu/drm/amd/amdgpu/atom.h').read_text()
romptr=int(re.search(r'#define\s+ATOM_ROM_TABLE_PTR\s+(0x[0-9A-Fa-f]+)',ah)[1],16)
results=[]
for path in [Path('firmware/unit-A-2026-09-21/vbios-debugfs.rom'),Path('firmware/bios/analysis/extracted/unitA-VFCT/VBIOS_from_unitA_VFCT_1002-13FE_runtime-patched.rom'),Path('firmware/bios/analysis/extracted/P3/A0327FE0_sec2_VBIOS_113-AMDRBN-003_1002-13FE_sectF1004391.rom')]:
 data=path.read_bytes();u16=lambda x:struct.unpack_from('<H',data,x)[0]
 rom=u16(romptr);assert data[rom+4:rom+8] in [b'ATOM',b'MOTA']
 master=u16(rom+offsets['masterdatatable_offset']);entry=u16(master+4+2*index)
 size,frev,crev,start,fw,drv=struct.unpack_from('<HBBIHH',data,entry)
 assert size>=12 and (frev,crev)==(2,1)
 results.append(dict(path=str(path),sha256=hashlib.sha256(data).hexdigest(),rom_header=rom,master_data=master,usage_table=entry,revision=f'{frev}.{crev}',start_kib=start,firmware_kib=fw,driver_kib=drv))
print(json.dumps(dict(derived_header_field_offset=offsets['masterdatatable_offset'],derived_table_index=index,images=results),indent=2))
