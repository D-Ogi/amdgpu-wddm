#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tgsi/tgsi_text.h"
#include "pipe/p_shader_tokens.h"
#include "nir/tgsi_to_nir.h"
#include "compiler/nir/nir.h"
#include "compiler/glsl_types.h"
#include "util/ralloc.h"
int main(void) {
 _set_error_mode(_OUT_TO_STDERR);
 const char *ops[]={"SAMPLE","SAMPLE_L","SAMPLE_B","SAMPLE_D","SAMPLE_C","SAMPLE_C_LZ","SAMPLE_I","SVIEWINFO"};
 const char *args[]={"IN[0], SVIEW[1], SAMP[0]","IN[0], SVIEW[1], SAMP[0], IMM[0]","IN[0], SVIEW[1], SAMP[0], IMM[0]","IN[0], SVIEW[1], SAMP[0], IMM[0], IMM[0]","IN[0], SVIEW[1], SAMP[0], IMM[0]","IN[0], SVIEW[1], SAMP[0], IMM[0]","IMM[1], SVIEW[1]","IMM[1], SVIEW[1]"};
 glsl_type_singleton_init_or_ref();
 unsigned failed=0;
 for(unsigned i=0;i<8;i++) {
  char text[1024];struct tgsi_token tokens[512];
  snprintf(text,sizeof text,"FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL SAMP[0]\nDCL SVIEW[1], 2D, FLOAT\nIMM[0] FLT32 {0.25,0.25,0.0,0.0}\nIMM[1] UINT32 {0,0,0,0}\n%s OUT[0], %s\nEND\n",ops[i],args[i]);
  if(!tgsi_text_translate(text,tokens,512)){printf("parse fail %s\n",ops[i]);failed++;continue;}
  nir_shader_compiler_options options={0};
  nir_shader *s=tgsi_to_nir_noscreen(tokens,&options);
  nir_validate_shader(s,"BC250 separate bindings control");
  unsigned texcount=0;bool okay=true;
  nir_foreach_function_impl(impl,s) nir_foreach_block(block,impl) nir_foreach_instr(ins,block) {
   if(ins->type!=nir_instr_type_tex)continue;
   nir_tex_instr *t=nir_instr_as_tex(ins);texcount++;
   int ts=nir_tex_instr_src_index(t,nir_tex_src_texture_deref);
   int ss=nir_tex_instr_src_index(t,nir_tex_src_sampler_deref);
   if(ts<0 || nir_deref_instr_get_variable(nir_src_as_deref(t->src[ts].src))->data.binding!=1)okay=false;
   if(i<6 && (ss<0 || nir_deref_instr_get_variable(nir_src_as_deref(t->src[ss].src))->data.binding!=0))okay=false;
   if((i==4 || i==5) && !t->is_shadow)okay=false;
  }
  if(texcount!=(i==7?2:1))okay=false;
  printf("%s texture=1 sampler=0 nodes=%u %s\n",ops[i],texcount,okay?"PASS":"FAIL");failed+=!okay;ralloc_free(s);
 }
 glsl_type_singleton_decref();return failed?1:0;
}
