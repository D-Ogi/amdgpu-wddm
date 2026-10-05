#include "P:/bc-250/scratch/mesa-wddm2/src/gallium/auxiliary/tgsi/tgsi_exec.c"
#include "P:/bc-250/scratch/mesa-wddm2/src/gallium/frontends/d3d10umd/ShaderParse.h"
#include <stdio.h>
#include <stdarg.h>
const struct tgsi_token *Shader_tgsi_translate(const unsigned *, unsigned *);
void DebugPrintf(const char *format, ...) { (void)format; }
void AssertFail(const char *e,const char *f,unsigned l,const char *fn) {
 fprintf(stderr,"ASSERT %s %s:%u %s\n",e,f,l,fn); exit(9);
}
#define OP(x) (D3D10_SB_OPCODE_##x | (1u << 24))
#define COND(x) (D3D10_SB_OPCODE_##x | (3u << 24) | (1u << 18))
#define IMM (D3D10_SB_OPERAND_TYPE_IMMEDIATE32 << 12 | D3D10_SB_OPERAND_1_COMPONENT)
static int run(const char *name, unsigned *code, unsigned count) {
 unsigned mapping[64]={0}; code[1]=count;
 const struct tgsi_token *tokens=Shader_tgsi_translate(code,mapping);
 struct tgsi_exec_machine *m=tgsi_exec_machine_create(MESA_SHADER_VERTEX);
 tgsi_exec_machine_bind_shader(m,tokens,NULL,NULL,NULL);
 tgsi_exec_machine_setup_masks(m); m->pc=0;
 unsigned steps=0;
 while(m->pc!=-1 && steps++<256) {
  if(m->LoopStackTop>=31 || m->CondStackTop>=31) break;
  exec_instruction(m,m->Instructions+m->pc,&m->pc);
 }
 int ok=m->pc==-1 && !m->LoopStackTop && !m->CondStackTop;
 printf("%s: %s steps=%u pc=%d loops=%d conditions=%d\n",name,ok?"PASS":"FAIL",steps,m->pc,m->LoopStackTop,m->CondStackTop);
 tgsi_exec_machine_destroy(m); free((void*)tokens); return !ok;
}
int main(void) {
 _set_error_mode(_OUT_TO_STDERR);
 unsigned a[]={0x10040,0,OP(LOOP),COND(IF),IMM,0,OP(BREAK),OP(ENDIF),OP(BREAK),OP(ENDLOOP),OP(RET)};
 unsigned b[]={0x10040,0,OP(LOOP),COND(IF),IMM,1,OP(BREAK),OP(ELSE),OP(BREAK),OP(ENDIF),OP(ENDLOOP),OP(RET)};
 unsigned c[]={0x10040,0,OP(LOOP),COND(BREAKC),IMM,0,COND(BREAKC),IMM,1,OP(ENDLOOP),OP(RET)};
 unsigned d[]={0x10040,0,OP(LOOP),COND(IF),IMM,0,OP(BREAK),OP(ELSE),COND(IF),IMM,1,OP(BREAK),OP(ENDIF),OP(ENDIF),OP(ENDLOOP),OP(RET)};
 unsigned e[]={0x10040,0,OP(LOOP),COND(CONTINUEC),IMM,0,COND(RETC),IMM,0,OP(BREAK),OP(ENDLOOP),OP(RET)};
 int failures=run("false IF inside LOOP",a,sizeof(a)/4);
 failures+=run("IF/ELSE inside LOOP",b,sizeof(b)/4);
 failures+=run("conditional BREAK false then true",c,sizeof(c)/4);
 failures+=run("nested IF in ELSE",d,sizeof(d)/4);
 failures+=run("false conditional CONTINUE and RETURN",e,sizeof(e)/4);
 b[5]=0;
 failures+=run("ELSE taken",b,sizeof(b)/4);
 a[3]&=~(1u<<18);
 failures+=run("IF test-zero",a,sizeof(a)/4);
 return failures?1:0;
}

