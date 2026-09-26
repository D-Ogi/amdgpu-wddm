#include "tgsi/tgsi_exec.c"
#include <stdio.h>
int main(void) {
 struct tgsi_exec_machine *m = calloc(1, sizeof(*m));
 if (!m) return 2;
 struct tgsi_full_src_register r = {0};
 union tgsi_exec_channel index, dim;
 m->ExecMask = 5;
 r.Register.File = TGSI_FILE_CONSTANT; r.Register.Index = 7; r.Register.Indirect = 1;
 r.Indirect.File = TGSI_FILE_TEMPORARY; r.Indirect.Index = 2; r.Indirect.Swizzle = 1;
 for(int i=0;i<4;i++)m->Temps[2].xyzw[1].u[i]=i;
 get_index_registers(m,&r,&index,&dim);
 if(index.i[0]!=7 || index.i[1]!=0 || index.i[2]!=9 || index.i[3]!=0)return 3;
 r.Register.Dimension=1;r.Dimension.Index=1;r.Dimension.Indirect=1;
 r.DimIndirect.File=TGSI_FILE_TEMPORARY;r.DimIndirect.Index=3;r.DimIndirect.Swizzle=2;
 for(int i=0;i<4;i++)m->Temps[3].xyzw[2].u[i]=i;
 get_index_registers(m,&r,&index,&dim);
 if(dim.i[0]!=1 || dim.i[1]!=0 || dim.i[2]!=3 || dim.i[3]!=0)return 4;
 r.Register.Dimension=0;r.Indirect.File=TGSI_FILE_ADDRESS;r.Indirect.Index=0;
 for(int i=0;i<4;i++)m->Addrs[0].xyzw[1].u[i]=i+4;
 get_index_registers(m,&r,&index,&dim);
 if(index.i[0]!=11 || index.i[1]!=0 || index.i[2]!=13 || index.i[3]!=0)return 5;
 free(m);puts("PASS: TEMP source index, TEMP dimension index, ADDR control, inactive lanes");return 0;
}
