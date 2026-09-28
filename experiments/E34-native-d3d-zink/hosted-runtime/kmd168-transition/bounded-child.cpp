// Own the complete child tree before its first instruction. No lab-specific data.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
struct Handle {
    HANDLE h=nullptr;
    ~Handle(){if(h && h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
};
#include "active-console-child.h"
static long long now(){LARGE_INTEGER v;QueryPerformanceCounter(&v);return v.QuadPart;}
static std::wstring quote(const wchar_t* arg){
    std::wstring out=L"\"";unsigned slash=0;
    for(const wchar_t* p=arg;*p;++p){
        if(*p==L'\\'){++slash;continue;}
        out.append(slash*(*p==L'"'?2:1),L'\\');slash=0;
        if(*p==L'"')out+=L'\\';out+=*p;
    }
    out.append(slash*2,L'\\');out+=L'"';return out;
}
int wmain(int argc,wchar_t** argv){
    if(argc==2 && std::wstring(argv[1])==L"--help"){
        puts("bounded-child [--active-console] DEADLINE_QPC STDOUT STDERR EXE [ARGS...]; returns0 only for an empty job and child exit0; console child writes its own logs");return 0;
    }
    const bool interactive=argc>1 && std::wstring(argv[1])==L"--active-console";
    if(interactive){--argc;++argv;}
    if(argc<5)return 125;
    wchar_t* end=nullptr;const long long deadline=_wcstoi64(argv[1],&end,10);
    LARGE_INTEGER freq;QueryPerformanceFrequency(&freq);
    if(!end || *end || deadline-now()<=freq.QuadPart)return 125;
    // Reserve the final second of the absolute budget for tree termination.
    const long long workEnd=deadline-freq.QuadPart;
    Handle job;job.h=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job.h || !SetInformationJobObject(job.h,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))return 125;
    SECURITY_ATTRIBUTES sa={sizeof(sa),nullptr,TRUE};
    Handle output,error,input;
    output.h=CreateFileW(argv[2],GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    error.h=CreateFileW(argv[3],GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    input.h=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
    if(output.h==INVALID_HANDLE_VALUE || error.h==INVALID_HANDLE_VALUE || input.h==INVALID_HANDLE_VALUE)return 125;
    std::wstring command;
    for(int i=4;i<argc;++i){if(i>4)command+=L' ';command+=quote(argv[i]);}
    std::vector<wchar_t> mutableCommand(command.begin(),command.end());mutableCommand.push_back(0);
    // Do not leak the helper's receipt pipes into any child or descendant.
    // Their survival must never make the supervisor wait indefinitely for EOF.
    STARTUPINFOEXW startup={};startup.StartupInfo.cb=sizeof(startup);
    startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput=input.h;startup.StartupInfo.hStdOutput=output.h;
    startup.StartupInfo.hStdError=error.h;
    SIZE_T bytes=0;
    InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
    if(!bytes)return 125;
    std::vector<unsigned char> attributes(bytes);
    startup.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if(!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&bytes))return 125;
    HANDLE inherited[]={input.h,output.h,error.h};
    if(!UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                 inherited,sizeof(inherited),nullptr,nullptr)){
        DeleteProcThreadAttributeList(startup.lpAttributeList);return 125;
    }
    PROCESS_INFORMATION pi={};
    DWORD selectedSession=0;
    const BOOL created=interactive ? createActiveConsoleChild(argv[4],mutableCommand.data(),&pi,selectedSession) :
        CreateProcessW(argv[4],mutableCommand.data(),nullptr,nullptr,TRUE,
        CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,
        nullptr,nullptr,&startup.StartupInfo,&pi);
    const DWORD creationError=created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if(!created){std::printf("{\"launch_error\":%lu}\n",creationError);return 125;}
    Handle process,thread;process.h=pi.hProcess;thread.h=pi.hThread;
    if(!AssignProcessToJobObject(job.h,process.h)){
        TerminateProcess(process.h,125);return 125;
    }
    DWORD childSession=0;
    if(interactive && (!ProcessIdToSessionId(pi.dwProcessId,&childSession) || childSession!=selectedSession ||
        WTSGetActiveConsoleSessionId()!=selectedSession)){TerminateJobObject(job.h,125);return 125;}
    if(now()>=workEnd || ResumeThread(thread.h)==DWORD(-1)){TerminateJobObject(job.h,125);return 125;}
    bool observed=false,timedOut=false,empty=false;DWORD childExit=STILL_ACTIVE;
    for(;;){
        if(WaitForSingleObject(process.h,0)==WAIT_OBJECT_0){observed=true;GetExitCodeProcess(process.h,&childExit);break;}
        if(now()>=workEnd){timedOut=true;break;}
        Sleep(10);
    }
    // Even a successful root process cannot leave its descendants resident.
    const bool terminated=TerminateJobObject(job.h,timedOut?124:125)!=FALSE;
    do{
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info={};
        if(QueryInformationJobObject(job.h,JobObjectBasicAccountingInformation,&info,sizeof(info),nullptr))empty=info.ActiveProcesses==0;
        if(empty || now()>=deadline)break;
        Sleep(10);
    }while(true);
    printf("{\"console_session\":%lu,\"child_pid\":%lu,\"root_exit_observed\":%s,\"child_exit\":%lu,\"timed_out\":%s,\"termination_requested\":%s,\"job_empty\":%s}\n",
        selectedSession,pi.dwProcessId,observed?"true":"false",childExit,timedOut?"true":"false",terminated?"true":"false",empty?"true":"false");
    if(!empty)return 125;
    if(timedOut)return 124;
    return observed && childExit==0?0:126;
}
