// Host-only negative control. Intentionally leak receipt pipes to a finite child.
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
int wmain(int argc,wchar_t** argv){
    if(argc==2 && std::wstring(argv[1])==L"--help")return 0;
    if(argc==2 && std::wstring(argv[1])==L"--holder"){Sleep(10000);return 0;}
    if(argc<5)return 125;
    wchar_t path[MAX_PATH]={};
    if(!GetModuleFileNameW(nullptr,path,MAX_PATH))return 125;
    std::wstring command=L"\""+std::wstring(path)+L"\" --holder";
    std::vector<wchar_t> buffer(command.begin(),command.end());buffer.push_back(0);
    STARTUPINFOW si={};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi={};
    if(!CreateProcessW(path,buffer.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))return 125;
    HANDLE file=CreateFileW(argv[2],GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){TerminateProcess(pi.hProcess,125);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return 125;}
    char pid[32];int size=sprintf_s(pid,"%lu",pi.dwProcessId);DWORD written=0;
    const bool saved=WriteFile(file,pid,static_cast<DWORD>(size),&written,nullptr)!=FALSE;
    CloseHandle(file);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    if(!saved)return 125;
    puts("{\"job_empty\":false}");
    return 125;
}
