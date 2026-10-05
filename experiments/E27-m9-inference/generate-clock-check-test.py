from pathlib import Path
import sys
r=Path(sys.argv[1]);out=Path(sys.argv[2])
s=(r/'tools/win/bc250rd/app/bc250rd_cli.c').read_text()
a=s.index('static int CheckClock(');b=s.index('// THM_TCON_CUR_TMP:',a)
code=r"""#include <stdio.h>
typedef void* HANDLE;
typedef unsigned long ULONG;
#include "bc250rd_ioctl.h"
static unsigned calls,failCall;
static ULONG measuredMHz=1000,measuredVid=116;
static int orderBad;
static int Smu(HANDLE h,const char*n,ULONG msg,ULONG param,BC250RD_SMU_MSG*out){
 (void)h;(void)n;calls++;
 if(param || msg!=(ULONG)(calls==1?BC250RD_SMU_GetGfxFrequency:BC250RD_SMU_GetGfxVid))orderBad++;
 out->Value=calls==1?measuredMHz:measuredVid;
 return calls!=failCall;
}
"""
code+=s[a:b]
if '--omit-compare' in sys.argv:
 code=code.replace('frequency.Value!=mhz || voltage.Value!=BC250RD_VID_FROM_MV(mv)','frequency.Value!=frequency.Value')
code+=r"""
int main(void){
 unsigned i,failures=0;
 for(i=0;i<10;i++){
  ULONG mhz=1000,mv=820;int got,expected=i==0||i==9;
  calls=failCall=0;measuredMHz=1000;measuredVid=116;
  switch(i){case 1:failCall=1;break;case 2:failCall=2;break;
  case 3:measuredMHz=1500;break;case 4:measuredVid=101;break;
  case 5:mhz=999;break;case 6:mhz=2001;break;case 7:mv=699;break;case 8:mv=1130;break;
  case 9:mhz=1500;mv=900;measuredMHz=1500;measuredVid=104;break;}
  got=CheckClock(NULL,mhz,mv);
  if(got!=expected || orderBad || calls!=(i>=5&&i<=8?0u:i==1?1u:2u))failures++;
 }
 printf("10 scenarios, %u failures\n",failures);return failures?1:0;
}
"""
out.write_text(code)
