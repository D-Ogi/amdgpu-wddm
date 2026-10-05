#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>
int wmain(int argc,wchar_t **argv){
 if(argc!=3)return 2;
 DWORD pid=wcstoul(argv[1],nullptr,10);if(!pid)return 3;
 HANDLE token=nullptr;
 if(OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token)){
  TOKEN_PRIVILEGES p={};p.PrivilegeCount=1;LookupPrivilegeValueW(nullptr,SE_DEBUG_NAME,&p.Privileges[0].Luid);p.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;AdjustTokenPrivileges(token,FALSE,&p,sizeof(p),nullptr,nullptr);CloseHandle(token);
 }
 HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_DUP_HANDLE,FALSE,pid);
 if(!process){printf("OpenProcess error=%lu\n",GetLastError());return 4;}
 HANDLE file=CreateFileW(argv[2],GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE){printf("CreateFile error=%lu\n",GetLastError());CloseHandle(process);return 5;}
 BOOL ok=MiniDumpWriteDump(process,pid,file,(MINIDUMP_TYPE)(MiniDumpWithThreadInfo|MiniDumpWithProcessThreadData|MiniDumpWithUnloadedModules),nullptr,nullptr,nullptr);
 DWORD error=ok?0:GetLastError();CloseHandle(file);CloseHandle(process);printf("MiniDump pid=%lu ok=%u error=%lu\n",pid,ok,error);return ok?0:6;
}