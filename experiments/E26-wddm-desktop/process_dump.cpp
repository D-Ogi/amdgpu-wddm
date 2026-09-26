#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>
#pragma comment(lib,"dbghelp.lib")
#pragma comment(lib,"advapi32.lib")
int wmain(int argc,wchar_t** argv) {
 if(argc!=3)return 2;
 HANDLE token=nullptr;
 if(OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token)) {
  TOKEN_PRIVILEGES p={};p.PrivilegeCount=1;
  if(LookupPrivilegeValueW(nullptr,L"SeDebugPrivilege",&p.Privileges[0].Luid)) {
   p.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
   AdjustTokenPrivileges(token,FALSE,&p,sizeof(p),nullptr,nullptr);
  }
  CloseHandle(token);
 }
 DWORD pid=wcstoul(argv[1],nullptr,10);
 HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);
 if(!process){printf("OpenProcess error %lu\n",GetLastError());return 3;}
 HANDLE file=CreateFileW(argv[2],GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE){printf("CreateFile error %lu\n",GetLastError());CloseHandle(process);return 4;}
 BOOL ok=MiniDumpWriteDump(process,pid,file,MINIDUMP_TYPE(MiniDumpWithThreadInfo|MiniDumpWithIndirectlyReferencedMemory),nullptr,nullptr,nullptr);
 DWORD err=ok?0:GetLastError();FlushFileBuffers(file);CloseHandle(file);CloseHandle(process);
 printf("MiniDumpWriteDump pid %lu success %u error %lu\n",pid,ok,err);return ok?0:5;
}

